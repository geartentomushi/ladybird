/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#clients-interface
class Clients : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Clients, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Clients);

public:
    [[nodiscard]] static GC::Ref<Clients> create(ServiceWorkerGlobalScope&);

    virtual ~Clients() override;

    GC::Ref<WebIDL::Promise> claim();

private:
    explicit Clients(ServiceWorkerGlobalScope&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<ServiceWorkerGlobalScope> m_global_scope;
};

}
