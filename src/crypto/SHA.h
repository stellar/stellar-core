#ifndef __SHA__
#define __SHA__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include <sodium.h>
#include <vector>

namespace stellar
{

// Plain SHA256
uint256
sha256(void const *bin, size_t size);

template <typename T>
uint256
sha256(typename std::enable_if<sizeof(typename T::value_type) == 1, T>::type const& bin)
{
    return sha256(bin.data(), bin.size());
}


// SHA512/256: SHA512 truncated to 256 bits
uint256
sha512_256(void const *bin, size_t size);

template <typename T>
uint256
sha512_256(typename std::enable_if<sizeof(typename T::value_type) == 1, T>::type const& bin)
{
    return sha512_256(bin.data(), bin.size());
}

// SHA512/256 in incremental mode, for large inputs.
class SHA512_256
{
    crypto_hash_sha512_state mState;
    bool mFinished;
public:
    SHA512_256();
    template <typename T>
    void add(typename std::enable_if<sizeof(typename T::value_type) == 1, T>::type const& bin)
    {
        add(bin.data(), bin.size());
    }
    void add(void const* buf, size_t size);
    uint256 finish();
};

}

#endif
