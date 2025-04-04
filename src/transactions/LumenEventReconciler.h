#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/EventManager.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include <vector>

namespace stellar
{

struct LedgerTxnDelta;

class LumenEventReconciler
{
  public:
    static void reconcileEvents(AccountID const& txSourceAccount,
                                Operation const& operation,
                                OperationResult const& result,
                                LedgerTxnDelta const& ltxDelta,
                                OpEventManager& opEventManager);
};
}
