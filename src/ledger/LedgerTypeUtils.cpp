// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"

namespace stellar
{

bool
isLive(LedgerEntry const& e, uint32_t expirationCutoff)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    releaseAssert(e.data.type() == EXPIRATION);
    return e.data.expiration().expirationLedgerSeq >= expirationCutoff;
#else
    return true;
#endif
}
};