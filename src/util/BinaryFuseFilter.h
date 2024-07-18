#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/binaryfusefilter.h"
#include "util/NonCopyable.h"
#include "util/types.h"
#include "xdr/Stellar-types.h"

#include <cereal/archives/binary.hpp>
#include <xdrpp/xdrpp/cereal.h>

namespace stellar
{

// This class is a wrapper around the binary_fuse_t library that provides
// serialization for the XDR BinaryFuseFilter type and provides a deterministic
// LedgerKey interface.

// Only allow uint8_t, uint16_t, and uint32_t
template <typename T,
          typename = std::enable_if_t<std::is_unsigned<T>::value &&
                                      !std::is_same<T, uint64_t>::value>>
class BinaryFuseFilter : public NonMovableOrCopyable
{
  private:
    binary_fuse_t<T> mFilter;

    // Note: as part of filter construction, the internal filter seed might
    // rotate and no longer be the same as the input seed. The input seed must
    // be maintained outside of the filter and used to hash input keys in the
    // contain function to ensure deterministic hashing of input keys
    // during both populating and querying the filter.
    binary_fuse_seed_t const mInputSeed;

  public:
    explicit BinaryFuseFilter(LedgerKeySet const& keys,
                              binary_fuse_seed_t const& seed);
    explicit BinaryFuseFilter(SerializedBinaryFuseFilter const& xdrFilter);

    bool contains(LedgerKey const& key) const;

    bool operator==(BinaryFuseFilter<T> const& other) const;

    template <class Archive>
    void
    save(Archive& archive) const
    {
        SerializedBinaryFuseFilter xdrFilter;
        std::copy(mInputSeed.begin(), mInputSeed.end(),
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