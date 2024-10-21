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
    bool isOpSupported(LedgerHeader const& header) const override;

    RevokeSponsorshipResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().revokeSponsorshipResult();
    }
    RevokeSponsorshipOp const& mRevokeSponsorshipOp;

    bool processSponsorshipResult(SponsorshipResult sr,
                                  OperationResult& res) const;

    bool updateLedgerEntrySponsorship(AbstractLedgerTxn& ltx,
                                      OperationResult& res) const;
    bool updateSignerSponsorship(AbstractLedgerTxn& ltx,
                                 OperationResult& res) const;

    bool tryRemoveEntrySponsorship(AbstractLedgerTxn& ltx,
                                   LedgerTxnHeader const& header,
                                   LedgerEntry& le, LedgerEntry& sponsoringAcc,
                                   LedgerEntry& sponsoredAcc,
                                   OperationResult& res) const;
    bool tryEstablishEntrySponsorship(AbstractLedgerTxn& ltx,
                                      LedgerTxnHeader const& header,
                                      LedgerEntry& le,
                                      LedgerEntry& sponsoringAcc,
                                      LedgerEntry& sponsoredAcc,
                                      OperationResult& res) const;

  public:
    RevokeSponsorshipOpFrame(Operation const& op,
                             TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::shared_ptr<SorobanTxData> sorobanData) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static RevokeSponsorshipResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().revokeSponsorshipResult().code();
    }
};
}
