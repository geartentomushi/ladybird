/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebCodecs/VideoFrame.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebCodecs {

GC_DEFINE_ALLOCATOR(VideoFrame);

GC::Ref<VideoFrame> VideoFrame::create(NonnullRefPtr<Gfx::Bitmap> bitmap, i64 timestamp, Optional<u64> duration)
{
    return GC::Heap::the().allocate<VideoFrame>(move(bitmap), timestamp, duration);
}

VideoFrame::VideoFrame(NonnullRefPtr<Gfx::Bitmap> bitmap, i64 timestamp, Optional<u64> duration)
    : m_bitmap(move(bitmap))
    , m_timestamp(timestamp)
    , m_duration(duration)
{
}

VideoFrame::~VideoFrame() = default;

// https://w3c.github.io/webcodecs/#dom-videoframe-format
Optional<VideoPixelFormat> VideoFrame::format() const
{
    // NB: The decoded frames are converted to the bitmap format, so the format is that of the bitmap.
    if (!m_bitmap)
        return {};
    switch (m_bitmap->format()) {
    case Gfx::BitmapFormat::BGRA8888:
        return VideoPixelFormat::BGRA;
    case Gfx::BitmapFormat::BGRx8888:
        return VideoPixelFormat::BGRX;
    case Gfx::BitmapFormat::RGBA8888:
        return VideoPixelFormat::RGBA;
    case Gfx::BitmapFormat::RGBx8888:
        return VideoPixelFormat::RGBX;
    default:
        return {};
    }
}

// https://w3c.github.io/webcodecs/#dom-videoframe-codedwidth
u32 VideoFrame::coded_width() const
{
    return m_bitmap ? static_cast<u32>(m_bitmap->width()) : 0;
}

// https://w3c.github.io/webcodecs/#dom-videoframe-codedheight
u32 VideoFrame::coded_height() const
{
    return m_bitmap ? static_cast<u32>(m_bitmap->height()) : 0;
}

// https://w3c.github.io/webcodecs/#dom-videoframe-displaywidth
u32 VideoFrame::display_width() const
{
    // FIXME: Apply the display aspect ratio and rotation of the decoder configuration.
    return coded_width();
}

// https://w3c.github.io/webcodecs/#dom-videoframe-displayheight
u32 VideoFrame::display_height() const
{
    return coded_height();
}

// https://w3c.github.io/webcodecs/#dom-videoframe-duration
Optional<u64> VideoFrame::duration() const
{
    return m_duration;
}

// https://w3c.github.io/webcodecs/#dom-videoframe-clone
WebIDL::ExceptionOr<GC::Ref<VideoFrame>> VideoFrame::clone()
{
    // 1. If the value of frame's [[Detached]] internal slot is true, throw an InvalidStateError DOMException.
    if (!m_bitmap)
        return WebIDL::InvalidStateError::create("The VideoFrame is closed"_utf16);

    // 2. Return the result of running the Clone VideoFrame algorithm with this.
    return create(*m_bitmap, m_timestamp, m_duration);
}

// https://w3c.github.io/webcodecs/#dom-videoframe-close
void VideoFrame::close()
{
    // 1. Run the Close VideoFrame algorithm with this.
    //    1. Assign null to frame's [[resource reference]].
    //    2. Assign true to frame's [[Detached]].
    //    3. Assign "" to frame's [[format]], 0 to its sizes and null to its [[duration]].
    m_bitmap = nullptr;
    m_duration = {};
}

}
