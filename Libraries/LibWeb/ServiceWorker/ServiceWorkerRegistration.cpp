/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ServiceWorker/Job.h>
#include <LibWeb/ServiceWorker/ServiceWorker.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerRegistration);

ServiceWorkerRegistration::ServiceWorkerRegistration(Registration const& registration)
    : DOM::EventTarget()
    , m_registration(registration)
{
}

void ServiceWorkerRegistration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_installing);
    visitor.visit(m_waiting);
    visitor.visit(m_active);
}

GC::Ref<ServiceWorkerRegistration> ServiceWorkerRegistration::create(Registration const& registration)
{
    return GC::Heap::the().allocate<ServiceWorkerRegistration>(registration);
}

// https://w3c.github.io/ServiceWorker/#navigator-service-worker-unregister
void ServiceWorkerRegistration::unregister(JS::Realm& realm, GC::Ref<WebIDL::Promise> promise)
{
    // 1. Let registration be the service worker registration.
    // 2. Let promise be a new promise.
    // 3. Let job be the result of running Create Job with unregister, registration's storage key, registration's
    //    scope url, null, promise, and this's relevant settings object.
    auto& settings = HTML::relevant_settings_object(promise->promise());
    auto job = Job::create(Job::Type::Unregister, m_registration->storage_key(), m_registration->scope_url(), {}, promise, settings);

    // 4. Invoke Schedule Job with job.
    schedule_job(realm.vm(), job);

    // 5. Return promise.
}

}
