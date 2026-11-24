/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Media {

struct VideoDecoderOptions {
    // Output each frame as soon as it has been decoded, rather than letting the decoder hold frames back to decode
    // several of them in parallel.
    bool optimize_for_latency { false };
};

}
