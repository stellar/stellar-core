// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BeginSponsoringFutureReservesOpFrame.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

BeginSponsoringFutureReservesOpFrame::BeginSponsoringFutureReservesOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mBeginSponsoringFutureReservesOp(
          mOperation.body.beginSponsoringFutureReservesOp())
{
}

bool
BeginSponsoringFutureReservesOpFrame::isVersionSupported(
    uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

void
BeginSponsoringFutureReservesOpFrame::createSponsorship(AbstractLedgerTxn& ltx)
{
    InternalLedgerEntry gle(InternalLedgerEntryType::SPONSORSHIP);
    auto& se = gle.sponsorshipEntry();
    se.sponsoredID = mBeginSponsoringFutureReservesOp.sponsoredID;
    se.sponsoringID = getSourceID();

    auto res = ltx.create(gle);
    if (!res)
    {
        throw std::runtime_error("create failed");
    }
}

void
BeginSponsoringFutureReservesOpFrame::createSponsorshipCounter(
    AbstractLedgerTxn& ltx)
{
    InternalLedgerEntry gle(InternalLedgerEntryType::SPONSORSHIP_COUNTER);
    auto& sce = gle.sponsorshipCounterEntry();
    sce.sponsoringID = getSourceID();
    sce.numSponsoring = 1;

    auto res = ltx.create(gle);
    if (!res)
    {
        throw std::runtime_error("create failed");
    }
}

bool
BeginSponsoringFutureReservesOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    if (loadSponsorship(ltx, mBeginSponsoringFutureReservesOp.sponsoredID))
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED);
        return false;
    }

    if (loadSponsorship(ltx, getSourceID()))
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        return false;
    }
    if (loadSponsorshipCounter(ltx,
                               mBeginSponsoringFutureReservesOp.sponsoredID))
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        return false;
    }

    createSponsorship(ltx);

    auto ltxe = loadSponsorshipCounter(ltx, getSourceID());
    if (ltxe)
    {
        ++ltxe.currentGeneralized().sponsorshipCounterEntry().numSponsoring;
    }
    else
    {
        createSponsorshipCounter(ltx);
    }

    innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
    return true;
}

bool
BeginSponsoringFutureReservesOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mBeginSponsoringFutureReservesOp.sponsoredID == getSourceID())
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED);
        return false;
    }
    return true;
}
}
