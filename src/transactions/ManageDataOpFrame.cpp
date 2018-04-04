// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageDataOpFrame.h"
#include "database/Database.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerState.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/types.h"

#include "ledger/AccountReference.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

ManageDataOpFrame::ManageDataOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageData(mOperation.body.manageDataOp())
{
}

bool
ManageDataOpFrame::doApply(Application& app, LedgerState& ls)
{
    auto header = ls.loadHeader();
    if (getCurrentLedgerVersion(header) == 3)
    {
        throw std::runtime_error(
            "MANAGE_DATA not supported on ledger version 3");
    }
    header->invalidate();

    auto dataRef = loadData(ls, getSourceID(), mManageData.dataName);
    if (mManageData.dataValue)
    {
        if (!dataRef)
        { // create a new data entry
            auto sourceAccount = loadSourceAccount(ls);
            auto header = ls.loadHeader();
            if (!sourceAccount.addNumEntries(header, 1))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-data", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_DATA_LOW_RESERVE);
                return false;
            }
            header->invalidate();

            LedgerEntry newData;
            newData.data.type(DATA);
            newData.data.data().accountID = getSourceID();
            newData.data.data().dataName = mManageData.dataName;
            newData.data.data().dataValue = *mManageData.dataValue;
            dataRef = ls.create(newData);
        }
        else
        { // modify an existing entry
            dataRef->entry()->data.data().dataValue = *mManageData.dataValue;
        }
    }
    else
    { // delete an existing piece of data
        if (!dataRef)
        {
            app.getMetrics()
                .NewMeter({"op-manage-data", "invalid", "not-found"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_DATA_NAME_NOT_FOUND);
            return false;
        }
        dataRef->erase();
        auto sourceAccount = loadSourceAccount(ls);
        auto header = ls.loadHeader();
        sourceAccount.addNumEntries(header, -1);
        header->invalidate();
    }

    innerResult().code(MANAGE_DATA_SUCCESS);

    app.getMetrics()
        .NewMeter({"op-manage-data", "success", "apply"}, "operation")
        .Mark();
    return true;
}

bool
ManageDataOpFrame::doCheckValid(Application& app, uint32_t ledgerVersion)
{
    if (ledgerVersion < 2)
    {
        app.getMetrics()
            .NewMeter(
                {"op-set-options", "invalid", "invalid-data-old-protocol"},
                "operation")
            .Mark();
        innerResult().code(MANAGE_DATA_NOT_SUPPORTED_YET);
        return false;
    }

    if ((mManageData.dataName.size() < 1) ||
        (!isString32Valid(mManageData.dataName)))
    {
        app.getMetrics()
            .NewMeter({"op-set-options", "invalid", "invalid-data-name"},
                      "operation")
            .Mark();
        innerResult().code(MANAGE_DATA_INVALID_NAME);
        return false;
    }

    return true;
}
}
