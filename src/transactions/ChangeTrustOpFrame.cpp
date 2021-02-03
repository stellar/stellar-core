// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ChangeTrustOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionUtils.h"
#include <Tracy.hpp>

namespace stellar
{

ChangeTrustOpFrame::ChangeTrustOpFrame(Operation const& op,
                                       OperationResult& res,
                                       TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mChangeTrust(mOperation.body.changeTrustOp())
{
}

bool
ChangeTrustOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "ChangeTrustOp apply", true);
    auto header = ltx.loadHeader();
    auto issuerID = getIssuer(mChangeTrust.line);

    if (header.current().ledgerVersion > 2)
    {
        // Note: No longer checking if issuer exists here, because if
        //     issuerID == getSourceID()
        // and issuer does not exist then this operation would have already
        // failed with opNO_ACCOUNT.
        if (issuerID == getSourceID())
        {
            // since version 3 it is not allowed to use CHANGE_TRUST on self
            innerResult().code(CHANGE_TRUST_SELF_NOT_ALLOWED);
            return false;
        }
    }
    else if (issuerID == getSourceID())
    {
        if (mChangeTrust.limit < INT64_MAX)
        {
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }
        else if (!stellar::loadAccountWithoutRecord(ltx, issuerID))
        {
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }
        return true;
    }

    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getSourceID();
    key.trustLine().asset = mChangeTrust.line;

    auto trustLine = ltx.load(key);
    if (trustLine)
    { // we are modifying an old trustline
        if (mChangeTrust.limit < getMinimumLimit(header, trustLine))
        {
            // Can't drop the limit below the balance you are holding with them
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        if (mChangeTrust.limit == 0)
        {
            // line gets deleted
            auto sourceAccount = loadSourceAccount(ltx, header);
            removeEntryWithPossibleSponsorship(ltx, header, trustLine.current(),
                                               sourceAccount);
            trustLine.erase();
        }
        else
        {
            auto issuer = stellar::loadAccountWithoutRecord(ltx, issuerID);
            if (!issuer)
            {
                innerResult().code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            trustLine.current().data.trustLine().limit = mChangeTrust.limit;
        }
        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
    else
    { // new trust line
        if (mChangeTrust.limit == 0)
        {
            innerResult().code(CHANGE_TRUST_INVALID_LIMIT);
            return false;
        }

        LedgerEntry trustLineEntry;
        trustLineEntry.data.type(TRUSTLINE);
        auto& tl = trustLineEntry.data.trustLine();
        tl.accountID = getSourceID();
        tl.asset = mChangeTrust.line;
        tl.limit = mChangeTrust.limit;
        tl.balance = 0;

        {
            auto issuer = stellar::loadAccountWithoutRecord(ltx, issuerID);
            if (!issuer)
            {
                innerResult().code(CHANGE_TRUST_NO_ISSUER);
                return false;
            }
            if (!isAuthRequired(issuer))
            {
                tl.flags = AUTHORIZED_FLAG;
            }
            if (isClawbackEnabledOnAccount(issuer))
            {
                tl.flags |= TRUSTLINE_CLAWBACK_ENABLED_FLAG;
            }
        }

        auto sourceAccount = loadSourceAccount(ltx, header);
        switch (createEntryWithPossibleSponsorship(ltx, header, trustLineEntry,
                                                   sourceAccount))
        {
        case SponsorshipResult::SUCCESS:
            break;
        case SponsorshipResult::LOW_RESERVE:
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        case SponsorshipResult::TOO_MANY_SUBENTRIES:
            mResult.code(opTOO_MANY_SUBENTRIES);
            return false;
        case SponsorshipResult::TOO_MANY_SPONSORING:
            mResult.code(opTOO_MANY_SPONSORING);
            return false;
        case SponsorshipResult::TOO_MANY_SPONSORED:
            // This is impossible right now because there is a limit on sub
            // entries, fall through and throw
        default:
            throw std::runtime_error("Unexpected result from "
                                     "createEntryWithPossibleSponsorship");
        }
        ltx.create(trustLineEntry);

        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mChangeTrust.limit < 0)
    {
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (!isAssetValid(mChangeTrust.line))
    {
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    if (ledgerVersion > 9)
    {
        if (mChangeTrust.line.type() == ASSET_TYPE_NATIVE)
        {
            innerResult().code(CHANGE_TRUST_MALFORMED);
            return false;
        }
    }

    if (ledgerVersion > 15 && getSourceID() == getIssuer(mChangeTrust.line))
    {
        innerResult().code(CHANGE_TRUST_MALFORMED);
        return false;
    }
    return true;
}
}
