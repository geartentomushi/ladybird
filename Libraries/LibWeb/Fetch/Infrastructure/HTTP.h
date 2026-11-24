/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Fetch/Infrastructure/RedirectTaint.h>
#include <LibWebCommon/Forward.h>

namespace Web::Fetch::Infrastructure {

// The default value takes the site-compatibility rules of a top-level site and of the site of the document or worker.
[[nodiscard]] String default_user_agent_value(LoaderConfig const&, URL::Origin const& top_level_origin, URL::Origin const& origin);
[[nodiscard]] String default_user_agent_value(LoaderConfig const&, Request const&);
[[nodiscard]] String environment_default_user_agent_value(LoaderConfig const&, HTML::EnvironmentSettingsObject const&);

struct UserAgentBrandVersion {
    String brand;
    String version;
    String full_version;
};

struct UserAgentClientHints {
    Vector<UserAgentBrandVersion> brands;
    StringView platform;
    bool mobile { false };
};

[[nodiscard]] UserAgentClientHints user_agent_client_hints(StringView effective_user_agent);

}
