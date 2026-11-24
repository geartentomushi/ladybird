/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/ExtendableEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::ServiceWorker {

using ExtendableEventInit = Bindings::ExtendableEventInit;

// https://w3c.github.io/ServiceWorker/#extendableevent-interface
class ExtendableEvent : public DOM::Event {
    WEB_WRAPPABLE(ExtendableEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ExtendableEvent);

public:
    [[nodiscard]] static GC::Ref<ExtendableEvent> create(Utf16FlyString const& event_name, ExtendableEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~ExtendableEvent() override;

    WebIDL::ExceptionOr<void> wait_until(GC::Ref<WebIDL::Promise>);

    Vector<GC::Ref<WebIDL::Promise>> const& extend_lifetime_promises() const { return m_extend_lifetime_promises; }

    // Calls on_complete once every extend lifetime promise, including ones added while waiting, has settled.
    void wait_for_extend_lifetime_promises(JS::Realm&, GC::Ref<GC::Function<void(bool succeeded)>> on_complete);

protected:
    ExtendableEvent(Utf16FlyString const& event_name, ExtendableEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    bool is_active() const;

    // https://w3c.github.io/ServiceWorker/#extendableevent-extend-lifetime-promises
    Vector<GC::Ref<WebIDL::Promise>> m_extend_lifetime_promises;

    // https://w3c.github.io/ServiceWorker/#extendableevent-pending-promises-count
    size_t m_pending_promises_count { 0 };

    // https://w3c.github.io/ServiceWorker/#extendableevent-timed-out-flag
    bool m_timed_out { false };

    GC::Ptr<JS::Realm> m_on_no_pending_promises_realm;
    GC::Ptr<GC::Function<void(bool succeeded)>> m_on_no_pending_promises;
};

}
