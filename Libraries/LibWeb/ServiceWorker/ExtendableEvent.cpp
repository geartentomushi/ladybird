/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/ServiceWorker/ExtendableEvent.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ExtendableEvent);

GC::Ref<ExtendableEvent> ExtendableEvent::create(Utf16FlyString const& event_name, ExtendableEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<ExtendableEvent>(event_name, event_init, time_stamp);
}

ExtendableEvent::ExtendableEvent(Utf16FlyString const& event_name, ExtendableEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

ExtendableEvent::~ExtendableEvent() = default;

void ExtendableEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_extend_lifetime_promises);
}

// https://w3c.github.io/ServiceWorker/#extendableevent-active
bool ExtendableEvent::is_active() const
{
    // An ExtendableEvent object is said to be active when its timed out flag is unset and either its pending promises
    // count is greater than zero or its dispatch flag is set.
    return !m_timed_out && (m_pending_promises_count > 0 || dispatched());
}

// https://w3c.github.io/ServiceWorker/#dom-extendableevent-waituntil
WebIDL::ExceptionOr<void> ExtendableEvent::wait_until(GC::Ref<WebIDL::Promise> promise)
{
    // The waitUntil(f) method steps are to add lifetime promise f to this.

    // https://w3c.github.io/ServiceWorker/#extendableevent-add-lifetime-promise
    // 1. If event’s isTrusted attribute is false, throw an "InvalidStateError" DOMException.
    if (!is_trusted())
        return WebIDL::InvalidStateError::create("waitUntil() can only be called on trusted events"_utf16);

    // 2. If event is not active, throw an "InvalidStateError" DOMException.
    if (!is_active())
        return WebIDL::InvalidStateError::create("waitUntil() can only be called while the event is active"_utf16);

    // 3. Add promise to event’s extend lifetime promises.
    m_extend_lifetime_promises.append(promise);

    // 4. Increment event’s pending promises count by one.
    ++m_pending_promises_count;

    // 5. Upon fulfillment or rejection of promise, queue a microtask to run these substeps:
    // NB: Promise reactions already run as microtasks.
    auto decrement = GC::create_function(GC::Heap::the(), GC::captures(*this), [](ExtendableEvent& self, JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Decrement event’s pending promises count by one.
        --self.m_pending_promises_count;

        // FIXME: 2. If event’s pending promises count is 0, then: let registration be the current global object's
        //           associated service worker's containing service worker registration, and if registration is
        //           unregistered, invoke Try Clear Registration with registration.
        return JS::js_undefined();
    });
    WebIDL::react_to_promise(promise, decrement, decrement);

    return {};
}

void ExtendableEvent::wait_for_extend_lifetime_promises(JS::Realm& realm, GC::Ref<GC::Function<void(bool succeeded)>> on_complete)
{
    auto promise_count = m_extend_lifetime_promises.size();
    if (promise_count == 0) {
        on_complete->function()(true);
        return;
    }

    auto success_steps = GC::create_function(GC::Heap::the(), GC::captures(*this, realm, on_complete, promise_count), [](ExtendableEvent& self, JS::Realm& realm, GC::Ref<GC::Function<void(bool)>>& on_complete, size_t promise_count, Vector<JS::Value> const&) {
        // NB: waitUntil() may have been called again while the earlier promises were pending.
        if (self.m_extend_lifetime_promises.size() != promise_count) {
            self.wait_for_extend_lifetime_promises(realm, on_complete);
            return;
        }
        on_complete->function()(true);
    });
    auto failure_steps = GC::create_function(GC::Heap::the(), GC::captures(on_complete), [](auto& on_complete, JS::Value) {
        on_complete->function()(false);
    });
    HTML::TemporaryExecutionContext const execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    WebIDL::wait_for_all(realm, m_extend_lifetime_promises, success_steps, failure_steps);
}

}
