/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoFrame.h>

#include "FFmpegVideoDecoder.h"
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>
#include <LibMediaCodecs/Limits.h>

namespace Media::FFmpeg {

Optional<DecoderCapabilities> FFmpegVideoDecoder::capabilities(FFmpegFunctions const& functions, ParsedCodec const& codec)
{
    if (track_type_from_codec_id(codec.codec_id()) != TrackType::Video)
        return {};
    if (!functions.avcodec_find_decoder(functions.ffmpeg_codec_id(codec.codec_id())))
        return {};
    return DecoderCapabilities { .smooth = true, .power_efficient = false };
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(FFmpegFunctions const& functions, CodecID codec_id, ReadonlyBytes codec_initialization_data, VideoDecoderOptions const& options)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            functions.avcodec_free_context(&codec_context);
            functions.av_packet_free(&packet);
            functions.av_frame_free(&frame);
        }
    };

    auto const* codec = functions.avcodec_find_decoder(functions.ffmpeg_codec_id(codec_id));
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    codec_context = functions.avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    TRY(set_codec_initialization_data(functions, *codec_context, codec_id, codec_initialization_data));
    VERIFY(functions.av_opt_set_int(codec_context, "threads", min(Core::System::hardware_concurrency(), 4u), 0) == 0);
    // Frame threading holds back one frame per extra thread, so only split each frame across threads when latency matters.
    // Decoders that thread internally, such as libdav1d, ignore the thread type and hold frames back unless low delay is set.
    // NB: These are set as options, as the layout of AVCodecContext differs between the libavcodec versions that may be loaded.
    if (options.optimize_for_latency) {
        VERIFY(functions.av_opt_set_int(codec_context, "thread_type", FF_THREAD_SLICE, 0) == 0);
        int64_t flags = 0;
        VERIFY(functions.av_opt_get_int(codec_context, "flags", 0, &flags) == 0);
        VERIFY(functions.av_opt_set_int(codec_context, "flags", flags | AV_CODEC_FLAG_LOW_DELAY, 0) == 0);
    }
    codec_context->max_pixels = MAX_VIDEO_FRAME_AREA;

    if (functions.avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = functions.av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = functions.av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    auto frame_pool_result = VideoFramePool::create();
    if (frame_pool_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to create a video frame pool: {}", frame_pool_result.release_error());

    memory_guard.disarm();
    return DECODER_TRY_ALLOC(try_make<FFmpegVideoDecoder>(functions, codec_id, codec_initialization_data, codec_context, packet, frame, frame_pool_result.release_value()));
}

Optional<DecoderCapabilities> FFmpegVideoDecoder::capabilities(ParsedCodec const& codec)
{
    return capabilities(FFmpegFunctions::bundled(), codec);
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data, VideoDecoderOptions const& options)
{
    return try_create(FFmpegFunctions::bundled(), codec_id, codec_initialization_data, options);
}

FFmpegVideoDecoder::FFmpegVideoDecoder(FFmpegFunctions const& functions, CodecID codec_id, ReadonlyBytes codec_initialization_data, AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, NonnullRefPtr<VideoFramePool> frame_pool)
    : m_functions(functions)
    , m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
    , m_frame_pool(move(frame_pool))
    , m_frames_awaiting_output(codec_id, codec_initialization_data)
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    m_frame_pool->shed_storage();
    m_functions.av_packet_free(&m_packet);
    m_functions.av_frame_free(&m_frame);
    m_functions.avcodec_free_context(&m_codec_context);
    m_functions.av_frame_free(&m_alpha_frame);
    m_functions.avcodec_free_context(&m_alpha_codec_context);
}

DecoderErrorOr<void> FFmpegVideoDecoder::receive_coded_data(CodedFrame const& coded_frame, DecodeIntent intent)
{
    auto key = TRY(m_frames_awaiting_output.add(coded_frame, intent));
    TRY(send_coded_frame(m_functions, *m_codec_context, *m_packet, coded_frame, key));
    if (!coded_frame.alpha_data().is_empty())
        TRY(decode_alpha_plane(coded_frame.alpha_data(), key, intent));
    return {};
}

