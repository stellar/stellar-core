// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCPDriver.h"

#include <algorithm>

#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"

namespace stellar
{

std::string
SCPDriver::getValueString(Value const& v) const
{
    uint256 valueHash = sha256(xdr::xdr_to_opaque(v));

    return hexAbbrev(valueHash);
}

// values used to switch hash function between priority and neighborhood checks
static const uint32 hash_N = 1;
static const uint32 hash_P = 2;

uint64
SCPDriver::computeHash(uint64 slotIndex, bool isPriority, int32_t roundNumber,
                       uint256 const& nodeID, Value const& prev)
{
    auto h = SHA256::create();
    h->add(xdr::xdr_to_opaque(slotIndex));
    h->add(xdr::xdr_to_opaque(prev));
    h->add(xdr::xdr_to_opaque(isPriority ? hash_P : hash_N));
    h->add(xdr::xdr_to_opaque(roundNumber));
    h->add(nodeID);
    uint256 t = h->finish();
    uint64 res = 0;
    for (int i = 0; i < sizeof(res); i++)
    {
        res = res << 8 | t[i];
    }
    return res;
}
}
