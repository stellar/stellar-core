// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Invariants.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/Invariant.h"
#include "ledger/LedgerDelta.h"
#include "lib/util/format.h"
#include "util/Logging.h"
#include "xdrpp/printer.h"

namespace stellar
{

Invariants::Invariants(std::vector<std::unique_ptr<Invariant>> invariants)
    : mInvariants{std::move(invariants)}
{
}

Invariants::~Invariants() = default;

void
Invariants::check(TxSetFramePtr const& txSet, LedgerDelta const& delta) const
{
    for (auto const& invariant : mInvariants)
    {
        auto s = invariant->check(delta);
        if (s.empty())
        {
            continue;
        }

        auto transactions = TransactionSet{};
        txSet->toXDR(transactions);
        auto message =
            fmt::format(R"(invariant "{}" does not hold on ledger {}: {}{}{})",
                        invariant->getName(), delta.getHeader().ledgerSeq, s,
                        "\n", xdr::xdr_to_string(transactions));
        CLOG(FATAL, "Invariant") << message;
        throw InvariantDoesNotHold{message};
    }
}
}
