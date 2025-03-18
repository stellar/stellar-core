#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "transactions/EventManager.h"
#include "xdr/Stellar-ledger.h"
#include "xdr/Stellar-transaction.h"
#include <vector>

namespace stellar
{

// This method is for handling a pre-protocol 8 bug where XLM could be minted or
// burned. It will add additional events to reflect the actual behaviour of the
// protocol then.
void reconcileEvents(AccountID const& txSourceAccount,
                     Operation const& operation, LedgerTxnDelta const& ltxDelta,
                     OpEventManager& opEventManager);

}
