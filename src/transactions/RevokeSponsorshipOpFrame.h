// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{
enum class SponsorshipResult;

class RevokeSponsorshipOpFrame : public OperationFrame
{
    bool isVersionSupported(uint32_t protocolVersion) const override;

    RevokeSponsorshipResult&
    innerResult()
    {
        return mResult.tr().revokeSponsorshipResult();
    }
    RevokeSponsorshipOp const& mRevokeSponsorshipOp;

    bool processSponsorshipResult(SponsorshipResult sr);

    bool updateLedgerEntrySponsorship(AbstractLedgerTxn& ltx);
    bool updateSignerSponsorship(AbstractLedgerTxn& ltx);

    bool tryRemoveEntrySponsorship(AbstractLedgerTxn& ltx,
                                   LedgerTxnHeader const& header,
                                   LedgerEntry& le, LedgerEntry& sponsoringAcc,
                                   LedgerEntry& sponsoredAcc);
    bool tryEstablishEntrySponsorship(AbstractLedgerTxn& ltx,
                                      LedgerTxnHeader const& header,
                                      LedgerEntry& le,
                                      LedgerEntry& sponsoringAcc,
                                      LedgerEntry& sponsoredAcc);

  public:
    RevokeSponsorshipOpFrame(Operation const& op, OperationResult& res,
                             TransactionFrame& parentTx);

    bool doApply(AbstractLedgerTxn& ltx) override;
    bool doCheckValid(uint32_t ledgerVersion) override;

    static RevokeSponsorshipResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().revokeSponsorshipResult().code();
    }
};
}
