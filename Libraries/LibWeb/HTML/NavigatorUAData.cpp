/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/NavigatorUAData.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Loader/UserAgent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigatorUAData);

GC::Ref<NavigatorUAData> NavigatorUAData::create(JS::Realm& realm, Fetch::Infrastructure::UserAgentClientHints hints)
{
    return realm.heap().allocate<NavigatorUAData>(realm, move(hints));
}

NavigatorUAData::NavigatorUAData(JS::Realm& realm, Fetch::Infrastructure::UserAgentClientHints hints)
    : m_realm(realm)
    , m_hints(move(hints))
{
}

GC::Ptr<Bindings::Wrappable> NavigatorUAData::relevant_global_impl() const
{
    return Bindings::wrappable_impl_from(&m_realm->global_object());
}

void NavigatorUAData::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
}

Vector<Bindings::NavigatorUABrandVersion> NavigatorUAData::brands() const
{
    Vector<Bindings::NavigatorUABrandVersion> brands;
    for (auto const& brand : m_hints.brands) {
        brands.append({
            .brand = Utf16String::from_utf8(brand.brand),
            .version = Utf16String::from_utf8(brand.version),
        });
    }
    return brands;
}

Utf16String NavigatorUAData::platform() const
{
    return Utf16String::from_utf8(m_hints.platform);
}

static Utf16String architecture()
{
#if ARCH(X86_64) || ARCH(I386)
    return "x86"_utf16;
#elif ARCH(AARCH64)
    return "arm"_utf16;
#else
    return {};
#endif
}

// https://wicg.github.io/ua-client-hints/#getHighEntropyValues
void NavigatorUAData::get_high_entropy_values(JS::Realm& realm, Vector<Utf16String> const& hints, GC::Ref<WebIDL::Promise> promise) const
{
    Bindings::UADataValues values;
    values.brands = brands();
    values.mobile = mobile();
    values.platform = platform();

    if (auto const* window = window_from_global_object(m_realm->global_object()); window && !window->associated_document().is_allowed_to_use_feature(DOM::PolicyControlledFeature::ClientHintsUAHighEntropyValues)) {
        WebIDL::resolve_promise(promise, Bindings::ua_data_values_to_value(realm, values));
        return;
    }

    if (hints.contains_slow("architecture"_utf16))
        values.architecture = architecture();
    if (hints.contains_slow("bitness"_utf16))
        values.bitness = sizeof(void*) == 8 ? "64"_utf16 : "32"_utf16;
    if (hints.contains_slow("formFactors"_utf16))
        values.form_factors = Vector<Utf16String> { mobile() ? "Mobile"_utf16 : "Desktop"_utf16 };
    if (hints.contains_slow("fullVersionList"_utf16)) {
        Vector<Bindings::NavigatorUABrandVersion> full_version_list;
        for (auto const& brand : m_hints.brands) {
            full_version_list.append({
                .brand = Utf16String::from_utf8(brand.brand),
                .version = Utf16String::from_utf8(brand.full_version),
            });
        }
        values.full_version_list = move(full_version_list);
    }
    if (hints.contains_slow("model"_utf16))
        values.model = Utf16String {};
    if (hints.contains_slow("platformVersion"_utf16))
        values.platform_version = Utf16String {};
    if (hints.contains_slow("uaFullVersion"_utf16)) {
        values.ua_full_version = Utf16String {};
        for (auto const& brand : m_hints.brands) {
            if (brand.brand == BROWSER_NAME ""sv) {
                values.ua_full_version = Utf16String::from_utf8(brand.full_version);
                break;
            }
        }
    }
    if (hints.contains_slow("wow64"_utf16))
        values.wow64 = false;

    auto result = Bindings::ua_data_values_to_value(realm, values);
    queue_a_task(Task::Source::Permissions, nullptr, nullptr,
        GC::create_function(realm.heap(), GC::captures(realm, promise, result), [](auto& realm, auto& promise, auto& result) {
            TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::resolve_promise(realm, promise, result);
        }));
}

// https://wicg.github.io/ua-client-hints/#toJSON
Bindings::UALowEntropyJSON NavigatorUAData::to_json() const
{
    return {
        .brands = brands(),
        .mobile = mobile(),
        .platform = platform(),
    };
}

}