DecoderErrorOr<void> FFmpegVideoDecoder::decode_alpha_plane(ReadonlyBytes data, u64 key, DecodeIntent intent)
{
    if (data.size() >= NumericLimits<int>::max())
        return DecoderError::corrupted("VP9 alpha packet is too large"sv);

    if (!m_alpha_codec_context) {
        auto* context = m_functions.avcodec_alloc_context3(m_codec_context->codec);
        auto* frame = m_functions.av_frame_alloc();
        ArmedScopeGuard free_resources { [&] {
            m_functions.avcodec_free_context(&context);
            m_functions.av_frame_free(&frame);
        } };
        if (!context || !frame)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a VP9 alpha decoder"sv);

        // Matroska stores alpha as a separate VP9 bitstream, with its samples paired by block.
        context->thread_count = 1;
        context->max_pixels = MAX_VIDEO_FRAME_AREA;
        if (m_functions.avcodec_open2(context, m_codec_context->codec, nullptr) < 0)
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "Failed to open the VP9 alpha decoder"sv);
        free_resources.disarm();
        m_alpha_codec_context = context;
        m_alpha_frame = frame;
    }

    auto* packet = m_functions.av_packet_alloc();
    ScopeGuard free_packet { [&] { m_functions.av_packet_free(&packet); } };
    if (!packet || m_functions.av_new_packet(packet, static_cast<int>(data.size())) < 0)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate a VP9 alpha packet"sv);
    data.copy_to({ packet->data, data.size() });
    packet->pts = static_cast<i64>(key);
    auto result = m_functions.avcodec_send_packet(m_alpha_codec_context, packet);
    if (result < 0)
        return DecoderError::corrupted("Failed to submit a VP9 alpha packet"sv);

    result = m_functions.avcodec_receive_frame(m_alpha_codec_context, m_alpha_frame);
    if (result == AVERROR(EAGAIN))
        return {};
    if (result < 0)
        return DecoderError::corrupted("Failed to decode the VP9 alpha plane"sv);

    ScopeGuard unref_frame { [&] { m_functions.av_frame_unref(m_alpha_frame); } };
    if (intent == DecodeIntent::Reference)
        return {};

    auto const& alpha = *m_alpha_frame;
    if (alpha.pts != packet->pts || !alpha.data[0] || alpha.width < 1 || alpha.height < 1 || alpha.linesize[0] < alpha.width
        || video_frame_exceeds_limits(alpha.width, alpha.height))
        return DecoderError::corrupted("Invalid decoded VP9 alpha plane"sv);
    switch (alpha.format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
        break;
    default:
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Unsupported VP9 alpha pixel format"sv);
    }

    return m_frames_awaiting_output.store_alpha_plane(key, { alpha.width, alpha.height }, alpha.data[0], static_cast<size_t>(alpha.linesize[0]));
}

void FFmpegVideoDecoder::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = m_functions.avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

