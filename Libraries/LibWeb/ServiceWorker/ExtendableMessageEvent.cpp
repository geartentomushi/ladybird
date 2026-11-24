/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/ServiceWorker/ExtendableMessageEvent.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ExtendableMessageEvent);

GC::Ref<ExtendableMessageEvent> ExtendableMessageEvent::create(JS::Realm& realm, Utf16FlyString const& event_name, Init const& init)
{
    return GC::Heap::the().allocate<ExtendableMessageEvent>(event_name, init, HighResolutionTime::current_high_resolution_time(realm.global_object()));
}

ExtendableMessageEvent::ExtendableMessageEvent(Utf16FlyString const& event_name, Init const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : ExtendableEvent(event_name, {}, time_stamp)
    , m_data(init.data)
    , m_origin(init.origin)
    , m_source(init.source)
    , m_ports(init.ports)
{
}

ExtendableMessageEvent::~ExtendableMessageEvent() = default;

void ExtendableMessageEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
    visitor.visit(m_source);
    visitor.visit(m_ports);
}

}
