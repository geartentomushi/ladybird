/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/ServiceWorker.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRecord.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(E)  \
    E(onstatechange, HTML::EventNames::statechange) \
    E(onerror, HTML::EventNames::error)

namespace Web::Bindings {

struct StructuredSerializeOptions;

}

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworker-interface
class ServiceWorker : public DOM::EventTarget {
    WEB_WRAPPABLE(ServiceWorker, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(ServiceWorker);

public:
    [[nodiscard]] static GC::Ref<ServiceWorker> create(ServiceWorkerRecord*);

    virtual ~ServiceWorker() override;

    Utf16String script_url() const;
    ServiceWorkerState service_worker_state() const { return m_state; }
    void set_service_worker_state(ServiceWorkerState state) { m_state = state; }

    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer);
    WebIDL::ExceptionOr<void> post_message(JS::Realm&, JS::Value message, Bindings::StructuredSerializeOptions const&);

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)       \
    void set_##attribute_name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* attribute_name();
    ENUMERATE_SERVICE_WORKER_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

private:
    ServiceWorker(ServiceWorkerRecord*);

    ServiceWorkerState m_state { ServiceWorkerState::Parsed };
    RefPtr<ServiceWorkerRecord> m_service_worker_record;
};

}
