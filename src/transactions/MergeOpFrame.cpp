// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/NewSponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>

using namespace stellar::SponsorshipUtils;

namespace stellar
{

MergeOpFrame::MergeOpFrame(Operation const& op, OperationResult& res,
                           TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
{
}

ThresholdLevel
MergeOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::HIGH;
}

bool
MergeOpFrame::isSeqnumTooFar(LedgerTxnHeader const& header,
                             AccountEntry const& sourceAccount)
{
    // don't allow the account to be merged if recreating it would cause it
    // to jump backwards
    SequenceNumber maxSeq = getStartingSequenceNumber(header);
    return sourceAccount.seqNum >= maxSeq;
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "MergeOp apply", true);

    if (ltx.loadHeader().current().ledgerVersion < 16)
    {
        return doApplyBeforeV16(ltx);
    }
    else
    {
        return doApplyFromV16(ltx);
    }
}

bool
MergeOpFrame::doApplyBeforeV16(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();

    auto otherAccount =
        stellar::loadAccount(ltx, toAccountID(mOperation.body.destination()));
    if (!otherAccount)
    {
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    int64_t sourceBalance = 0;
    if (header.current().ledgerVersion > 4 &&
        header.current().ledgerVersion < 8)
    {
        // in versions < 8, merge account could be called with a stale account
        LedgerKey key(ACCOUNT);
        key.account().accountID = getSourceID();
        auto thisAccount = ltx.loadWithoutRecord(key);
        if (!thisAccount)
        {
            innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
            return false;
        }

        if (header.current().ledgerVersion > 5)
        {
            sourceBalance = thisAccount.current().data.account().balance;
        }
    }

    auto sourceAccountEntry = loadSourceAccount(ltx, header);
    auto const& sourceAccount = sourceAccountEntry.current().data.account();
    // Only set sourceBalance here if it wasn't set in the previous block
    if (header.current().ledgerVersion <= 5 ||
        header.current().ledgerVersion >= 8)
    {
        sourceBalance = sourceAccount.balance;
    }

    if (isImmutableAuth(sourceAccountEntry))
    {
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    if (sourceAccount.numSubEntries != sourceAccount.signers.size())
    {
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (header.current().ledgerVersion >= 10)
    {
        if (isSeqnumTooFar(header, sourceAccount))
        {
            innerResult().code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
            return false;
        }
    }

    if (header.current().ledgerVersion >= 14)
    {
        if (loadSponsorshipCounter(ltx, getSourceID()))
        {
            innerResult().code(ACCOUNT_MERGE_IS_SPONSOR);
            return false;
        }

        if (getNumSponsoring(sourceAccountEntry.current()) > 0)
        {
            innerResult().code(ACCOUNT_MERGE_IS_SPONSOR);
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
        innerResult().code(ACCOUNT_MERGE_DEST_FULL);
        return false;
    }

    if (sourceAccountEntry.current().ext.v() == 1 &&
        sourceAccountEntry.current().ext.v1().sponsoringID &&
        toAccountID(mOperation.body.destination()) ==
            *sourceAccountEntry.current().ext.v1().sponsoringID)
    {
        throw std::runtime_error("merged account into sponsor");
    }

    OwnedEntrySponsorable oes(LedgerEntryKey(sourceAccountEntry.current()));
    oes.erase(ltx);

    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doApplyFromV16(AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();

    if (!stellar::loadAccount(ltx, toAccountID(mOperation.body.destination())))
    {
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    auto sourceAccountEntry = loadSourceAccount(ltx, header);

    if (isImmutableAuth(sourceAccountEntry))
    {
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    // use a lambda so we don't hold a reference to the AccountEntry
    auto sourceAccount = [&]() -> AccountEntry const& {
        return sourceAccountEntry.current().data.account();
    };

    if (sourceAccount().numSubEntries != sourceAccount().signers.size())
    {
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    if (isSeqnumTooFar(header, sourceAccount()))
    {
        innerResult().code(ACCOUNT_MERGE_SEQNUM_TOO_FAR);
        return false;
    }

    if (loadSponsorshipCounter(ltx, getSourceID()))
    {
        innerResult().code(ACCOUNT_MERGE_IS_SPONSOR);
        return false;
    }

    if (getNumSponsoring(sourceAccountEntry.current()) > 0)
    {
        innerResult().code(ACCOUNT_MERGE_IS_SPONSOR);
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
            innerResult().code(ACCOUNT_MERGE_DEST_FULL);
            return false;
        }
    }

    OwnedEntrySponsorable oes(LedgerEntryKey(sourceAccountEntry.current()));
    oes.erase(ltx);

    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    // makes sure not merging into self
    if (getSourceID() == toAccountID(mOperation.body.destination()))
    {
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}
