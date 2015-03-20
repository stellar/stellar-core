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
  public:
    static std::unique_ptr<SHA256> create();
    virtual ~SHA256() {};
    virtual void add(ByteSlice const& bin) = 0;
    virtual uint256 finish() = 0;
};
}
