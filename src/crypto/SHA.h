#ifndef __SHA__
#define __SHA__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include <sodium.h>

namespace stellar
{

class ByteSlice;

// Plain SHA256
uint256
sha256(ByteSlice const& bin);

// SHA512/256: SHA512 truncated to 256 bits
uint256
sha512_256(ByteSlice const& bin);

// SHA512/256 in incremental mode, for large inputs.
class SHA512_256
{
    crypto_hash_sha512_state mState;
    bool mFinished;
public:
    SHA512_256();
    void add(ByteSlice const& bin);
    uint256 finish();
};

}

#endif
