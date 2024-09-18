// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "OfferExchange.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <Tracy.hpp>
#include <algorithm>

#include "main/Application.h"

namespace stellar
{

using namespace std;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApplyBeforeV14(AbstractLedgerTxn& ltx,
                                       OperationResult& res) const
{
    auto header = ltx.loadHeader();
    if (mCreateAccount.startingBalance <
        getMinBalance(header.current(), 0, 0, 0))
    { // not over the minBalance to make an account
        innerResult(res).code(CREATE_ACCOUNT_LOW_RESERVE);
        return false;
    }

    bool doesAccountExist =
        protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_8) ||
        (bool)ltx.loadWithoutRecord(accountKey(getSourceID()));

    auto sourceAccount = loadSourceAccount(ltx, header);
    if (getAvailableBalance(header, sourceAccount) <
        mCreateAccount.startingBalance)
    { // they don't have enough to send
        innerResult(res).code(CREATE_ACCOUNT_UNDERFUNDED);
        return false;
    }

    if (!doesAccountExist)
    {
        throw std::runtime_error("modifying account that does not exist");
    }

    auto ok =
        addBalance(header, sourceAccount, -mCreateAccount.startingBalance);
    releaseAssertOrThrow(ok);

    LedgerEntry newAccountEntry;
    newAccountEntry.data.type(ACCOUNT);
    auto& newAccount = newAccountEntry.data.account();
    newAccount.thresholds[0] = 1;
    newAccount.accountID = mCreateAccount.destination;
    newAccount.seqNum = getStartingSequenceNumber(header);
    newAccount.balance = mCreateAccount.startingBalance;
    ltx.create(newAccountEntry);

    innerResult(res).code(CREATE_ACCOUNT_SUCCESS);
    return true;
}

bool
CreateAccountOpFrame::doApplyFromV14(AbstractLedgerTxn& ltxOuter,
                                     OperationResult& res) const
{
    LedgerTxn ltx(ltxOuter);
    auto header = ltx.loadHeader();

    LedgerEntry newAccountEntry;
    newAccountEntry.data.type(ACCOUNT);
    auto& newAccount = newAccountEntry.data.account();
    newAccount.thresholds[0] = 1;
    newAccount.accountID = mCreateAccount.destination;
    newAccount.seqNum = getStartingSequenceNumber(header);
    newAccount.balance = mCreateAccount.startingBalance;

    LedgerTxnEntry empty;
    switch (
        createEntryWithPossibleSponsorship(ltx, header, newAccountEntry, empty))
    {
    case SponsorshipResult::SUCCESS:
        break;
    case SponsorshipResult::LOW_RESERVE:
        innerResult(res).code(CREATE_ACCOUNT_LOW_RESERVE);
        return false;
    case SponsorshipResult::TOO_MANY_SUBENTRIES:
        res.code(opTOO_MANY_SUBENTRIES);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORING:
        res.code(opTOO_MANY_SPONSORING);
        return false;
    case SponsorshipResult::TOO_MANY_SPONSORED:
        // This is impossible right now because there is a limit on sub
        // entries, fall through and throw
    default:
        throw std::runtime_error("Unexpected result from "
                                 "createEntryWithPossibleSponsorship");
    }

    auto sourceAccount = loadAccount(ltx, getSourceID());
    if (getAvailableBalance(header, sourceAccount) <
        mCreateAccount.startingBalance)
    { // they don't have enough to send
        innerResult(res).code(CREATE_ACCOUNT_UNDERFUNDED);
        return false;
    }

    auto ok =
        addBalance(header, sourceAccount, -mCreateAccount.startingBalance);
    releaseAssertOrThrow(ok);

    ltx.create(newAccountEntry);
    innerResult(res).code(CREATE_ACCOUNT_SUCCESS);

    ltx.commit();
    return true;
}

bool
CreateAccountOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                              Hash const& sorobanBasePrngSeed,
                              OperationResult& res,
                              std::shared_ptr<SorobanTxData> sorobanData) const
{
    ZoneNamedN(applyZone, "CreateAccountOp apply", true);
    if (stellar::loadAccount(ltx, mCreateAccount.destination))
    {
        innerResult(res).code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }

    if (protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_14))
    {
        return doApplyBeforeV14(ltx, res);
    }
    else
    {
        return doApplyFromV14(ltx, res);
    }
}

bool
CreateAccountOpFrame::doCheckValid(uint32_t ledgerVersion,
                                   OperationResult& res) const
{
    int64_t minStartingBalance =
        protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_14) ? 0 : 1;
    if (mCreateAccount.startingBalance < minStartingBalance)
    {
        innerResult(res).code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    if (mCreateAccount.destination == getSourceID())
    {
        innerResult(res).code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}

void
CreateAccountOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(accountKey(mCreateAccount.destination));
}
}
