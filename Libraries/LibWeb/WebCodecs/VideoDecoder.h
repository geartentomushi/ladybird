/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibMediaClient/Forward.h>
#include <LibMediaCodecs/Forward.h>
#include <LibWeb/Bindings/VideoDecoder.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebCodecs {

using CodecState = Bindings::CodecState;
using VideoDecoderConfig = Bindings::VideoDecoderConfig;
using VideoDecoderInit = Bindings::VideoDecoderInit;

// https://w3c.github.io/webcodecs/#videodecoder-interface
class WEB_API VideoDecoder final : public DOM::EventTarget {
    WEB_WRAPPABLE(VideoDecoder, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(VideoDecoder);

public:
    static GC::Ref<VideoDecoder> create_for_constructor(JS::Object& relevant_global_object, VideoDecoderInit const&);
    virtual ~VideoDecoder() override;

    CodecState state() const { return m_state; }
    u32 decode_queue_size() const { return m_decode_queue_size; }

    void set_ondequeue(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> ondequeue();

    WebIDL::ExceptionOr<void> configure(VideoDecoderConfig const&);
    WebIDL::ExceptionOr<void> decode(EncodedVideoChunk&);
    void flush(GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> reset();
    WebIDL::ExceptionOr<void> close();

    static WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> is_config_supported(JS::VM&, VideoDecoderConfig const&);

private:
    VideoDecoder(GC::Ref<DOM::EventTarget> relevant_global_object, GC::Ref<WebIDL::CallbackType> output, GC::Ref<WebIDL::CallbackType> error);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;
    JS::Object& relevant_global_object() const;

    template<typename... Values, typename Callable>
    void queue_a_codec_task(GC::CaptureList<Values...>&&, Callable&&);
    void schedule_dequeue_event();
    void reset_video_decoder(GC::Ref<WebIDL::DOMException>);
    void close_video_decoder(GC::Ref<WebIDL::DOMException>);
    void release_codec();
    void update_pending_flush_keep_alive();

    void output_frame(u64 codec_generation, i64 timestamp, Optional<u64> duration, NonnullRefPtr<Gfx::Bitmap>);
    void codec_flushed(u64 codec_generation, u64 flush_id);
    void codec_failed(u64 codec_generation, Media::DecoderError const&);

    GC::Ref<DOM::EventTarget> m_global_object;

    // https://w3c.github.io/webcodecs/#dom-videodecoder-output-callback-slot
    GC::Ref<WebIDL::CallbackType> m_output_callback;
    // https://w3c.github.io/webcodecs/#dom-videodecoder-error-callback-slot
    GC::Ref<WebIDL::CallbackType> m_error_callback;

    // https://w3c.github.io/webcodecs/#dom-videodecoder-state-slot
    CodecState m_state { CodecState::Unconfigured };
    // https://w3c.github.io/webcodecs/#dom-videodecoder-decodequeuesize-slot
    u32 m_decode_queue_size { 0 };
    // https://w3c.github.io/webcodecs/#dom-videodecoder-key-chunk-required-slot
    bool m_key_chunk_required { true };
    // https://w3c.github.io/webcodecs/#dom-videodecoder-dequeue-event-scheduled-slot
    bool m_dequeue_event_scheduled { false };

    // https://w3c.github.io/webcodecs/#dom-videodecoder-pending-flush-promises-slot
    struct PendingFlush {
        u64 flush_id { 0 };
        GC::Ref<WebIDL::Promise> promise;
        bool awaiting_codec { true };
    };
    Vector<PendingFlush> m_pending_flush_promises;
    u64 m_next_flush_id { 1 };

    // NB: The promises returned by flush() must settle even when the page keeps nothing else that refers to us. Once
    //     the codec has flushed, the queued task keeps us alive instead, so that a destroyed document's discarded
    //     tasks do not leak us.
    GC::Root<VideoDecoder> m_pending_flush_keep_alive;

    // https://w3c.github.io/webcodecs/#dom-videodecoder-codec-implementation-slot
    // NB: The codec runs in the MediaServer. Each instance has a generation, so that work it reports after it has
    //     been released is ignored.
    RefPtr<MediaClient::Client> m_media_client;
    Optional<u64> m_codec_id;
    u64 m_codec_generation { 0 };
    u64 m_reset_generation { 0 };
};

}
