// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BeginSponsoringFutureReservesOpFrame.h"
#include "ledger/InternalLedgerEntry.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/TransactionUtils.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>

namespace stellar
{

BeginSponsoringFutureReservesOpFrame::BeginSponsoringFutureReservesOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mBeginSponsoringFutureReservesOp(
          mOperation.body.beginSponsoringFutureReservesOp())
{
}

bool
BeginSponsoringFutureReservesOpFrame::isOpSupported(
    LedgerHeader const& header) const
{
    return protocolVersionStartsFrom(header.ledgerVersion,
                                     ProtocolVersion::V_14);
}

void
BeginSponsoringFutureReservesOpFrame::createSponsorship(
    AbstractLedgerTxn& ltx) const
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
    AbstractLedgerTxn& ltx) const
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
BeginSponsoringFutureReservesOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "BeginSponsoringFutureReservesOpFrame apply", true);
    if (loadSponsorship(ltx, mBeginSponsoringFutureReservesOp.sponsoredID))
    {
        innerResult(res).code(
            BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED);
        return false;
    }

    if (loadSponsorship(ltx, getSourceID()))
    {
        innerResult(res).code(BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        return false;
    }
    if (loadSponsorshipCounter(ltx,
                               mBeginSponsoringFutureReservesOp.sponsoredID))
    {
        innerResult(res).code(BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
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

    innerResult(res).code(BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
    return true;
}

bool
BeginSponsoringFutureReservesOpFrame::doCheckValid(uint32_t ledgerVersion,
                                                   OperationResult& res) const
{
    if (mBeginSponsoringFutureReservesOp.sponsoredID == getSourceID())
    {
        innerResult(res).code(BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED);
        return false;
    }
    return true;
}
}
