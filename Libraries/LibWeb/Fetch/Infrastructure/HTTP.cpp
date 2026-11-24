/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWebCommon/Loader/LoaderConfigSnapshot.h>
#include <LibWebCommon/Loader/UserAgent.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#default-user-agent-value
String default_user_agent_value(LoaderConfig const& loader_config, URL::Origin const& top_level_origin, URL::Origin const& origin)
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    return loader_config.user_agent_for(top_level_origin, origin);
}

// The top-level origin of an environment, or the origin of its top-level creation URL, as the network partition key
// takes it.
template<typename EnvironmentType>
static Optional<URL::Origin> top_level_origin_of(EnvironmentType const& environment)
{
    if (environment.top_level_origin.has_value())
        return environment.top_level_origin;
    if (environment.top_level_creation_url.has_value())
        return environment.top_level_creation_url->origin();
    return {};
}

String default_user_agent_value(LoaderConfig const& loader_config, Request const& request)
{
    auto current_url_origin = request.current_url().origin();

    if (request.mode() == Request::Mode::Navigate) {
        // A top-level navigation request is for the top-level document it creates, whose site is that of the current
        // URL, even after a cross-site redirect.
        if (request.destination() == Request::Destination::Document)
            return default_user_agent_value(loader_config, current_url_origin, current_url_origin);

        // Any other navigation request is for a document under the top-level of its reserved client.
        Optional<URL::Origin> top_level_origin;
        if (auto const& reserved_client = request.reserved_client_snapshot())
            top_level_origin = top_level_origin_of(*reserved_client);
        return default_user_agent_value(loader_config, top_level_origin.value_or(current_url_origin), current_url_origin);
    }

    if (auto const& client = request.client_snapshot()) {
        auto top_level_origin = top_level_origin_of(*client);
        return default_user_agent_value(loader_config, top_level_origin.value_or(client->origin), client->origin);
    }

    return default_user_agent_value(loader_config, current_url_origin, current_url_origin);
}

// https://fetch.spec.whatwg.org/#environment-default-user-agent-value
String environment_default_user_agent_value(LoaderConfig const& loader_config, HTML::EnvironmentSettingsObject const& environment)
{
    // FIXME: Honor WebDriver BiDi emulated User-Agent.
    auto origin = environment.origin();
    auto top_level_origin = top_level_origin_of(environment);
    return default_user_agent_value(loader_config, top_level_origin.value_or(origin), origin);
}

UserAgentClientHints user_agent_client_hints(StringView user_agent)
{
    constexpr auto version = BROWSER_VERSION ""sv;
    auto major = version.substring_view(0, version.find('.').value_or(version.length()));
    auto seed = major.to_number<u32>().value_or(0);

    // https://wicg.github.io/ua-client-hints/#create-arbitrary-brands-section
    static constexpr Array separators { ' ', '(', ')', '-', '.', '/', ':', ';', '=', '?', '_' };
    auto arbitrary_version = (seed % 91) + 9;
    if (major.to_number<u32>() == arbitrary_version)
        ++arbitrary_version;
    UserAgentBrandVersion arbitrary_brand {
        MUST(String::formatted("An{}Arbitrary{}Brand", separators[seed % separators.size()], separators[(seed / separators.size() + 1) % separators.size()])),
        MUST(String::formatted("{}", arbitrary_version)),
        MUST(String::formatted("{}.0.0", arbitrary_version)),
    };

    UserAgentClientHints hints;
    // Do not undo a site-compatibility rule that removes the Ladybird product.
    bool has_ladybird_product = false;
    auto platform_start = user_agent.find('(');
    auto platform_end = platform_start.has_value() ? user_agent.find(')', *platform_start + 1) : Optional<size_t> {};
    if (platform_end.has_value()) {
        for (auto product : user_agent.substring_view(*platform_end + 1).split_view(' ')) {
            if (product.starts_with(BROWSER_NAME "/"sv)) {
                has_ladybird_product = true;
                break;
            }
        }
    } else {
        has_ladybird_product = user_agent.starts_with(BROWSER_NAME "/"sv);
    }
    if (!has_ladybird_product || seed % 2 == 0)
        hints.brands.append(arbitrary_brand);
    if (has_ladybird_product)
        hints.brands.append({ MUST(String::from_utf8(BROWSER_NAME ""sv)), MUST(String::from_utf8(major)), MUST(String::from_utf8(version)) });
    if (has_ladybird_product && seed % 2 != 0)
        hints.brands.append(move(arbitrary_brand));

    auto platform = platform_end.has_value() ? user_agent.substring_view(*platform_start + 1, *platform_end - *platform_start - 1) : StringView {};
    if (platform.contains("Android"sv))
        hints.platform = "Android"sv;
    else if (platform.contains("iPhone"sv) || platform.contains("iPad"sv) || platform.contains("iPod"sv))
        hints.platform = "iOS"sv;
    else if (platform.contains("Macintosh"sv))
        hints.platform = "macOS"sv;
    else if (platform.contains("Windows"sv))
        hints.platform = "Windows"sv;
    else if (platform.contains("CrOS"sv))
        hints.platform = "Chrome OS"sv;
    else if (platform.contains("Linux"sv))
        hints.platform = "Linux"sv;

    hints.mobile = user_agent.contains(" Mobile"sv) || platform.contains("iPhone"sv) || platform.contains("iPod"sv);
    return hints;
}

}
