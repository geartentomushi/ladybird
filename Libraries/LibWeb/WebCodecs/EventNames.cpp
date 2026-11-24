/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebCodecs/EventNames.h>

namespace Web::WebCodecs::EventNames {

#define __ENUMERATE_WEBCODECS_ATTRIBUTE(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_WEBCODECS_ATTRIBUTES
#undef __ENUMERATE_WEBCODECS_ATTRIBUTE

}
