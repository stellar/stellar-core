// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "crypto/SHA.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include <fmt/format.h>

namespace stellar
{

bool
isLive(LedgerEntry const& e, uint32_t cutoffLedger)
{
    releaseAssert(e.data.type() == TTL);
    return e.data.ttl().liveUntilLedgerSeq >= cutoffLedger;
}

LedgerKey
getTTLKey(LedgerEntry const& e)
{
    return getTTLKey(LedgerEntryKey(e));
}

LedgerKey
getTTLKey(LedgerKey const& e)
{
    releaseAssert(e.type() == CONTRACT_CODE || e.type() == CONTRACT_DATA);
    LedgerKey k;
    k.type(TTL);
    k.ttl().keyHash = sha256(xdr::xdr_to_opaque(e));
    return k;
}
};