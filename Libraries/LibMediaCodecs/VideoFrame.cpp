/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibMediaCodecs/VideoSurface.h>

#include "VideoFrame.h"

namespace Media {

static u64 next_frame_id()
{
    static Atomic<u64> s_next_id { 1 };
    return s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth,
    Subsampling subsampling,
    ColorMetadata color_metadata,
    NonnullRefPtr<VideoFrameStorage> storage, bool has_alpha)
    : m_id(next_frame_id())
    , m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_has_alpha(has_alpha)
    , m_subsampling(subsampling)
    , m_color_metadata(move(color_metadata))
    , m_storage(move(storage))
{
}

VideoFrame::~VideoFrame() = default;

RefPtr<VideoSurface> VideoFrame::surface() const
{
    return m_storage->surface();
}

Core::AnonymousBuffer VideoFrame::pixel_buffer() const
{
    return m_storage->pixel_buffer();
}

Optional<Gfx::YUVData> VideoFrame::yuv_data() const
{
    auto yuv_data = yuv_data_in_buffer(pixel_buffer(), m_size.to_type<int>(), m_bit_depth, m_subsampling, cicp());
    if (yuv_data.is_error())
        return {};
    return yuv_data.release_value();
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VideoFrame::to_bitmap() const
{
    if (auto surface = this->surface())
        return surface->read_back_bitmap(cicp());
    auto yuv_data = this->yuv_data();
    if (!yuv_data.has_value())
        return Error::from_string_literal("Video frame has no pixels to read");
    auto bitmap = TRY(yuv_data->to_bitmap());
    if (!has_alpha())
        return bitmap;

    auto buffer = pixel_buffer();
    auto layout = TRY(frame_plane_layout(size().to_type<int>(), bit_depth(), subsampling(), true));
    if (layout.total_byte_count > buffer.size())
        return Error::from_string_literal("Video frame alpha plane does not fit its buffer");

    auto* alpha_row = buffer.data<u8 const>() + layout.alpha_offset;
    for (u32 y = 0; y < height(); ++y) {
        auto* pixel = reinterpret_cast<u8*>(bitmap->scanline(y));
        for (u32 x = 0; x < width(); ++x, pixel += 4) {
            auto alpha = alpha_row[x];
            for (size_t component = 0; component < 3; ++component)
                pixel[component] = (pixel[component] * alpha + 127) / 255;
            pixel[3] = alpha;
        }
        alpha_row += width();
    }
    return bitmap;
}

}
