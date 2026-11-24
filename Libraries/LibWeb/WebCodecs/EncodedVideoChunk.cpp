/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebCodecs/EncodedVideoChunk.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebCodecs {

GC_DEFINE_ALLOCATOR(EncodedVideoChunk);

// https://w3c.github.io/webcodecs/#dom-encodedvideochunk-encodedvideochunk
WebIDL::ExceptionOr<GC::Ref<EncodedVideoChunk>> EncodedVideoChunk::construct_impl(Bindings::EncodedVideoChunkInit const& init)
{
    // 1. If init.transfer contains more than one reference to the same ArrayBuffer, then throw a DataCloneError
    //    DOMException.
    HashTable<JS::ArrayBuffer const*> transfer_set;
    for (auto const& transferable : init.transfer) {
        if (transfer_set.set(transferable.ptr()) != HashSetResult::InsertedNewEntry)
            return WebIDL::DataCloneError::create("The transfer list contains the same ArrayBuffer more than once"_utf16);
    }

    // 2. For each transferable in init.transfer:
    for (auto const& transferable : init.transfer) {
        // 1. If [[Detached]] internal slot is true, then throw a DataCloneError DOMException.
        if (transferable->is_detached())
            return WebIDL::DataCloneError::create("The transfer list contains a detached ArrayBuffer"_utf16);

        // NB: DetachArrayBuffer in step 4 fails for a buffer with a detach key, such as a WebAssembly.Memory buffer.
        //     Reject it here, before any buffer in the list has been detached.
        if (!transferable->detach_key().is_undefined())
            return WebIDL::DataCloneError::create("The transfer list contains an ArrayBuffer that cannot be detached"_utf16);
    }

    // 3. Let chunk be a new EncodedVideoChunk object, initialized as follows
    //    1. Assign init.type to [[type]].
    //    2. Assign init.timestamp to [[timestamp]].
    //    3. If duration is present in init, assign init.duration to [[duration]]. Otherwise, assign null to
    //       [[duration]].
    //    4. Assign init.data.byteLength to [[byte length]];
    //    5. If init.transfer contains an ArrayBuffer referenced by init.data the User Agent MAY choose to:
    //       1. Let resource be a new media resource referencing sample data in init.data.
    //    6. Otherwise:
    //       1. Assign a copy of init.data to [[internal data]].
    // NB: The data is always copied.
    WebIDL::BufferSource data { init.data };
    auto internal_data_or_error = WebIDL::get_buffer_source_copy(data);
    if (internal_data_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Unable to allocate the chunk data"_utf16 };
    auto chunk = GC::Heap::the().allocate<EncodedVideoChunk>(init.type, init.timestamp, init.duration, internal_data_or_error.release_value());

    // 4. For each transferable in init.transfer:
    for (auto const& transferable : init.transfer) {
        // 1. Perform DetachArrayBuffer on transferable
        TRY(JS::detach_array_buffer(transferable->vm(), *transferable));
    }

    // 5. Return chunk.
    return chunk;
}

EncodedVideoChunk::EncodedVideoChunk(EncodedVideoChunkType type, i64 timestamp, Optional<u64> duration, ByteBuffer data)
    : m_internal_data(move(data))
    , m_type(type)
    , m_timestamp(timestamp)
    , m_duration(duration)
    , m_byte_length(static_cast<WebIDL::UnsignedLong>(m_internal_data.size()))
{
}

EncodedVideoChunk::~EncodedVideoChunk() = default;

// https://w3c.github.io/webcodecs/#dom-encodedvideochunk-copyto
WebIDL::ExceptionOr<void> EncodedVideoChunk::copy_to(WebIDL::BufferSourceVariant const& destination)
{
    WebIDL::BufferSource destination_buffer { destination };
    auto array_buffer = destination_buffer.viewed_array_buffer();
    auto destination_byte_length = array_buffer && !array_buffer->is_detached() && !destination_buffer.is_out_of_bounds() ? destination_buffer.byte_length() : 0;

    // 1. If [[byte length]] is greater than the [[byte length]] of destination, throw a TypeError.
    if (m_byte_length > destination_byte_length)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The destination buffer is too small"_utf16 };

    // 2. Copy the [[internal data]] into destination.
    if (m_byte_length > 0)
        array_buffer->overwrite(destination_buffer.byte_offset(), m_internal_data.data(), m_byte_length);
    return {};
}

}
