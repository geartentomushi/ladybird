/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace MediaClient {

// Each WebCodecs decoder in the MediaServer runs on a thread of its own, so a renderer, and all of the renderers
// sharing a MediaServer together, may only have so many of them.
static constexpr size_t max_webcodecs_video_decoders_per_client = 16;
static constexpr size_t max_webcodecs_video_decoders_per_process = 64;

// How many chunks, and how many bytes of chunk data, may wait in one WebCodecs decoder in the MediaServer. A chunk is
// always accepted when no others are waiting, so that a chunk larger than the byte limit can still be decoded.
static constexpr size_t max_webcodecs_video_decoder_pending_chunks = 16;
static constexpr size_t max_webcodecs_video_decoder_pending_bytes = 32 * MiB;

constexpr bool webcodecs_video_decoder_can_accept_chunk(size_t pending_chunks, size_t pending_bytes, size_t chunk_size)
{
    if (pending_chunks == 0)
        return true;
    return pending_chunks < max_webcodecs_video_decoder_pending_chunks && chunk_size <= max_webcodecs_video_decoder_pending_bytes - min(pending_bytes, max_webcodecs_video_decoder_pending_bytes);
}

}
