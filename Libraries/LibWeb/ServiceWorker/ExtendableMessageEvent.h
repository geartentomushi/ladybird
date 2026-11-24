/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/ServiceWorker/ExtendableEvent.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#extendablemessageevent-interface
class ExtendableMessageEvent final : public ExtendableEvent {
    WEB_WRAPPABLE(ExtendableMessageEvent, ExtendableEvent);
    GC_DECLARE_ALLOCATOR(ExtendableMessageEvent);

public:
    struct Init {
        JS::Value data { JS::js_null() };
        String origin;
        JS::Value source { JS::js_null() };
        JS::Value ports { JS::js_undefined() };
    };

    [[nodiscard]] static GC::Ref<ExtendableMessageEvent> create(JS::Realm&, Utf16FlyString const& event_name, Init const&);

    virtual ~ExtendableMessageEvent() override;

    JS::Value data() const { return m_data; }
    String const& origin() const { return m_origin; }
    Utf16String last_event_id() const { return {}; }
    JS::Value source() const { return m_source; }
    JS::Value ports() const { return m_ports; }

private:
    ExtendableMessageEvent(Utf16FlyString const& event_name, Init const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Cell::Visitor&) override;

    JS::Value m_data;
    String m_origin;
    JS::Value m_source;
    JS::Value m_ports;
};

}
