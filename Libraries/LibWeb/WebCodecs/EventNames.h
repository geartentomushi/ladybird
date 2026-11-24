/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>

namespace Web::WebCodecs::EventNames {

#define ENUMERATE_WEBCODECS_ATTRIBUTES \
    __ENUMERATE_WEBCODECS_ATTRIBUTE(dequeue)

#define __ENUMERATE_WEBCODECS_ATTRIBUTE(name) extern Utf16FlyString const& name;
ENUMERATE_WEBCODECS_ATTRIBUTES
#undef __ENUMERATE_WEBCODECS_ATTRIBUTE

}
