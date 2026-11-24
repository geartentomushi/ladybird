/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorker);

ServiceWorker::ServiceWorker(ServiceWorkerRecord* service_worker_record)
    : DOM::EventTarget()
    , m_service_worker_record(service_worker_record)
{
}

ServiceWorker::~ServiceWorker() = default;

GC::Ref<ServiceWorker> ServiceWorker::create(ServiceWorkerRecord* service_worker_record)
{
    return GC::Heap::the().allocate<ServiceWorker>(service_worker_record);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworker-postmessage-message-transfer
WebIDL::ExceptionOr<void> ServiceWorker::post_message(JS::Realm& realm, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer)
{
    // 1. Let serviceWorker be the service worker represented by this.
    // 2. Let incumbentSettings be the incumbent settings object.
    // 3. Let incumbentGlobal be incumbentSettings's global object.
    // 4. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, options["transfer"]).
    //    Rethrow any exceptions.
    // 5. If the result of running the Should Skip Event algorithm with "message" and serviceWorker is true, then return.
    // 6. Run these substeps in parallel:
    //     1. If the result of running the Run Service Worker algorithm with serviceWorker is failure, then return.
    //     2. Queue a task on the DOM manipulation task source to run the following steps:
    //         ...
    //         7. Dispatch e at destination.
    // NB: The agent of the service worker was started with a port entangled with its global scope's inside port, which
    //     delivers the messages posted to it as message events at the global scope, like the port of a dedicated
    //     worker does. Posting to the port serializes the message and rethrows any exceptions.
    // FIXME: Dispatch an ExtendableMessageEvent whose source is a Client or ServiceWorker for the incumbent global.
    // FIXME: Run the service worker again if it is not running.
    if (m_state == ServiceWorkerState::Redundant || !m_service_worker_record || !m_service_worker_record->agent)
        return {};
    auto outside_port = m_service_worker_record->agent->outside_port();
    if (!outside_port)
        return {};
    return outside_port->post_message(realm, message, transfer);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworker-postmessage-options
WebIDL::ExceptionOr<void> ServiceWorker::post_message(JS::Realm& realm, JS::Value message, Bindings::StructuredSerializeOptions const& options)
{
    // The postMessage(message, transfer) method steps are to run postMessage(message, options) with message and
    // options whose transfer is transfer.
    return post_message(realm, message, options.transfer);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworker-scripturl
Utf16String ServiceWorker::script_url() const
{
    if (!m_service_worker_record)
        return {};

    auto serialized_url = m_service_worker_record->script_url.serialize();
    return Utf16String::from_ascii_without_validation(serialized_url.bytes());
}

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                           \
    void ServiceWorker::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                     \
        set_event_handler_attribute(event_name, value);                   \
    }                                                                     \
    WebIDL::CallbackType* ServiceWorker::attribute_name()                 \
    {                                                                     \
        return event_handler_attribute(event_name);                       \
    }
ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
