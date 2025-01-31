#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/binaryfusefilter.h"
#include "util/NonCopyable.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-types.h"

#include <cereal/archives/binary.hpp>

namespace stellar
{

// This class is a wrapper around the binary_fuse_t library that provides
// serialization for the XDR BinaryFuseFilter type and provides a deterministic
// LedgerKey interface.
template <typename T> class BinaryFuseFilter : public NonMovableOrCopyable
{
    static_assert(std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                      std::is_same_v<T, uint32_t>,
                  "Binary Fuse Filter only supports 8, 16, or 32 bit width");

  private:
    binary_fuse_t<T> const mFilter;

    // Note: This is the seed used to hash keys going into the filter, and we
    // need to preserve it
    // to hash keys we're looking up in the filter. The filter also has its own
    // seed determining its overall structure, which _starts_ with this seed,
    // but which may be rotated a bit in during repeated attempts to
    // successfully populate the filter. So we have to keep both seeds.
    binary_fuse_seed_t const mHashSeed;

  public:
    // keyHashes is the SipHash24 digest on the keys to insert into the filter.
    // Seed is the random seed used to initialize the hash function
    explicit BinaryFuseFilter(std::vector<uint64_t>& keyHashes,
                              binary_fuse_seed_t const& seed);
    explicit BinaryFuseFilter(SerializedBinaryFuseFilter const& xdrFilter);

    bool contains(LedgerKey const& key) const;

    bool operator==(BinaryFuseFilter<T> const& other) const;

    template <class Archive>
    void
    save(Archive& archive) const
    {
        SerializedBinaryFuseFilter xdrFilter;
        std::copy(mHashSeed.begin(), mHashSeed.end(),
                  xdrFilter.inputHashSeed.seed.begin());

        mFilter.copyTo(xdrFilter);
        archive(xdrFilter);
    }

    template <class Archive>
    void
    load(Archive& archive)
    {
        SerializedBinaryFuseFilter xdrFilter;
        archive(xdrFilter);
    }

    template <class Archive>
    static void
    load_and_construct(Archive& ar,
                       cereal::construct<BinaryFuseFilter<T>>& construct)
    {
        SerializedBinaryFuseFilter xdrFilter;
        ar(xdrFilter);
        construct(xdrFilter);
    }
};

// False positive rate: 1/256
// Approximate bits per entry: 9
typedef BinaryFuseFilter<uint8_t> BinaryFuseFilter8;

// False positive rate: 1/65536
// Approximate bits per entry: 18
typedef BinaryFuseFilter<uint16_t> BinaryFuseFilter16;

// False positive rate: 1 / 4 billion
// Approximate bits per entry: 36
typedef BinaryFuseFilter<uint32_t> BinaryFuseFilter32;
}