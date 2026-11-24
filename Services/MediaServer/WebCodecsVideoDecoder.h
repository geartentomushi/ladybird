/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <AK/Variant.h>
#include <LibCore/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibThreading/Thread.h>

namespace MediaServer {

// The codec implementation behind a renderer's WebCodecs VideoDecoder. Its work runs in order on a thread of its own,
// and its callbacks run on the main thread.
class WebCodecsVideoDecoder final : public AtomicRefCounted<WebCodecsVideoDecoder> {
public:
    struct Callbacks {
        Function<void(i64 timestamp, Optional<u64> duration, Gfx::ShareableBitmap)> on_frame;
        Function<void(u64 flush_id)> on_flushed;
        Function<void(Media::DecoderError)> on_error;
        Function<void()> on_chunk_dequeued;
    };

    static NonnullRefPtr<WebCodecsVideoDecoder> create(Media::ParsedCodec, ByteBuffer description, Callbacks);
    ~WebCodecsVideoDecoder();

    // How many decoders have a thread that has not exited yet.
    static size_t live_count();

    // Returns false, leaving the chunk undecoded, when more chunks are waiting than the client may queue.
    [[nodiscard]] bool decode(bool is_key, i64 timestamp, Optional<u64> duration, ByteBuffer data);
    void flush(u64 flush_id);
    // Stops the thread once its current work is done. No callback runs after this.
    void stop();

private:
    struct DecodeCommand {
        bool is_key { false };
        i64 timestamp { 0 };
        Optional<u64> duration;
        ByteBuffer data;
    };
    struct FlushCommand {
        u64 flush_id { 0 };
    };
    using Command = Variant<DecodeCommand, FlushCommand>;

    WebCodecsVideoDecoder(Core::EventLoop& main_thread_event_loop, Media::ParsedCodec, ByteBuffer description, Callbacks);

    intptr_t run();
    void enqueue(Command);
    Media::DecoderErrorOr<void> handle(DecodeCommand&);
    Media::DecoderErrorOr<void> handle(FlushCommand&);
    Media::DecoderErrorOr<void> create_decoder();
    enum class DrainMode {
        AvailableOutput,
        UntilEndOfStream,
    };
    Media::DecoderErrorOr<void> drain(DrainMode);
    void report_error(Media::DecoderError);

    template<typename Callback>
    void invoke_on_main_thread(Callback);

    Core::EventLoop& m_main_thread_event_loop;
    Media::ParsedCodec m_codec;
    ByteBuffer m_description;
    Callbacks m_callbacks;

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    Queue<Command> m_commands;
    size_t m_pending_chunks { 0 };
    size_t m_pending_bytes { 0 };
    bool m_stopped { false };
    bool m_decode_completed { false };
    bool m_storage_freed { false };

    // Only used on the decode thread.
    OwnPtr<Media::VideoDecoder> m_decoder;
    bool m_decoder_needs_codec_configuration { true };
    bool m_failed { false };
    HashMap<i64, Optional<u64>> m_durations;
};

}
