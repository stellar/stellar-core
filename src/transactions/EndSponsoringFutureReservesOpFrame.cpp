// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/EndSponsoringFutureReservesOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

EndSponsoringFutureReservesOpFrame::EndSponsoringFutureReservesOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

bool
EndSponsoringFutureReservesOpFrame::isVersionSupported(
    uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

bool
EndSponsoringFutureReservesOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    auto sponsorship = loadSponsorship(ltx, getSourceID());
    if (!sponsorship)
    {
        innerResult().code(END_SPONSORING_FUTURE_RESERVES_NOT_SPONSORED);
        return false;
    }

    auto const& sponsoringID =
        sponsorship.currentGeneralized().sponsorshipEntry().sponsoringID;
    auto sponsorshipCounter = loadSponsorshipCounter(ltx, sponsoringID);
    if (!sponsorshipCounter)
    {
        throw std::runtime_error("no sponsorship counter");
    }

    {
        auto& sce =
            sponsorshipCounter.currentGeneralized().sponsorshipCounterEntry();
        if (sce.numSponsoring == 0)
        {
            throw std::runtime_error("bad sponsorship counter");
        }
        if (--sce.numSponsoring == 0)
        {
            sponsorshipCounter.erase();
        }
    }

    sponsorship.erase();
    innerResult().code(END_SPONSORING_FUTURE_RESERVES_SUCCESS);
    return true;
}

bool
EndSponsoringFutureReservesOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}
}
