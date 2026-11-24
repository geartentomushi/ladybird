/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/DedicatedWorkerThreadLink.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/ServiceWorker/Registration.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope
class WEB_API ServiceWorkerGlobalScope : public HTML::WorkerGlobalScope {
    WEB_WRAPPABLE(ServiceWorkerGlobalScope, HTML::WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(ServiceWorkerGlobalScope);

public:
    virtual ~ServiceWorkerGlobalScope() override;

    void set_oninstall(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> oninstall();

    void set_onactivate(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onactivate();

    void set_onfetch(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onfetch();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();

    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    [[nodiscard]] GC::Ref<CookieStore::CookieStore> cookie_store();

    GC::Ref<WebIDL::Promise> skip_waiting();

    GC::Ref<ServiceWorkerRegistration> registration();
    GC::Ref<ServiceWorker> service_worker();

    void set_up_service_worker(HTML::ServiceWorkerRegistrationSnapshot const&);

    // The message event target of the service worker's inside port. It fires the messages that arrive there at this
    // global as ExtendableMessageEvents.
    GC::Ref<DOM::EventTarget> message_event_relay();

    // Runs a step of the service worker's lifecycle that its registration asked for from another agent.
    void run_lifecycle_step(HTML::ServiceWorkerLifecycleStep, GC::Ref<GC::Function<void(bool succeeded)>> on_complete);

protected:
    explicit ServiceWorkerGlobalScope(GC::Ref<Web::Page>);

private:
    virtual void initialize_web_interfaces_impl() override;
    virtual void visit_edges(Cell::Visitor&) override;

    void did_finish_install(bool succeeded);

    GC::Ptr<CookieStore::CookieStore> m_cookie_store;
    GC::Ptr<DOM::EventTarget> m_message_event_relay;

    // NB: The service worker registration and the service worker live in the agent of the client that registered the
    //     service worker. This agent keeps its own copy of both, which follows the lifecycle steps run on it.
    // FIXME: Share them with the client's agent, once service workers are managed by the user agent.
    RefPtr<Registration> m_registration;
    RefPtr<ServiceWorkerRecord> m_service_worker;
};

}
