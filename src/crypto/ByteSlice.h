#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>
#include <vector>
#include <xdrpp/message.h>
#include <xdrpp/types.h>

namespace stellar
{

/**
 * Transient adaptor type to permit passing a few different sorts of
 * byte-container types into encoder functions.
 */
class ByteSlice
{
    void const* mData;
    size_t const mSize;

  public:
    unsigned char const*
    data() const
    {
        return static_cast<unsigned char const*>(mData);
    }
    unsigned char const*
    begin() const
    {
        return data();
    }
    unsigned char const*
    end() const
    {
        return data() + size();
    }
    unsigned char operator[](size_t i) const
    {
        if (i >= mSize)
            throw std::range_error("ByteSlice index out of bounds");
        return data()[i];
    }
    size_t
    size() const
    {
        return mSize;
    }
    bool
    empty() const
    {
        return mSize == 0;
    }

    template <uint32_t N>
    ByteSlice(xdr::opaque_array<N> const& arr)
        : mData(arr.data()), mSize(arr.size())
    {
    }
    ByteSlice(xdr::msg_ptr const& p) : mData(p->data()), mSize(p->size())
    {
    }
    ByteSlice(std::vector<uint8_t> const& bytes)
        : mData(bytes.data()), mSize(bytes.size())
    {
    }
    ByteSlice(std::string const& bytes)
        : mData(bytes.data()), mSize(bytes.size())
    {
    }
    ByteSlice(char const* str) : ByteSlice((void const*)str, strlen(str))
    {
    }
    ByteSlice(void const* data, size_t size) : mData(data), mSize(size)
    {
    }
};
}
