/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/Color/ColorMetadata.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <MediaServer/WebCodecsVideoDecoder.h>

namespace MediaServer {

NonnullRefPtr<WebCodecsVideoDecoder> WebCodecsVideoDecoder::create(Media::ParsedCodec codec, ByteBuffer description, Media::VideoDecoderOptions options, Callbacks callbacks)
{
    auto decoder = adopt_ref(*new WebCodecsVideoDecoder(Core::EventLoop::current(), codec, move(description), options, move(callbacks)));
    // NB: The detached thread keeps itself, and so this decoder, alive until it exits.
    auto thread = Threading::Thread::construct("WebCodecsDecode"sv, [decoder] { return decoder->run(); });
    thread->start();
    thread->detach();
    return decoder;
}

WebCodecsVideoDecoder::WebCodecsVideoDecoder(Core::EventLoop& main_thread_event_loop, Media::ParsedCodec codec, ByteBuffer description, Media::VideoDecoderOptions options, Callbacks callbacks)
    : m_main_thread_event_loop(main_thread_event_loop)
    , m_codec(codec)
    , m_description(move(description))
    , m_options(options)
    , m_callbacks(move(callbacks))
{
}

WebCodecsVideoDecoder::~WebCodecsVideoDecoder() = default;

void WebCodecsVideoDecoder::enqueue(Command command)
{
    MutexLocker locker { m_mutex };
    m_commands.enqueue(move(command));
    m_condition.broadcast();
}

void WebCodecsVideoDecoder::decode(bool is_key, i64 timestamp, Optional<u64> duration, ByteBuffer data)
{
    enqueue(DecodeCommand { is_key, timestamp, duration, move(data) });
}

void WebCodecsVideoDecoder::flush(u64 flush_id)
{
    enqueue(FlushCommand { flush_id });
}

void WebCodecsVideoDecoder::stop()
{
    {
        MutexLocker locker { m_mutex };
        m_stopped = true;
        m_commands.clear();
        m_condition.broadcast();
    }

    // NB: The decode thread may drop the last reference to us, so destroy the callbacks, and the connection references
    //     they hold, here on the main thread.
    m_callbacks = {};
}

template<typename Callback>
void WebCodecsVideoDecoder::invoke_on_main_thread(Callback callback)
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), callback = move(callback)] mutable {
        {
            MutexLocker locker { self->m_mutex };
            if (self->m_stopped)
                return;
        }
        callback(*self);
    });
}

void WebCodecsVideoDecoder::report_error(Media::DecoderError error)
{
    m_failed = true;
    invoke_on_main_thread([error = move(error)](WebCodecsVideoDecoder& self) mutable {
        self.m_callbacks.on_error(move(error));
    });
}

intptr_t WebCodecsVideoDecoder::run()
{
    // An unsupported configuration is reported right away rather than when the first chunk arrives.
    if (auto result = create_decoder(); result.is_error())
        report_error(result.release_error());

    while (true) {
        Optional<Command> command;
        {
            MutexLocker locker { m_mutex };
            while (m_commands.is_empty() && !m_stopped)
                m_condition.wait();
            if (m_stopped)
                break;
            command = m_commands.dequeue();
        }
        if (m_failed)
            continue;
        auto result = command->visit([&](auto& specific_command) { return handle(specific_command); });
        if (result.is_error())
            report_error(result.release_error());
    }
    m_decoder = nullptr;
    return 0;
}

Media::DecoderErrorOr<void> WebCodecsVideoDecoder::create_decoder()
{
    // Each frame is read back into memory to be shared with the page.
    auto selection = Media::select_video_decoder(m_codec, {}, false, true);
    if (!selection.has_value())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::NotImplemented, "Could not find a decoder for the codec"sv);
    m_decoder = TRY(Media::create_video_decoder(selection, m_codec.codec_id(), m_description.bytes(), m_options));
    m_decoder->set_storage_freed_callback([self = NonnullRefPtr(*this)] {
        MutexLocker locker { self->m_mutex };
        self->m_storage_freed = true;
        self->m_condition.broadcast();
    });
    m_decoder->set_decode_completed_callback([self = NonnullRefPtr(*this)](Media::DecodeCompletion) {
        MutexLocker locker { self->m_mutex };
        self->m_decode_completed = true;
        self->m_condition.broadcast();
    });
    m_decoder_needs_codec_configuration = true;
    return {};
}

