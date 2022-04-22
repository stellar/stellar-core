#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "util/types.h"
#include "xdr/Stellar-types.h"

#include <cereal/cereal.hpp>
#include <iosfwd>
#include <string>

namespace stellar
{
// Type wrapper around Hash. Internally, buckets are refered to and managed by
// hash. Since there are multiple possible hashes associated with a bucket
// depending on file sort order, this wrapper class helps clarify a consistent
// identifying hash.
class HashID
{
    friend class std::hash<HashID>;
    Hash mHash;

  public:
    HashID() : mHash()
    {
    }

    HashID(Hash const& hash) : mHash(hash)
    {
    }

    HashID(std::string const& hash) : mHash(hexToBin256(hash))
    {
    }

    bool
    operator==(HashID const& rhs) const
    {
        return this->mHash == rhs.mHash;
    }

    bool
    operator<(HashID const& rhs) const
    {
        return this->mHash < rhs.mHash;
    }

    bool
    operator()(HashID const& rhs) const
    {
        return *this < rhs;
    }

    // Returns hex encoded string of hash
    std::string
    toHex() const
    {
        return binToHex(mHash);
    }

    // Returns 6 character prefix of hex encoded Hash for logging
    std::string
    toLogString() const
    {
        return hexAbbrev(mHash);
    }

    bool
    isZero() const
    {
        return ::stellar::isZero(mHash);
    }

    inline Hash const&
    raw() const
    {
        return mHash;
    }
};
}

namespace std
{
template <> struct hash<stellar::HashID>
{
    size_t
    operator()(stellar::HashID const& k) const noexcept
    {
        std::hash<string> h;
        return h(k.toHex());
    }
};
}