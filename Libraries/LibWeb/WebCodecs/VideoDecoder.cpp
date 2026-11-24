/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibMediaClient/Client.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebCodecs/EncodedVideoChunk.h>
#include <LibWeb/WebCodecs/EventNames.h>
#include <LibWeb/WebCodecs/VideoDecoder.h>
#include <LibWeb/WebCodecs/VideoFrame.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebCodecs {

GC_DEFINE_ALLOCATOR(VideoDecoder);

// https://w3c.github.io/webcodecs/#dom-videodecoder-videodecoder
GC::Ref<VideoDecoder> VideoDecoder::create_for_constructor(JS::Object& relevant_global_object, VideoDecoderInit const& init)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object);
    VERIFY(global_scope);

    // 1. Let d be a new VideoDecoder object.
    // 2. Assign a new queue to [[control message queue]].
    // 3. Assign false to [[message queue blocked]].
    // 4. Assign null to [[codec implementation]].
    // 5. Assign the result of starting a new parallel queue to [[codec work queue]].
    // 6. Assign false to [[codec saturated]].
    // 7. Assign init.output to [[output callback]].
    // 8. Assign init.error to [[error callback]].
    // 9. Assign true to [[key chunk required]].
    // 10. Assign "unconfigured" to [[state]]
    // 11. Assign 0 to [[decodeQueueSize]].
    // 12. Assign a new list to [[pending flush promises]].
    // 13. Assign false to [[dequeue event scheduled]].
    // 14. Return d.
    // NB: The control messages run as soon as they are queued, as the codec in the MediaServer queues its own work and
    //     so never becomes saturated.
    return GC::Heap::the().allocate<VideoDecoder>(global_scope->this_impl(), init.output, init.error);
}

VideoDecoder::VideoDecoder(GC::Ref<DOM::EventTarget> relevant_global_object, GC::Ref<WebIDL::CallbackType> output, GC::Ref<WebIDL::CallbackType> error)
    : m_global_object(relevant_global_object)
    , m_output_callback(output)
    , m_error_callback(error)
{
}

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
    visitor.visit(m_output_callback);
    visitor.visit(m_error_callback);
    for (auto& pending_flush : m_pending_flush_promises)
        visitor.visit(pending_flush.promise);
}

void VideoDecoder::finalize()
{
    Base::finalize();
    release_codec();
}

GC::Ptr<Bindings::Wrappable> VideoDecoder::relevant_global_impl() const
{
    return m_global_object;
}

JS::Object& VideoDecoder::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-ondequeue
void VideoDecoder::set_ondequeue(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::dequeue, event_handler);
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-ondequeue
GC::Ptr<WebIDL::CallbackType> VideoDecoder::ondequeue()
{
    return event_handler_attribute(EventNames::dequeue);
}

// https://w3c.github.io/webcodecs/#valid-videodecoderconfig
static bool is_valid_video_decoder_config(VideoDecoderConfig const& config)
{
    // 1. If codec is empty after stripping leading and trailing ASCII whitespace, return false.
    if (config.codec.trim_ascii_whitespace().is_empty())
        return false;

    // 2. If one of codedWidth or codedHeight is provided and the other is not, return false.
    if (config.coded_width.has_value() != config.coded_height.has_value())
        return false;

    // 3. If codedWidth = 0 or codedHeight = 0, return false.
    if (config.coded_width == 0u || config.coded_height == 0u)
        return false;

    // 4. If one of displayAspectWidth or displayAspectHeight is provided and the other is not, return false.
    if (config.display_aspect_width.has_value() != config.display_aspect_height.has_value())
        return false;

    // 5. If displayAspectWidth = 0 or displayAspectHeight = 0, return false.
    if (config.display_aspect_width == 0u || config.display_aspect_height == 0u)
        return false;

    // 6. If description is [detached], return false.
    if (config.description.has_value()) {
        WebIDL::BufferSource description { *config.description };
        if (auto buffer = description.viewed_array_buffer(); buffer && buffer->is_detached())
            return false;
    }

    // 7. Return true.
    return true;
}

