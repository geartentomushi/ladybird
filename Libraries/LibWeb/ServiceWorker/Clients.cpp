/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/ServiceWorker/Clients.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Clients);

GC::Ref<Clients> Clients::create(ServiceWorkerGlobalScope& global_scope)
{
    return GC::Heap::the().allocate<Clients>(global_scope);
}

Clients::Clients(ServiceWorkerGlobalScope& global_scope)
    : m_global_scope(global_scope)
{
}

Clients::~Clients() = default;

void Clients::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
}

// https://w3c.github.io/ServiceWorker/#clients-claim
GC::Ref<WebIDL::Promise> Clients::claim()
{
    auto& realm = m_global_scope->realm();

    // 1. If the service worker is not an active worker, return a promise rejected with an "InvalidStateError" DOMException.
    if (!m_global_scope->is_active_worker())
        return WebIDL::create_rejected_promise(realm, WebIDL::InvalidStateError::create("claim() can only be called by an active service worker"_utf16));

    // 2. Let promise be a new promise.
    // 3. Run the following substeps in parallel:
    //     1. For each service worker client client whose origin is the same as the service worker's origin:
    //         ...
    //         6. If client's active service worker is not the service worker, then:
    //             1. Invoke Handle Service Worker Client Unload with client as the argument.
    //             2. Set client's active service worker to service worker.
    //             3. Invoke Notify Controller Change algorithm with client as the argument.
    //     2. Resolve promise with undefined.
    // 4. Return promise.
    // FIXME: Make the service worker the active service worker of its clients, once service workers can control clients.
    //        Until then, no client has an active service worker to replace, and fetches from clients still go to the
    //        network.
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

}
