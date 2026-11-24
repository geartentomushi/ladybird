/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGfx/Size.h>
#include <LibMediaCodecs/Codecs/HighDynamicRangeMetadataTracker.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Forward.h>
#include <LibMediaCodecs/VideoDecoder.h>

namespace Media::FFmpeg {

// FFmpeg returns each packet's timestamp with the frame decoded from it, so a video decoder hands it a key instead and
// keeps the frame's timing, intent and HDR metadata under that key until the frame comes out.
class MEDIACODECS_API VideoFramesAwaitingOutput {
public:
    struct AlphaPlane {
        Gfx::IntSize size;
        Core::ImmutableBytes data;
    };

    struct Frame {
        AK::Duration timestamp;
        AK::Duration duration;
        DecodeIntent intent;
        ColorMetadata high_dynamic_range_metadata;
        Optional<AlphaPlane> alpha_plane {};
    };

    VideoFramesAwaitingOutput(CodecID codec_id, ReadonlyBytes configuration)
        : m_high_dynamic_range_metadata_tracker(codec_id, configuration)
    {
    }

    DecoderErrorOr<u64> add(CodedFrame const&, DecodeIntent);
    void set_alpha_plane(u64 key, AlphaPlane);
    Optional<Frame> take(i64 key);
    size_t size() const { return m_frames.size(); }
    void flush()
    {
        m_frames.clear();
        m_high_dynamic_range_metadata_tracker.forget_stream();
    }

private:
    Codecs::HighDynamicRangeMetadataTracker m_high_dynamic_range_metadata_tracker;
    u64 m_next_key { 0 };
    HashMap<u64, Frame> m_frames;
};

}
