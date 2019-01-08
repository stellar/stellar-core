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
#include "transactions/TransactionUtils.h"

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
ChangeTrustOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx)
{
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
            trustLine.erase();
            auto sourceAccount = loadSourceAccount(ltx, header);
            addNumEntries(header, sourceAccount, -1);
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
        auto issuer = stellar::loadAccountWithoutRecord(ltx, issuerID);
        if (!issuer)
        {
            innerResult().code(CHANGE_TRUST_NO_ISSUER);
            return false;
        }

        auto sourceAccount = loadSourceAccount(ltx, header);
        if (!addNumEntries(header, sourceAccount, 1))
        {
            innerResult().code(CHANGE_TRUST_LOW_RESERVE);
            return false;
        }

        LedgerEntry trustLineEntry;
        trustLineEntry.data.type(TRUSTLINE);
        auto& tl = trustLineEntry.data.trustLine();
        tl.accountID = getSourceID();
        tl.asset = mChangeTrust.line;
        tl.limit = mChangeTrust.limit;
        tl.balance = 0;
        if (!isAuthRequired(issuer))
        {
            tl.flags = AUTHORIZED_FLAG;
        }
        ltx.create(trustLineEntry);

        innerResult().code(CHANGE_TRUST_SUCCESS);
        return true;
    }
}

bool
ChangeTrustOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
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
    return true;
}
}
