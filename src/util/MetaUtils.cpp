// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/MetaUtils.h"
#include "crypto/SHA.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include <algorithm>

namespace
{
using namespace stellar;
struct CmpLedgerEntryChanges
{
    int
    remap(LedgerEntryChangeType let)
    {
        // order that we want is:
        // LEDGER_ENTRY_STATE, LEDGER_ENTRY_CREATED,
        // LEDGER_ENTRY_UPDATED, LEDGER_ENTRY_REMOVED
        static constexpr std::array<int, 4> reindex = {1, 2, 3, 0};
        releaseAssert(let >= 0 && let < 4);
        return reindex[let];
    }

    LedgerKey
    getKeyFromChange(LedgerEntryChange const& change)
    {
        LedgerKey res;
        switch (change.type())
        {
        case LEDGER_ENTRY_STATE:
            res = LedgerEntryKey(change.state());
            break;
        case LEDGER_ENTRY_CREATED:
            res = LedgerEntryKey(change.created());
            break;
        case LEDGER_ENTRY_UPDATED:
            res = LedgerEntryKey(change.updated());
            break;
        case LEDGER_ENTRY_REMOVED:
            res = change.removed();
            break;
        }
        return res;
    }

    bool
    operator()(LedgerEntryChange const& l, LedgerEntryChange const& r)
    {
        auto lT =
            std::make_tuple(getKeyFromChange(l), remap(l.type()), xdrSha256(l));
        auto rT =
            std::make_tuple(getKeyFromChange(r), remap(r.type()), xdrSha256(r));
        return lT < rT;
    }
};

void
sortChanges(LedgerEntryChanges& c)
{
    std::sort(c.begin(), c.end(), CmpLedgerEntryChanges());
}

void
normalizeOps(xdr::xvector<OperationMeta>& oms)
{
    for (auto& om : oms)
    {
        sortChanges(om.changes);
    }
}
}

namespace stellar
{

void
normalizeMeta(LedgerCloseMeta& m)
{
    switch (m.v())
    {
    case 0:
        for (auto& trm : m.v0().txProcessing)
        {
            normalizeMeta(trm);
        }
        for (auto& up : m.v0().upgradesProcessing)
        {
            sortChanges(up.changes);
        }
        break;
    case 1:
        for (auto& trm : m.v1().txProcessing)
        {
            normalizeMeta(trm);
        }
        for (auto& up : m.v1().upgradesProcessing)
        {
            sortChanges(up.changes);
        }
        break;
    case 2:
        for (auto& trm : m.v2().txProcessing)
        {
            normalizeMeta(trm);
        }
        for (auto& up : m.v2().upgradesProcessing)
        {
            sortChanges(up.changes);
        }
        std::sort(m.v2().evictedPersistentLedgerEntries.begin(),
                  m.v2().evictedPersistentLedgerEntries.end());
        std::sort(m.v2().evictedTemporaryLedgerKeys.begin(),
                  m.v2().evictedTemporaryLedgerKeys.end());
        break;
    default:
        releaseAssert(false);
    }
}

void
normalizeMeta(TransactionResultMeta& m)
{
    sortChanges(m.feeProcessing);
    normalizeMeta(m.txApplyProcessing);
}

void
normalizeMeta(TransactionMeta& m)
{
    switch (m.v())
    {
    case 0:
        normalizeOps(m.operations());
        break;
    case 1:
        sortChanges(m.v1().txChanges);
        normalizeOps(m.v1().operations);
        break;
    case 2:
        sortChanges(m.v2().txChangesBefore);
        sortChanges(m.v2().txChangesAfter);
        normalizeOps(m.v2().operations);
        break;
    case 3:
        sortChanges(m.v3().txChangesBefore);
        sortChanges(m.v3().txChangesAfter);
        normalizeOps(m.v3().operations);
        break;
    default:
        releaseAssert(false);
    }
}

}