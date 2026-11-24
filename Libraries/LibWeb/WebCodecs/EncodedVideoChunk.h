/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <LibWeb/Bindings/EncodedVideoChunk.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebCodecs {

using EncodedVideoChunkType = Bindings::EncodedVideoChunkType;

// https://w3c.github.io/webcodecs/#encodedvideochunk-interface
class WEB_API EncodedVideoChunk final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(EncodedVideoChunk, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(EncodedVideoChunk);

public:
    static WebIDL::ExceptionOr<GC::Ref<EncodedVideoChunk>> construct_impl(Bindings::EncodedVideoChunkInit const&);
    virtual ~EncodedVideoChunk() override;

    EncodedVideoChunkType type() const { return m_type; }
    i64 timestamp() const { return m_timestamp; }
    Optional<u64> duration() const { return m_duration; }
    WebIDL::UnsignedLong byte_length() const { return m_byte_length; }

    WebIDL::ExceptionOr<void> copy_to(WebIDL::BufferSourceVariant const& destination);

    ReadonlyBytes data() const { return m_internal_data; }

private:
    EncodedVideoChunk(EncodedVideoChunkType, i64 timestamp, Optional<u64> duration, ByteBuffer data);

    // https://w3c.github.io/webcodecs/#dom-encodedvideochunk-internal-data-slot
    ByteBuffer m_internal_data;

    // https://w3c.github.io/webcodecs/#dom-encodedvideochunk-type-slot
    EncodedVideoChunkType m_type;

    // https://w3c.github.io/webcodecs/#dom-encodedvideochunk-timestamp-slot
    i64 m_timestamp { 0 };

    // https://w3c.github.io/webcodecs/#dom-encodedvideochunk-duration-slot
    Optional<u64> m_duration;

    // https://w3c.github.io/webcodecs/#dom-encodedvideochunk-byte-length-slot
    WebIDL::UnsignedLong m_byte_length { 0 };
};

}
