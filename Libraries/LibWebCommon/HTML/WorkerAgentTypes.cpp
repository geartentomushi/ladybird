/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ServiceWorkerRegistrationSnapshot const& registration)
{
    TRY(encoder.encode(registration.scope_url));
    TRY(encoder.encode(registration.update_via_cache));
    return {};
}

template<>
ErrorOr<Web::HTML::ServiceWorkerRegistrationSnapshot> decode(Decoder& decoder)
{
    return Web::HTML::ServiceWorkerRegistrationSnapshot {
        .scope_url = TRY(decoder.decode<URL::URL>()),
        .update_via_cache = TRY(decoder.decode<Web::Bindings::ServiceWorkerUpdateViaCache>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::WorkerAgentStartRequest const& request)
{
    TRY(encoder.encode(request.url));
    TRY(encoder.encode(request.agent_type));
    TRY(encoder.encode(request.type));
    TRY(encoder.encode(request.credentials));
    TRY(encoder.encode(request.name));
    TRY(encoder.encode(request.extended_lifetime));
    TRY(encoder.encode(request.outside_port));
    TRY(encoder.encode(request.outside_settings));
    TRY(encoder.encode(request.caller_is_secure_context));
    TRY(encoder.encode(request.maximum_frames_per_second));
    TRY(encoder.encode(request.owner_token));
    TRY(encoder.encode(request.service_worker_registration));
    return {};
}

template<>
ErrorOr<Web::HTML::WorkerAgentStartRequest> decode(Decoder& decoder)
{
    return Web::HTML::WorkerAgentStartRequest {
        .url = TRY(decoder.decode<URL::URL>()),
        .agent_type = TRY(decoder.decode<Web::HTML::AgentType>()),
        .type = TRY(decoder.decode<Web::HTML::WorkerType>()),
        .credentials = TRY(decoder.decode<Web::HTML::RequestCredentials>()),
        .name = TRY(decoder.decode<String>()),
        .extended_lifetime = TRY(decoder.decode<bool>()),
        .outside_port = TRY(decoder.decode<Web::HTML::TransferDataEncoder>()),
        .outside_settings = TRY(decoder.decode<Web::HTML::SerializedEnvironmentSettingsObject>()),
        .caller_is_secure_context = TRY(decoder.decode<bool>()),
        .maximum_frames_per_second = TRY(decoder.decode<double>()),
        .owner_token = TRY(decoder.decode<Web::HTML::WorkerAgentOwnerToken>()),
        .service_worker_registration = TRY(decoder.decode<Optional<Web::HTML::ServiceWorkerRegistrationSnapshot>>()),
    };
}

}