Media::DecoderErrorOr<void> WebCodecsVideoDecoder::handle(DecodeCommand& command)
{
    if (!m_decoder)
        TRY(create_decoder());

    auto data_or_error = FixedArray<u8>::create(command.data.bytes());
    if (data_or_error.is_error())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to allocate the coded data"sv);

    Optional<FixedArray<u8>> codec_configuration;
    if (m_decoder_needs_codec_configuration) {
        auto configuration_or_error = FixedArray<u8>::create(m_description.bytes());
        if (configuration_or_error.is_error())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to allocate the codec configuration"sv);
        codec_configuration = configuration_or_error.release_value();
        m_decoder_needs_codec_configuration = false;
    }

    auto timestamp = AK::Duration::from_microseconds(command.timestamp);
    auto duration = AK::Duration::from_microseconds(static_cast<i64>(command.duration.value_or(0)));
    Media::CodedFrame frame {
        m_codec.codec_id(),
        timestamp,
        timestamp,
        duration,
        command.is_key ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
        data_or_error.release_value(),
        move(codec_configuration),
    };
    m_durations.set(command.timestamp, command.duration);

    {
        MutexLocker locker { m_mutex };
        m_decode_completed = false;
    }
    TRY(m_decoder->receive_coded_data(frame, Media::DecodeIntent::Output));
    return drain(DrainMode::AvailableOutput);
}

Media::DecoderErrorOr<void> WebCodecsVideoDecoder::handle(FlushCommand& command)
{
    if (m_decoder) {
        m_decoder->signal_end_of_stream();
        TRY(drain(DrainMode::UntilEndOfStream));
        // A decoder that has reached the end of its stream takes no more data, so the next chunk gets a new one.
        m_decoder = nullptr;
    }
    m_durations.clear();
    invoke_on_main_thread([flush_id = command.flush_id](WebCodecsVideoDecoder& self) {
        self.m_callbacks.on_flushed(flush_id);
    });
    return {};
}

struct SharedFrame {
    i64 timestamp;
    Gfx::ShareableBitmap bitmap;
};

// NB: This takes ownership of the frame so that the decoder can reuse its storage as soon as it has been copied.
static Media::DecoderErrorOr<SharedFrame> share_frame(NonnullRefPtr<Media::VideoFrame> frame)
{
    auto bitmap = frame->to_bitmap();
    if (bitmap.is_error())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to convert a decoded frame"sv);
    auto shareable_bitmap = bitmap.value()->to_bitmap_backed_by_anonymous_buffer();
    if (shareable_bitmap.is_error())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to share a decoded frame"sv);
    return SharedFrame {
        .timestamp = frame->timestamp().to_microseconds(),
        .bitmap = Gfx::ShareableBitmap { shareable_bitmap.release_value(), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap },
    };
}

Media::DecoderErrorOr<void> WebCodecsVideoDecoder::drain(DrainMode mode)
{
    while (true) {
        auto frame_result = m_decoder->take_next_output(Media::ColorMetadata {});
        if (!frame_result.is_error()) {
            auto shared_frame = share_frame(frame_result.release_value());
            if (shared_frame.is_error())
                return shared_frame.release_error();
            auto [timestamp, bitmap] = shared_frame.release_value();
            auto duration = m_durations.take(timestamp).value_or({});
            invoke_on_main_thread([timestamp, duration, bitmap = move(bitmap)](WebCodecsVideoDecoder& self) mutable {
                self.m_callbacks.on_frame(timestamp, duration, move(bitmap));
            });
            continue;
        }

        auto category = frame_result.error().category();
        if (category == Media::DecoderErrorCategory::EndOfStream)
            return {};

        if (category == Media::DecoderErrorCategory::NeedsMoreInput || category == Media::DecoderErrorCategory::TryAgain) {
            if (category == Media::DecoderErrorCategory::NeedsMoreInput && mode == DrainMode::AvailableOutput && !m_decoder->has_any_decode_in_progress())
                return {};
            MutexLocker locker { m_mutex };
            auto& flag = category == Media::DecoderErrorCategory::TryAgain ? m_storage_freed : m_decode_completed;
            while (!flag && !m_stopped) {
                if (!m_condition.wait_for(AK::Duration::from_seconds(5)) && !flag)
                    return Media::DecoderError::with_description(Media::DecoderErrorCategory::Unknown, "The video decoder stopped responding"sv);
            }
            if (m_stopped)
                return {};
            flag = false;
            continue;
        }

        return frame_result.release_error();
    }
}

}
