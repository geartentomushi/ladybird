/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/NavigatorUAData.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>

namespace Web::HTML {

class NavigatorUAData final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(NavigatorUAData, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(NavigatorUAData);

public:
    static GC::Ref<NavigatorUAData> create(JS::Realm&, Fetch::Infrastructure::UserAgentClientHints, bool uses_user_agent_preset);

    bool uses_user_agent_preset() const { return m_uses_user_agent_preset; }
    Vector<Bindings::NavigatorUABrandVersion> brands() const;
    bool mobile() const { return m_hints.mobile; }
    Utf16String platform() const;
    void get_high_entropy_values(JS::Realm&, Vector<Utf16String> const&, GC::Ref<WebIDL::Promise>) const;
    Bindings::UALowEntropyJSON to_json() const;

private:
    NavigatorUAData(JS::Realm&, Fetch::Infrastructure::UserAgentClientHints, bool uses_user_agent_preset);
    bool should_suppress_user_agent_hints() const;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<JS::Realm> m_realm;
    Fetch::Infrastructure::UserAgentClientHints m_hints;
    bool m_uses_user_agent_preset;
};

}
