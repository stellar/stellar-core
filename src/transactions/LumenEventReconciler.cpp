// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/LumenEventReconciler.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include <numeric>

namespace stellar
{

namespace
{

int64_t
calculateDeltaBalance(
    std::shared_ptr<InternalLedgerEntry const> const& genCurrent,
    std::shared_ptr<InternalLedgerEntry const> const& genPrevious)
{
    auto type = genCurrent ? genCurrent->type() : genPrevious->type();
    if (type == InternalLedgerEntryType::LEDGER_ENTRY)
    {
        auto const* current = genCurrent ? &genCurrent->ledgerEntry() : nullptr;
        auto const* previous =
            genPrevious ? &genPrevious->ledgerEntry() : nullptr;

        releaseAssert(current || previous);
        auto let = current ? current->data.type() : previous->data.type();
        switch (let)
        {
        case ACCOUNT:
            return (current ? current->data.account().balance : 0) -
                   (previous ? previous->data.account().balance : 0);
        default:
            break;
        }
    }
    return 0;
}

}

void
reconcileEvents(AccountID const& txSourceAccount, Operation const& operation,
                LedgerTxnDelta const& ltxDelta, OpEventManager& opEventManager)
{
    // Should only be called pre protocol 8
    int64_t deltaBalances = std::accumulate(
        ltxDelta.entry.begin(), ltxDelta.entry.end(), static_cast<int64_t>(0),
        [](int64_t lhs, decltype(ltxDelta.entry)::value_type const& rhs) {
            return lhs + calculateDeltaBalance(rhs.second.current,
                                               rhs.second.previous);
        });

    if (deltaBalances == 0)
    {
        return;
    }

    auto opSource = operation.sourceAccount
                        ? toAccountID(*operation.sourceAccount)
                        : txSourceAccount;

    Asset native(ASSET_TYPE_NATIVE);
    if (operation.body.type() == ACCOUNT_MERGE ||
        operation.body.type() == PAYMENT)
    {

        opEventManager.newMintEvent(native, makeAccountAddress(opSource),
                                    deltaBalances, false,
                                    true /*Insert mint at the beginning*/);
    }
    else if (operation.body.type() == PATH_PAYMENT_STRICT_RECEIVE)
    {
        opEventManager.newBurnEvent(native, makeAccountAddress(opSource),
                                    std::abs(deltaBalances));
    }
    else
    {
        CLOG_ERROR(
            Tx, "LumenEventReconciler: Unknown mint or burn. OperationType={}",
            operation.body.type());
    }
}
}
