/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/VideoFrame.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebCodecs {

using VideoPixelFormat = Bindings::VideoPixelFormat;

// https://w3c.github.io/webcodecs/#videoframe-interface
class WEB_API VideoFrame final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(VideoFrame, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(VideoFrame);

public:
    static GC::Ref<VideoFrame> create(NonnullRefPtr<Gfx::Bitmap>, i64 timestamp, Optional<u64> duration);
    virtual ~VideoFrame() override;

    Optional<VideoPixelFormat> format() const;
    u32 coded_width() const;
    u32 coded_height() const;
    u32 display_width() const;
    u32 display_height() const;
    Optional<u64> duration() const;
    i64 timestamp() const { return m_timestamp; }

    WebIDL::ExceptionOr<GC::Ref<VideoFrame>> clone();
    void close();

    bool is_closed() const { return !m_bitmap; }
    // The frame's pixels, or null once the frame is closed.
    RefPtr<Gfx::Bitmap> bitmap() const { return m_bitmap; }

private:
    VideoFrame(NonnullRefPtr<Gfx::Bitmap>, i64 timestamp, Optional<u64> duration);

    // https://w3c.github.io/webcodecs/#dom-videoframe-resource-reference-slot
    RefPtr<Gfx::Bitmap> m_bitmap;
    i64 m_timestamp { 0 };
    Optional<u64> m_duration;
};

}
