#ifndef __BYTESLICE__
#define __BYTESLICE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <xdrpp/types.h>
#include <xdrpp/message.h>
#include <vector>
#include <string>

namespace stellar
{

/**
 * Transient adaptor type to permit passing a few different sorts of byte-container
 * types into encoder functions.
 */
class ByteSlice
{
    void const* mData;
    size_t const mSize;

public:

    unsigned char const* data() const { return static_cast<unsigned char const*>(mData); }
    unsigned char const* begin() const { return data(); }
    unsigned char const* end() const { return data() + size(); }
    unsigned char operator[](size_t i) const {
        if (i >= mSize)
            throw std::range_error("ByteSlice index out of bounds");
        return data()[i];
    }
    size_t size() const { return mSize; }
    bool empty() const { return mSize == 0; }

    template<uint32_t N>
    ByteSlice(xdr::opaque_array<N> const& arr) : mData(arr.data()), mSize(arr.size()) {}
    ByteSlice(xdr::msg_ptr const& p) : mData(p->data()), mSize(p->size()) {}
    ByteSlice(std::vector<uint8_t> const& bytes) : mData(bytes.data()), mSize(bytes.size()) {}
    ByteSlice(std::string const& bytes) : mData(bytes.data()), mSize(bytes.size()) {}
    ByteSlice(char const* str) : ByteSlice(std::string(str)) {}
    ByteSlice(void const* data, size_t size) : mData(data), mSize(size) {}
};

}
#endif