// https://w3c.github.io/webcodecs/codec_registry.html#video-codec-registry
// NB: The codec strings are matched exactly, as the registry requires, so that ambiguous strings (such as "vp9"), audio
//     codecs and strings with surrounding whitespace are not recognized even where a container's codecs parameter would
//     accept them. H.264 strings are also checked for a known profile and level.
static bool is_recognized_video_codec_string(StringView codec)
{
    if (codec != codec.trim_whitespace())
        return false;
    if (codec == "vp8"sv)
        return true;
    if (codec.starts_with("vp09."sv) || codec.starts_with("av01."sv) || codec.starts_with("hev1."sv) || codec.starts_with("hvc1."sv))
        return true;
    if (!codec.starts_with("avc1."sv) && !codec.starts_with("avc3."sv))
        return false;

    // https://w3c.github.io/webcodecs/avc_codec_registration.html#fully-qualified-codec-strings
    auto parameters = codec.substring_view(5);
    if (parameters.length() != 6 || !all_of(parameters, is_ascii_hex_digit))
        return false;
    auto hex_byte_at = [&](size_t index) {
        return static_cast<u8>(parse_ascii_hex_digit(parameters[index]) << 4 | parse_ascii_hex_digit(parameters[index + 1]));
    };
    auto profile_idc = hex_byte_at(0);
    auto level_idc = hex_byte_at(4);
    static constexpr Array known_profiles { 0x42, 0x4D, 0x58, 0x64, 0x6E, 0x7A, 0xF4, 0x2C, 0x53, 0x56, 0x76, 0x80, 0x8A, 0x8B };
    static constexpr Array known_levels { 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x14, 0x15, 0x16, 0x1E, 0x1F, 0x20, 0x28, 0x29, 0x2A, 0x32, 0x33, 0x34, 0x3C, 0x3D, 0x3E };
    return any_of(known_profiles, [&](auto profile) { return profile == profile_idc; })
        && any_of(known_levels, [&](auto level) { return level == level_idc; });
}

static ErrorOr<ByteBuffer> copy_of_description(VideoDecoderConfig const& config)
{
    if (!config.description.has_value())
        return ByteBuffer {};
    WebIDL::BufferSource description { *config.description };
    return WebIDL::get_buffer_source_copy(description);
}

