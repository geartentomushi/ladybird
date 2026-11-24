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
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(page)
{
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

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-registration
GC::Ref<ServiceWorkerRegistration> ServiceWorkerGlobalScope::registration()
{
    // The registration getter steps are to return the result of getting the service worker registration object
    // representing this's service worker's containing service worker registration in this's relevant settings object.
    VERIFY(m_registration);
    return HTML::relevant_settings_object(*this).get_service_worker_registration_object(*m_registration);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-serviceworker
GC::Ref<ServiceWorker> ServiceWorkerGlobalScope::service_worker()
{
    // The serviceWorker getter steps are to return the result of getting the service worker object that represents
    // this's service worker in this's relevant settings object.
    VERIFY(m_service_worker);
    return HTML::relevant_settings_object(*this).get_service_worker_object(m_service_worker.ptr());
}

void ServiceWorkerGlobalScope::set_up_service_worker(HTML::ServiceWorkerRegistrationSnapshot const& registration)
{
    auto storage_key = StorageAPI::obtain_a_storage_key(HTML::relevant_settings_object(*this));
    VERIFY(storage_key.has_value());
    m_registration = Registration::set(storage_key.release_value(), registration.scope_url, registration.update_via_cache);

    m_service_worker = adopt_ref(*new ServiceWorkerRecord);
    m_service_worker->script_url = url();
    m_service_worker->worker_type = type();
}

// https://w3c.github.io/ServiceWorker/#installation-algorithm
void ServiceWorkerGlobalScope::did_finish_install(bool succeeded)
{
    if (!m_registration)
        return;
    auto& event_loop = HTML::relevant_settings_object(*this).responsible_event_loop();

    // 14. If installFailed is true, then:
    if (!succeeded) {
        // 1. Run the Update Worker State algorithm passing registration's installing worker and "redundant".
        update_worker_state(event_loop, *m_service_worker, ServiceWorkerState::Redundant);

        // 2. Run the Update Registration State algorithm passing registration, "installing" and null.
        update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Installing, nullptr);
        return;
    }

    // 17. Run the Update Registration State algorithm passing registration, "waiting" and registration's installing worker.
    update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Waiting, m_service_worker);

    // 18. Run the Update Registration State algorithm passing registration, "installing" and null.
    update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Installing, nullptr);

    // 19. Run the Update Worker State algorithm passing registration's waiting worker and "installed".
    update_worker_state(event_loop, *m_service_worker, ServiceWorkerState::Installed);
}

void ServiceWorkerGlobalScope::run_lifecycle_step(HTML::ServiceWorkerLifecycleStep step, GC::Ref<GC::Function<void(bool succeeded)>> on_complete)
{
    // NB: Steps only run once the worker's script has been evaluated, which is all RunScript waits for.
    if (step == HTML::ServiceWorkerLifecycleStep::RunScript) {
        on_complete->function()(true);
        return;
    }

    auto const& event_name = step == HTML::ServiceWorkerLifecycleStep::Install ? EventNames::install : EventNames::activate;

    // NB: Mirror the state changes that the registration makes before it dispatches the event, and after it.
    if (m_registration) {
        auto& event_loop = HTML::relevant_settings_object(*this).responsible_event_loop();
        if (step == HTML::ServiceWorkerLifecycleStep::Install) {
            // https://w3c.github.io/ServiceWorker/#installation-algorithm (steps 1-3 and 8-9)
            update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Installing, m_service_worker);
            update_worker_state(event_loop, *m_service_worker, ServiceWorkerState::Installing);
            fire_update_found(event_loop, *m_registration);
            on_complete = GC::create_function(GC::Heap::the(), GC::captures(*this, on_complete), [](ServiceWorkerGlobalScope& self, GC::Ref<GC::Function<void(bool)>>& on_complete, bool succeeded) {
                self.did_finish_install(succeeded);
                on_complete->function()(succeeded);
            });
        } else {
            // https://w3c.github.io/ServiceWorker/#activation-algorithm (steps 3-5 and 12)
            update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Active, m_service_worker);
            update_registration_state(event_loop, *m_registration, RegistrationWorkerSlot::Waiting, nullptr);
            update_worker_state(event_loop, *m_service_worker, ServiceWorkerState::Activating);
            on_complete = GC::create_function(GC::Heap::the(), GC::captures(*this, on_complete), [](ServiceWorkerGlobalScope& self, GC::Ref<GC::Function<void(bool)>>& on_complete, bool succeeded) {
                update_worker_state(HTML::relevant_settings_object(self).responsible_event_loop(), *self.m_service_worker, ServiceWorkerState::Activated);
                on_complete->function()(succeeded);
            });
        }
    }

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
