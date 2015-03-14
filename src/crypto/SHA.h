#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "crypto/ByteSlice.h"
#include <memory>

namespace stellar
{

// Plain SHA256
uint256 sha256(ByteSlice const& bin);

// SHA256 in incremental mode, for large inputs.
class SHA256
{
    struct Impl;
    std::unique_ptr<Impl> mImpl;

  public:
    SHA256();
    ~SHA256();
    void add(ByteSlice const& bin);
    uint256 finish();
};
}