DecoderErrorOr<NonnullRefPtr<VideoFrame>> FFmpegVideoDecoder::take_next_output(ColorMetadata const& container_color_metadata, [[maybe_unused]] Optional<AK::Duration> target)
{
    while (!m_pending_frame.has_value()) {
        auto result = m_functions.avcodec_receive_frame(m_codec_context, m_frame);

        switch (result) {
        case 0: {
            auto frame = m_frames_awaiting_output.take(m_frame->pts);
            if (!frame.has_value()) {
                dbgln("FFmpegVideoDecoder: Dropping a frame whose timing was forgotten");
                continue;
            }
            if (frame->intent == DecodeIntent::Reference)
                continue;
            m_pending_frame = frame.release_value();
            break;
        }
        case AVERROR(EAGAIN):
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder has no frames available, send more input"sv);
        case AVERROR_EOF:
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
        case AVERROR(EINVAL):
            return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
        default:
            return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
        }
    }

    auto color_metadata = container_color_metadata;
    color_metadata.adopt_specified_values(m_pending_frame->high_dynamic_range_metadata);
    color_metadata.cicp = coding_independent_code_points_of(m_functions, *m_codec_context, container_color_metadata.cicp);

    auto pixel_format = static_cast<AVPixelFormat>(m_frame->format);
    auto bit_depth = [&]() -> Optional<u8> {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
            return 8;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV444P10:
            return 10;
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV444P12:
            return 12;
        default:
            return {};
        }
    }();
    if (!bit_depth.has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "FFmpeg decoder produced an unsupported pixel format {}", to_underlying(pixel_format));

    auto subsampling = [&] {
        switch (pixel_format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUVJ420P:
            return Subsampling::yuv420();
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUVJ422P:
            return Subsampling::yuv422();
        default:
            return Subsampling::yuv444();
        }
    }();

    Gfx::IntSize size { m_frame->width, m_frame->height };
    auto const& alpha_plane = m_pending_frame->alpha_plane;
    if (alpha_plane.has_value() && alpha_plane->size != size)
        return DecoderError::corrupted("VP9 alpha plane does not match the color frame size"sv);
    auto layout_result = frame_plane_layout(size, *bit_depth, subsampling, alpha_plane.has_value());
    if (layout_result.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "Failed to compute video frame plane layout: {}", layout_result.release_error());
    auto layout = layout_result.release_value();

    auto acquired_slot = m_frame_pool->try_acquire(layout.total_byte_count);
    if (!acquired_slot.has_value())
        return DecoderError::with_description(DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);

    auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(size, *bit_depth, subsampling, color_metadata.cicp,
        acquired_slot->bytes.slice(0, layout.y_size),
        acquired_slot->bytes.slice(layout.u_offset, layout.u_size),
        acquired_slot->bytes.slice(layout.v_offset, layout.v_size)));
    TRY(copy_pending_frame_into(yuv_data));
    if (alpha_plane.has_value())
        alpha_plane->data.bytes().copy_to(acquired_slot->bytes.slice(layout.alpha_offset, layout.alpha_size));

    auto frame = DECODER_TRY_ALLOC(try_make_ref_counted<VideoFrame>(m_pending_frame->timestamp, m_pending_frame->duration, size.to_type<u32>(), *bit_depth, subsampling, move(color_metadata), acquired_slot->storage, alpha_plane.has_value()));
    m_pending_frame.clear();
    return frame;
}

DecoderErrorOr<void> FFmpegVideoDecoder::copy_pending_frame_into(Gfx::YUVData& yuv_data)
{
    auto size = Gfx::Size<u32> { m_frame->width, m_frame->height };
    auto y_plane_size = size.to_type<size_t>();
    auto uv_plane_size = yuv_data.subsampling().subsampled_size(size).to_type<size_t>();

    Bytes buffers[] = { yuv_data.y_data(), yuv_data.u_data(), yuv_data.v_data() };
    Gfx::Size<size_t> plane_sizes[] = { y_plane_size, uv_plane_size, uv_plane_size };

    auto component_size = yuv_data.bit_depth() <= 8 ? 1 : 2;

    for (u32 plane = 0; plane < 3; plane++) {
        VERIFY(m_frame->linesize[plane] != 0);
        if (m_frame->linesize[plane] < 0)
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

        auto plane_size = plane_sizes[plane];
        auto const* source = m_frame->data[plane];
        VERIFY(source != nullptr);
        auto destination = buffers[plane];

        auto output_line_size = plane_size.width() * component_size;
        VERIFY(output_line_size <= static_cast<size_t>(m_frame->linesize[plane]));
        VERIFY(destination.size() >= output_line_size * plane_size.height());

        auto* dest_ptr = destination.data();
        for (size_t row = 0; row < plane_size.height(); row++) {
            memcpy(dest_ptr, source, output_line_size);
            source += m_frame->linesize[plane];
            dest_ptr += output_line_size;
        }
    }

    return {};
}

void FFmpegVideoDecoder::flush()
{
    m_functions.avcodec_flush_buffers(m_codec_context);
    if (m_alpha_codec_context)
        m_functions.avcodec_flush_buffers(m_alpha_codec_context);
    m_pending_frame.clear();
    m_frames_awaiting_output.flush();
}

}
