// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageDataOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{

using namespace std;

ManageDataOpFrame::ManageDataOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageData(mOperation.body.manageDataOp())
{
}

bool
ManageDataOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx)
{
    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion == 3)
    {
        throw std::runtime_error(
            "MANAGE_DATA not supported on ledger version 3");
    }

    auto data = stellar::loadData(ltx, getSourceID(), mManageData.dataName);
    if (mManageData.dataValue)
    {
        if (!data)
        { // create a new data entry
            auto sourceAccount = loadSourceAccount(ltx, header);
            if (!addNumEntries(header, sourceAccount, 1))
            {
                innerResult().code(MANAGE_DATA_LOW_RESERVE);
                return false;
            }

            LedgerEntry newData;
            newData.data.type(DATA);
            auto& dataEntry = newData.data.data();
            dataEntry.accountID = getSourceID();
            dataEntry.dataName = mManageData.dataName;
            dataEntry.dataValue = *mManageData.dataValue;
            ltx.create(newData);
        }
        else
        { // modify an existing entry
            data.current().data.data().dataValue = *mManageData.dataValue;
        }
    }
    else
    { // delete an existing piece of data
        if (!data)
        {
            innerResult().code(MANAGE_DATA_NAME_NOT_FOUND);
            return false;
        }
        data.erase();
        auto sourceAccount = loadSourceAccount(ltx, header);
        addNumEntries(header, sourceAccount, -1);
    }

    innerResult().code(MANAGE_DATA_SUCCESS);

    return true;
}

bool
ManageDataOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (ledgerVersion < 2)
    {
        innerResult().code(MANAGE_DATA_NOT_SUPPORTED_YET);
        return false;
    }

    if ((mManageData.dataName.size() < 1) ||
        (!isString32Valid(mManageData.dataName)))
    {
        innerResult().code(MANAGE_DATA_INVALID_NAME);
        return false;
    }

    return true;
}
}
