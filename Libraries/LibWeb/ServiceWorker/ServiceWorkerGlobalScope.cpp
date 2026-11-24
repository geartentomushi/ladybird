/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/EventNames.h>
#include <LibWeb/ServiceWorker/ExtendableEvent.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(page)
{
}

void ServiceWorkerGlobalScope::finalize()
{
    Base::finalize();
    WindowOrWorkerGlobalScopeMixin::finalize();
}

void ServiceWorkerGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_cookie_store);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
void ServiceWorkerGlobalScope::set_oninstall(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::install, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::oninstall()
{
    return event_handler_attribute(EventNames::install);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
void ServiceWorkerGlobalScope::set_onactivate(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::activate, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onactivate()
{
    return event_handler_attribute(EventNames::activate);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
void ServiceWorkerGlobalScope::set_onfetch(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::fetch, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onfetch()
{
    return event_handler_attribute(EventNames::fetch);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
void ServiceWorkerGlobalScope::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
void ServiceWorkerGlobalScope::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

// https://cookiestore.spec.whatwg.org/#ServiceWorkerGlobalScope
GC::Ref<CookieStore::CookieStore> ServiceWorkerGlobalScope::cookie_store()
{
    // The cookieStore getter steps are to return this’s associated CookieStore.
    if (!m_cookie_store)
        m_cookie_store = CookieStore::CookieStore::create(realm());
    return *m_cookie_store;
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-skipwaiting
GC::Ref<WebIDL::Promise> ServiceWorkerGlobalScope::skip_waiting()
{
    // 1. Let promise be a new promise.
    // 2. Run the following substeps in parallel:
    //     1. Set service worker's skip waiting flag.
    //     2. Invoke Try Activate with service worker's containing service worker registration.
    //     3. Resolve promise with undefined.
    // 3. Return promise.
    // NB: The registration activates a worker as soon as it is installed, since no client is ever controlled by its
    //     active worker yet. So there's never a waiting worker for the skip waiting flag to promote.
    return WebIDL::create_resolved_promise(realm(), JS::js_undefined());
}

void ServiceWorkerGlobalScope::run_lifecycle_step(HTML::ServiceWorkerLifecycleStep step, GC::Ref<GC::Function<void(bool succeeded)>> on_complete)
{
    // NB: Steps only run once the worker's script has been evaluated, which is all RunScript waits for.
    if (step == HTML::ServiceWorkerLifecycleStep::RunScript) {
        on_complete->function()(true);
        return;
    }

    auto const& event_name = step == HTML::ServiceWorkerLifecycleStep::Install ? EventNames::install : EventNames::activate;

    // https://w3c.github.io/ServiceWorker/#installation-algorithm (step 11.3)
    // https://w3c.github.io/ServiceWorker/#activation-algorithm (step 10.2)
    // Queue a task task on installingWorker's / activeWorker's event loop using the DOM manipulation task source to
    // run the following steps:
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, realm().global_object(), GC::create_function(GC::Heap::the(), GC::captures(*this, event_name, on_complete), [](ServiceWorkerGlobalScope& self, Utf16FlyString& event_name, GC::Ref<GC::Function<void(bool)>>& on_complete) {
        auto& realm = self.realm();

        // 1. Let e be the result of creating an event with InstallEvent / ExtendableEvent.
        // 2. Initialize e's type attribute to install / activate.
        // FIXME: Create an InstallEvent for install, once it is implemented.
        auto event = ExtendableEvent::create(event_name, {}, HighResolutionTime::current_high_resolution_time(realm.global_object()));
        event->set_is_trusted(true);

        // 3. Dispatch e at installingWorker's / activeWorker's global object.
        self.dispatch_event(event);

        // 4. WaitForAsynchronousExtensions: Run the following substeps in parallel:
        //    1. Wait until e is not active.
        //    2. If e's timed out flag is set, set installFailed to true.
        //    3. Let p be the result of getting a promise to wait for all of e's extend lifetime promises.
        //    4. Upon rejection of p, set installFailed to true.
        // FIXME: Time out events that are extended for too long.
        event->wait_for_extend_lifetime_promises(realm, on_complete);
    }));
}

}