template<typename... Values, typename Callable>
void VideoDecoder::queue_a_codec_task(GC::CaptureList<Values...>&& captures, Callable&& steps)
{
    HTML::queue_global_task(HTML::Task::Source::Codec, relevant_global_object(), GC::create_function(GC::Heap::the(), move(captures), forward<Callable>(steps)));
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-configure
WebIDL::ExceptionOr<void> VideoDecoder::configure(VideoDecoderConfig const& config)
{
    // 1. If config is not a valid VideoDecoderConfig, throw a TypeError.
    if (!is_valid_video_decoder_config(config))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The VideoDecoderConfig is not valid"_utf16 };

    // 2. If [[state]] is "closed", throw an InvalidStateError DOMException.
    if (m_state == CodecState::Closed)
        return WebIDL::InvalidStateError::create("The VideoDecoder is closed"_utf16);

    // 3. Set [[state]] to "configured".
    m_state = CodecState::Configured;

    // 4. Set [[key chunk required]] to true.
    m_key_chunk_required = true;

    // 5. Queue a control message to configure the decoder with config.
    // 6. Process the control message queue.
    // NB: The control message runs here.
    //     1. Assign true to [[message queue blocked]].
    //     2. Enqueue the following steps to [[codec work queue]]:
    //        1. Assign the result of attempting to create a codec implementation that supports config to
    //           [[codec implementation]]. If that fails, queue a task to run the Close VideoDecoder algorithm with
    //           NotSupportedError.
    //        2. Assign false to [[message queue blocked]].
    // FIXME: Reconfigure the existing codec rather than replacing it, so that the output of chunks already given to it
    //        is not dropped.
    release_codec();

    // NB: The replaced codec drops its pending outputs, so the flushes waiting on it are complete.
    for (auto const& pending_flush : m_pending_flush_promises)
        codec_flushed(m_codec_generation, pending_flush.flush_id);

    auto codec = config.codec.to_utf8();
    auto description = copy_of_description(config);
    auto media_client = MediaClient::Client::acquire();
    if (!is_recognized_video_codec_string(codec) || description.is_error() || media_client.is_error()) {
        queue_a_codec_task(GC::captures(*this, m_codec_generation), [](VideoDecoder& self, u64& codec_generation) {
            if (codec_generation != self.m_codec_generation || self.m_state == CodecState::Closed)
                return;
            HTML::TemporaryExecutionContext context(HTML::relevant_realm(self.relevant_global_object()), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            self.close_video_decoder(WebIDL::NotSupportedError::create("The VideoDecoderConfig is not supported"_utf16));
        });
        return {};
    }

    m_media_client = media_client.release_value();
    auto codec_generation = ++m_codec_generation;
    auto weak_this = GC::Weak { *this };
    MediaClient::Client::WebCodecsVideoDecoderCallbacks callbacks;
    callbacks.on_frame = [weak_this, codec_generation](i64 timestamp, Optional<u64> duration, NonnullRefPtr<Gfx::Bitmap> bitmap) {
        if (auto self = weak_this.ptr())
            self->output_frame(codec_generation, timestamp, duration, move(bitmap));
    };
    callbacks.on_flushed = [weak_this, codec_generation](u64 flush_id) {
        if (auto self = weak_this.ptr())
            self->codec_flushed(codec_generation, flush_id);
    };
    callbacks.on_error = [weak_this, codec_generation](Media::DecoderError error) {
        if (auto self = weak_this.ptr())
            self->codec_failed(codec_generation, error);
    };
    m_codec_id = m_media_client->create_webcodecs_video_decoder(codec, description.value().bytes(), move(callbacks));
    return {};
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-decode
WebIDL::ExceptionOr<void> VideoDecoder::decode(EncodedVideoChunk& chunk)
{
    // 1. If [[state]] is not "configured", throw an InvalidStateError DOMException.
    if (m_state != CodecState::Configured)
        return WebIDL::InvalidStateError::create("The VideoDecoder is not configured"_utf16);

    // 2. If [[key chunk required]] is true:
    if (m_key_chunk_required) {
        // 1. If chunk.type is not key, throw a DataError.
        if (chunk.type() != Bindings::EncodedVideoChunkType::Key)
            return WebIDL::DataError::create("A key chunk is required"_utf16);

        // 2. Implementers SHOULD inspect the chunk's [[internal data]] to verify that it is truly a key chunk. If a
        //    mismatch is detected, throw a DataError.
        // 3. Otherwise, assign false to [[key chunk required]].
        m_key_chunk_required = false;
    }

    // 3. Increment [[decodeQueueSize]].
    ++m_decode_queue_size;

    // 4. Queue a control message to decode the chunk.
    // 5. Process the control message queue.
    // NB: The control message runs here.
    //     1. If [[codec saturated]] equals true, return "not processed".
    //     2. If decoding chunk will cause the [[codec implementation]] to become saturated, assign true to
    //        [[codec saturated]].
    //     3. Decrement [[decodeQueueSize]] and run the Schedule Dequeue Event algorithm.
    --m_decode_queue_size;
    schedule_dequeue_event();

    //     4. Enqueue the following steps to the [[codec work queue]]:
    //        1. Attempt to use [[codec implementation]] to decode the chunk.
    //        2. If decoding results in an error, queue a task to run the Close VideoDecoder algorithm with
    //           EncodingError and return.
    //        3. If [[codec saturated]] equals true and [[codec implementation]] is no longer saturated, queue a task
    //           to perform the following steps: ...
    //        4. Let decoded outputs be a list of decoded video data outputs emitted by [[codec implementation]] in
    //           presentation order.
    //        5. If decoded outputs is not empty, queue a task to run the Output VideoFrame algorithm with decoded
    //           outputs.
    if (m_media_client && m_codec_id.has_value())
        m_media_client->webcodecs_video_decoder_decode(*m_codec_id, chunk.type() == Bindings::EncodedVideoChunkType::Key, chunk.timestamp(), chunk.duration(), chunk.data());
    return {};
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-flush
void VideoDecoder::flush(GC::Ref<WebIDL::Promise> promise)
{
    // 1. If [[state]] is not "configured", return a promise rejected with InvalidStateError DOMException.
    if (m_state != CodecState::Configured) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("The VideoDecoder is not configured"_utf16));
        return;
    }

    // 2. Set [[key chunk required]] to true.
    m_key_chunk_required = true;

    // 3. Let promise be a new Promise.
    // 4. Append promise to [[pending flush promises]].
    auto flush_id = m_next_flush_id++;
    m_pending_flush_promises.append({ flush_id, promise });
    update_pending_flush_keep_alive();

    // 5. Queue a control message to flush the codec with promise.
    // 6. Process the control message queue.
    // NB: The control message runs here.
    //     1. Signal [[codec implementation]] to emit all internal pending outputs.
    //     2. Let decoded outputs be a list of decoded video data outputs emitted by [[codec implementation]].
    //     3. Queue a task to perform these steps: ...
    if (m_media_client && m_codec_id.has_value()) {
        m_media_client->webcodecs_video_decoder_flush(*m_codec_id, flush_id);
        return;
    }

    // NB: Without a codec, there is nothing to flush.
    codec_flushed(m_codec_generation, flush_id);

    // 7. Return promise.
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-reset
WebIDL::ExceptionOr<void> VideoDecoder::reset()
{
    // 1. Run the Reset VideoDecoder algorithm with an AbortError DOMException.
    // NB: Its first step throws when [[state]] is "closed".
    if (m_state == CodecState::Closed)
        return WebIDL::InvalidStateError::create("The VideoDecoder is closed"_utf16);
    reset_video_decoder(WebIDL::AbortError::create("The VideoDecoder was reset"_utf16));
    return {};
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-close
WebIDL::ExceptionOr<void> VideoDecoder::close()
{
    // 1. Run the Close VideoDecoder algorithm with an AbortError DOMException.
    // NB: Its reset step throws when [[state]] is "closed".
    if (m_state == CodecState::Closed)
        return WebIDL::InvalidStateError::create("The VideoDecoder is closed"_utf16);
    close_video_decoder(WebIDL::AbortError::create("The VideoDecoder was closed"_utf16));
    return {};
}

// https://w3c.github.io/webcodecs/#clone-config
static WebIDL::ExceptionOr<JS::Value> clone_configuration(JS::Realm& realm, VideoDecoderConfig const& config)
{
    // 1. Let clone be a new empty instance of config's dictionary type.
    // 2. For each dictionary member m in config:
    //    1. If m is a BufferSource, let clone.m be a copy of config.m.
    //    2. Otherwise, let clone.m be config.m.
    VideoDecoderConfig clone = config;
    if (config.description.has_value()) {
        auto bytes = copy_of_description(config);
        if (bytes.is_error())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Unable to copy the description"_utf16 };
        clone.description = JS::ArrayBuffer::create(realm, bytes.release_value());
    }

    // 3. Return clone.
    return Bindings::video_decoder_config_to_value(realm, clone);
}

// https://w3c.github.io/webcodecs/#dom-videodecoder-isconfigsupported
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> VideoDecoder::is_config_supported(JS::VM& vm, VideoDecoderConfig const& config)
{
    auto& realm = *vm.current_realm();

    // 1. If config is not a valid VideoDecoderConfig, return a promise rejected with TypeError.
    if (!is_valid_video_decoder_config(config))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The VideoDecoderConfig is not valid"_utf16 };

    // 2. Let p be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // NB: The configuration is cloned now, as the buffers it refers to may change before the check completes.
    auto cloned_config = GC::make_root(TRY(clone_configuration(realm, config)).as_object());

    // 3. Let checkSupportQueue be the result of starting a new parallel queue.
    // 4. Enqueue the following steps to checkSupportQueue:
    //    1. Let supported be the result of running the Check Configuration Support algorithm with config.
    //    2. Queue a task to run the following steps:
    //       1. Let decoderSupport be a newly constructed VideoDecoderSupport, initialized as follows:
    //          1. Set config to the result of running the Clone Configuration algorithm with config.
    //          2. Set supported to supported.
    //       2. Resolve p with decoderSupport.
    auto resolve = [promise = GC::make_root(promise), cloned_config](bool supported) {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::queue_global_task(HTML::Task::Source::Codec, realm.global_object(), GC::create_function(GC::Heap::the(), GC::captures(promise, cloned_config, supported), [](auto& promise, auto& cloned_config, auto& supported) {
            auto& realm = WebIDL::promise_realm(*promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto decoder_support = JS::Object::create(realm, realm.intrinsics().object_prototype());
            MUST(decoder_support->create_data_property_or_throw("config"_utf16_fly_string, cloned_config.ptr()));
            MUST(decoder_support->create_data_property_or_throw("supported"_utf16_fly_string, JS::Value(supported)));
            WebIDL::resolve_promise(*promise, decoder_support);
        }));
    };

    auto codec = config.codec.to_utf8();
    auto media_client = MediaClient::Client::acquire();
    if (!is_recognized_video_codec_string(codec) || media_client.is_error()) {
        resolve(false);
        return promise;
    }
    auto capabilities = media_client.value()->request_decoder_capabilities(codec.bytes_as_string_view());
    capabilities->when_resolved([resolve](Optional<Media::DecoderCapabilities>& capabilities) {
        resolve(capabilities.has_value());
    });
    capabilities->when_rejected([resolve](Error&) {
        resolve(false);
    });

    // 5. Return p.
    return promise;
}

// https://w3c.github.io/webcodecs/#videodecoder-schedule-dequeue-event
void VideoDecoder::schedule_dequeue_event()
{
    // 1. If [[dequeue event scheduled]] equals true, return.
    if (m_dequeue_event_scheduled)
        return;

    // 2. Assign true to [[dequeue event scheduled]].
    m_dequeue_event_scheduled = true;

    // 3. Queue a task to run the following steps:
    queue_a_codec_task(GC::captures(*this), [](VideoDecoder& self) {
        // 1. Fire a simple event named dequeue at this.
        self.dispatch_event(DOM::Event::create(EventNames::dequeue, HighResolutionTime::current_high_resolution_time(self.relevant_global_object())));

        // 2. Assign false to [[dequeue event scheduled]].
        self.m_dequeue_event_scheduled = false;
    });
}

void VideoDecoder::update_pending_flush_keep_alive()
{
    if (m_pending_flush_promises.is_empty())
        m_pending_flush_keep_alive = nullptr;
    else if (!m_pending_flush_keep_alive)
        m_pending_flush_keep_alive = *this;
}

void VideoDecoder::release_codec()
{
    if (m_media_client && m_codec_id.has_value())
        m_media_client->destroy_webcodecs_video_decoder(*m_codec_id);
    m_codec_id = {};
    m_media_client = nullptr;
    ++m_codec_generation;
}

// https://w3c.github.io/webcodecs/#reset-videodecoder
void VideoDecoder::reset_video_decoder(GC::Ref<WebIDL::DOMException> exception)
{
    // 1. If [[state]] is "closed", throw an InvalidStateError.
    // NB: The callers check this.
    VERIFY(m_state != CodecState::Closed);

    // 2. Set [[state]] to "unconfigured".
    m_state = CodecState::Unconfigured;

    // 3. Signal [[codec implementation]] to cease producing output for the previous configuration.
    release_codec();

    // 4. Remove all control messages from the [[control message queue]].
    // 5. If [[decodeQueueSize]] is greater than zero:
    if (m_decode_queue_size > 0) {
        // 1. Set [[decodeQueueSize]] to zero.
        m_decode_queue_size = 0;

        // 2. Run the Schedule Dequeue Event algorithm.
        schedule_dequeue_event();
    }

    // 6. For each promise in [[pending flush promises]]:
    auto pending_flush_promises = move(m_pending_flush_promises);
    update_pending_flush_keep_alive();
    for (auto& pending_flush : pending_flush_promises) {
        // 1. Reject promise with exception.
        WebIDL::reject_promise(*pending_flush.promise, exception);

        // 2. Remove promise from [[pending flush promises]].
    }
}

// https://w3c.github.io/webcodecs/#close-videodecoder
void VideoDecoder::close_video_decoder(GC::Ref<WebIDL::DOMException> exception)
{
    // 1. Run the Reset VideoDecoder algorithm with exception.
    reset_video_decoder(exception);

    // 2. Set [[state]] to "closed".
    m_state = CodecState::Closed;

    // 3. Clear [[codec implementation]] and release associated system resources.
    // NB: Resetting released the codec.

    // 4. If exception is not an AbortError DOMException, invoke the [[error callback]] with exception.
    if (exception->name() == "AbortError"sv)
        return;
    auto& callback_realm = m_error_callback->callback_context->realm();
    auto& wrapper_world = Bindings::host_defined_wrapper_world(callback_realm);
    JS::Value wrapped_exception = Bindings::wrap(wrapper_world, callback_realm, exception);
    (void)WebIDL::invoke_callback(*m_error_callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, ReadonlySpan<JS::Value> { &wrapped_exception, 1 });
}

// https://w3c.github.io/webcodecs/#videodecoder-output-videoframes
void VideoDecoder::output_frame(u64 codec_generation, i64 timestamp, Optional<u64> duration, NonnullRefPtr<Gfx::Bitmap> bitmap)
{
    // NB: The frame is made now, so that the task holds a cell rather than the bitmap.
    auto frame = VideoFrame::create(move(bitmap), timestamp, duration);
    queue_a_codec_task(GC::captures(*this, *frame, codec_generation), [](VideoDecoder& self, VideoFrame& frame, u64& codec_generation) {
        // NB: Output from a codec that has since been reset or closed is dropped.
        if (codec_generation != self.m_codec_generation || self.m_state != CodecState::Configured)
            return;

        // 1. For each output in outputs:
        //    1. Let timestamp and duration be the timestamp and duration from the EncodedVideoChunk associated with
        //       output.
        //    2. Let displayAspectWidth and displayAspectHeight be the values of the configuration's
        //       displayAspectWidth and displayAspectHeight if they exist, or undefined otherwise.
        //    3. Let colorSpace be the VideoColorSpace for output as detected by the codec implementation.
        //    4. Let frame be the result of running the Create a VideoFrame algorithm with output, timestamp, duration,
        //       displayAspectWidth, displayAspectHeight, and colorSpace.
        // NB: The frame was created before queueing this task.

        //    5. Invoke [[output callback]] with frame.
        auto& callback_realm = self.m_output_callback->callback_context->realm();
        auto& wrapper_world = Bindings::host_defined_wrapper_world(callback_realm);
        JS::Value wrapped_frame = Bindings::wrap(wrapper_world, callback_realm, GC::Ref { frame });
        (void)WebIDL::invoke_callback(*self.m_output_callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, ReadonlySpan<JS::Value> { &wrapped_frame, 1 });
    });
}

void VideoDecoder::codec_flushed(u64 codec_generation, u64 flush_id)
{
    // https://w3c.github.io/webcodecs/#flush-videodecoder
    queue_a_codec_task(GC::captures(*this, codec_generation, flush_id), [](VideoDecoder& self, u64& codec_generation, u64& flush_id) {
        if (codec_generation != self.m_codec_generation)
            return;

        // 1. If [[codec implementation]] has been reset since the flush, the promise has already been rejected.
        // 2. Remove promise from [[pending flush promises]].
        auto index = self.m_pending_flush_promises.find_first_index_if([&](auto const& pending_flush) { return pending_flush.flush_id == flush_id; });
        if (!index.has_value())
            return;
        auto promise = self.m_pending_flush_promises.take(*index).promise;
        self.update_pending_flush_keep_alive();

        // 3. Resolve promise.
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        WebIDL::resolve_promise(*promise);
    });
}

void VideoDecoder::codec_failed(u64 codec_generation, Media::DecoderError const& error)
{
    auto is_unsupported = error.category() == Media::DecoderErrorCategory::NotImplemented || error.category() == Media::DecoderErrorCategory::UnrecognizedFormat;
    queue_a_codec_task(GC::captures(*this, codec_generation, is_unsupported), [](VideoDecoder& self, u64& codec_generation, bool& is_unsupported) {
        if (codec_generation != self.m_codec_generation || self.m_state == CodecState::Closed)
            return;

        // NB: A codec that cannot be created for the configuration closes the decoder with NotSupportedError, and
        //     one that fails to decode closes it with EncodingError.
        HTML::TemporaryExecutionContext context(HTML::relevant_realm(self.relevant_global_object()), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        if (is_unsupported)
            self.close_video_decoder(WebIDL::NotSupportedError::create("The VideoDecoderConfig is not supported"_utf16));
        else
            self.close_video_decoder(WebIDL::EncodingError::create("The video could not be decoded"_utf16));
    });
}

}
