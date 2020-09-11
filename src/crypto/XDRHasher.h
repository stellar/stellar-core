#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <xdrpp/endian.h>
#include <xdrpp/marshal.h>

namespace stellar
{

// CRTP base class to use in XDR-hashing xdrpp-compatible archiver objects. All
// you need to provide is a `void Derived::hashBytes(unsigned char const*,
// size_t)` member.
template <typename Derived> struct XDRHasher
{
    // Buffer and batch calls to underlying hasher a _little_ bit.
    unsigned char mBuf[256] = {0};
    size_t mLen = 0;
    size_t
    available() const
    {
        return sizeof(mBuf) - mLen;
    }
    void
    queueOrHash(unsigned char const* u, size_t sz)
    {
        if (u == nullptr)
        {
            if (sz != 0)
            {
                throw std::invalid_argument(
                    "nullptr input with nonzero length");
            }
            return;
        }
        if (sz > available())
        {
            flush();
            if (sz > available())
            {
                static_cast<Derived*>(this)->hashBytes(u, sz);
                return;
            }
        }
        memcpy(mBuf + mLen, u, sz);
        mLen += sz;
    }
    void
    flush()
    {
        if (mLen != 0)
        {
            static_cast<Derived*>(this)->hashBytes(mBuf, mLen);
            mLen = 0;
        }
    }
    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint32_t, typename xdr::xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        auto u = xdr::swap32le(xdr::xdr_traits<T>::to_uint(t));
        queueOrHash(reinterpret_cast<unsigned char const*>(&u), sizeof(u));
    }

    template <typename T>
    typename std::enable_if<std::is_same<
        std::uint64_t, typename xdr::xdr_traits<T>::uint_type>::value>::type
    operator()(T t)
    {
        auto u = xdr::swap64le(xdr::xdr_traits<T>::to_uint(t));
        queueOrHash(reinterpret_cast<unsigned char const*>(&u), sizeof(u));
    }

    template <typename T>
    typename std::enable_if<xdr::xdr_traits<T>::is_bytes>::type
    operator()(const T& t)
    {
        size_t len = t.size();
        if (xdr::xdr_traits<T>::variable_nelem)
        {
            (*this)(static_cast<uint32_t>(len));
        }
        if (len != 0)
        {
            queueOrHash(reinterpret_cast<unsigned char const*>(t.data()), len);
            if (len & 3)
            {
                static unsigned char const pad[3] = {0};
                queueOrHash(pad, 4 - (len & 3));
            }
        }
    }

    template <typename T>
    typename std::enable_if<xdr::xdr_traits<T>::is_class ||
                            xdr::xdr_traits<T>::is_container>::type
    operator()(const T& t)
    {
        xdr::xdr_traits<T>::save(*this, t);
    }
};
}
