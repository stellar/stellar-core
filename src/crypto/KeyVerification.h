#pragma once

// Copyright 2014-2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

// public key utility functions
namespace PubKeyUtils
{

// Return true iff `signature` is valid for `bin` under `key`.
bool verifySig(PublicKey const& key, Signature const& signature,
               ByteSlice const& bin);

void clearVerifySigCache();
void flushVerifySigCacheCounts(uint64_t& hits, uint64_t& misses);
}
}
