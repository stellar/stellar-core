// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/SponsorFutureReservesOpFrame.h"
#include "ledger/GeneralizedLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

SponsorFutureReservesOpFrame::SponsorFutureReservesOpFrame(
    Operation const& op, OperationResult& res, TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mSponsorFutureReservesOp(
          mOperation.body.beginSponsoringFutureReservesOp())
{
}

bool
SponsorFutureReservesOpFrame::isVersionSupported(uint32_t protocolVersion) const
{
    return protocolVersion >= 14;
}

void
SponsorFutureReservesOpFrame::createSponsorship(AbstractLedgerTxn& ltx)
{
    GeneralizedLedgerEntry gle(GeneralizedLedgerEntryType::SPONSORSHIP);
    auto& se = gle.sponsorshipEntry();
    se.sponsoredID = mSponsorFutureReservesOp.sponsoredID;
    se.sponsoringID = getSourceID();

    auto res = ltx.create(gle);
    if (!res)
    {
        throw std::runtime_error("create failed");
    }
}

void
SponsorFutureReservesOpFrame::createSponsorshipCounter(AbstractLedgerTxn& ltx)
{
    GeneralizedLedgerEntry gle(GeneralizedLedgerEntryType::SPONSORSHIP_COUNTER);
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
SponsorFutureReservesOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    if (loadSponsorship(ltx, mSponsorFutureReservesOp.sponsoredID))
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED);
        return false;
    }

    if (loadSponsorship(ltx, getSourceID()))
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        return false;
    }
    if (loadSponsorshipCounter(ltx, mSponsorFutureReservesOp.sponsoredID))
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
SponsorFutureReservesOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mSponsorFutureReservesOp.sponsoredID == getSourceID())
    {
        innerResult().code(BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED);
        return false;
    }
    return true;
}
}
