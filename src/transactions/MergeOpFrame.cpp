// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

using namespace soci;

namespace stellar
{

MergeOpFrame::MergeOpFrame(Operation const& op,
                           TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
{
}

ThresholdLevel
MergeOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::HIGH;
}

bool
MergeOpFrame::isSeqnumTooFar(AbstractLedgerTxn& ltx,
                             LedgerTxnHeader const& header,
                             AccountEntry const& sourceAccount) const
{
    // don't allow the account to be merged if recreating it would cause it
    // to jump backwards
    SequenceNumber maxSeq = getStartingSequenceNumber(header);

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        auto ltxe = loadMaxSeqNumToApply(ltx, getSourceID());
        if (ltxe &&
            ltxe.currentGeneralized().maxSeqNumToApplyEntry().maxSeqNum >=
                maxSeq)
        {
            return true;
        }
    }
    return sourceAccount.seqNum >= maxSeq;
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                      Hash const& sorobanBasePrngSeed, OperationResult& res,
                      std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "MergeOp apply", true);

    if (protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_16))
    {
        return doApplyBeforeV16(ltx, res);
    }
    else
    {
        return doApplyFromV16(ltx, res);
    }
}

bool
MergeOpFrame::doApplyBeforeV16(AbstractLedgerTxn& ltx,
                               OperationResult& res) const
{
    auto header = ltx.loadHeader();

    auto otherAccount =
        stellar::loadAccount(ltx, toAccountID(mOperation.body.destination()));
    if (!otherAccount)
    {
        innerResult(res).code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    int64_t sourceBalance = 0;
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_5) &&
        protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        // in versions < 8, merge account could be called with a stale account
        LedgerKey key(ACCOUNT);
        key.account().accountID = getSourceID();
        auto thisAccount = ltx.loadWithoutRecord(key);
        if (!thisAccount)
        {
            innerResult(res).code(ACCOUNT_MERGE_NO_ACCOUNT);
            return false;
        }

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_6))
        {
            sourceBalance = thisAccount.current().data.account().balance;
        }
    }

    auto sourceAccountEntry = loadSourceAccount(ltx, header);
    auto const& sourceAccount = sourceAccountEntry.current().data.account();
    // Only set sourceBalance here if it wasn't set in the previous block
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_6) ||
        protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_8))
    {
        sourceBalance = sourceAccount.balance;
    }

    if (isImmutableAuth(sourceAccountEntry))
    {
        innerResult(res).code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    if (sourceAccount.numSubEntries != sourceAccount.signers.size())
    {
        innerResult(res).code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10))
    {
        if (isSeqnumTooFar(ltx, header, sourceAccount))
        {
            innerResult(res).code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
            return false;
        }
    }

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_14))
    {
        if (loadSponsorshipCounter(ltx, getSourceID()))
        {
            innerResult(res).code(ACCOUNT_MERGE_IS_SPONSOR);
            return false;
        }

        if (getNumSponsoring(sourceAccountEntry.current()) > 0)
        {
            innerResult(res).code(ACCOUNT_MERGE_IS_SPONSOR);
            return false;
        }

        while (!sourceAccount.signers.empty())
        {
            removeSignerWithPossibleSponsorship(ltx, header,
                                                sourceAccount.signers.end() - 1,
                                                sourceAccountEntry);
        }
    }

    // "success" path starts
    if (!addBalance(header, otherAccount, sourceBalance))
    {
        innerResult(res).code(ACCOUNT_MERGE_DEST_FULL);
        return false;
    }

    removeEntryWithPossibleSponsorship(
        ltx, header, sourceAccountEntry.current(), sourceAccountEntry);
    sourceAccountEntry.erase();

    innerResult(res).code(ACCOUNT_MERGE_SUCCESS);
    innerResult(res).sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doApplyFromV16(AbstractLedgerTxn& ltx, OperationResult& res) const
{
    auto header = ltx.loadHeader();

    if (!stellar::loadAccount(ltx, toAccountID(mOperation.body.destination())))
    {
        innerResult(res).code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    auto sourceAccountEntry = loadSourceAccount(ltx, header);

    if (isImmutableAuth(sourceAccountEntry))
    {
        innerResult(res).code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    // use a lambda so we don't hold a reference to the AccountEntry
    auto sourceAccount = [&]() -> AccountEntry const& {
        return sourceAccountEntry.current().data.account();
    };

    if (sourceAccount().numSubEntries != sourceAccount().signers.size())
    {
        innerResult(res).code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (isSeqnumTooFar(ltx, header, sourceAccount()))
    {
        innerResult(res).code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
        return false;
    }

    if (loadSponsorshipCounter(ltx, getSourceID()))
    {
        innerResult(res).code(ACCOUNT_MERGE_IS_SPONSOR);
        return false;
    }

    if (getNumSponsoring(sourceAccountEntry.current()) > 0)
    {
        innerResult(res).code(ACCOUNT_MERGE_IS_SPONSOR);
        return false;
    }

    while (!sourceAccount().signers.empty())
    {
        removeSignerWithPossibleSponsorship(
            ltx, header, sourceAccount().signers.end() - 1, sourceAccountEntry);
    }

    // "success" path starts
    auto sourceBalance = sourceAccount().balance;
    {
        auto otherAccount = stellar::loadAccount(
            ltx, toAccountID(mOperation.body.destination()));
        if (!addBalance(header, otherAccount, sourceBalance))
        {
            innerResult(res).code(ACCOUNT_MERGE_DEST_FULL);
            return false;
        }
    }

    removeEntryWithPossibleSponsorship(
        ltx, header, sourceAccountEntry.current(), sourceAccountEntry);
    sourceAccountEntry.erase();

    innerResult(res).code(ACCOUNT_MERGE_SUCCESS);
    innerResult(res).sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doCheckValid(uint32_t ledgerVersion, OperationResult& res) const
{
    // makes sure not merging into self
    if (getSourceID() == toAccountID(mOperation.body.destination()))
    {
        innerResult(res).code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}
