// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "database/Database.h"
#include "transactions/ManageDataOpFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/DataFrame.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerEntries.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/types.h"

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
ManageDataOpFrame::doApply(Application& app, LedgerDelta& ledgerDelta,
                           LedgerManager& ledgerManager)
{
    if (app.getLedgerManager().getCurrentLedgerVersion() == 3)
    {
        throw std::runtime_error(
            "MANAGE_DATA not supported on ledger version 3");
    }

    auto sourceFrame = AccountFrame{*mSourceAccount};
    auto existingData = ledgerDelta.loadData(sourceFrame.getAccountID(), mManageData.dataName);
    if (mManageData.dataValue)
    {
        if (!existingData)
        { // create a new data entry
            if (!sourceFrame.addNumEntries(1, ledgerManager))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-data", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                innerResult().code(MANAGE_DATA_LOW_RESERVE);
                return false;
            }
            ledgerDelta.updateEntry(sourceFrame);

            auto dataFrame = DataFrame{sourceFrame.getAccountID(), mManageData.dataName, *mManageData.dataValue};
            ledgerDelta.addEntry(dataFrame);
        }
        else
        { // modify an existing entry
            auto dataFrame = DataFrame{*existingData};
            dataFrame.setValue(*mManageData.dataValue);
            ledgerDelta.updateEntry(dataFrame);
        }
    }
    else
    { // delete an existing piece of data
        if (!existingData)
        {
            app.getMetrics()
                .NewMeter({"op-manage-data", "invalid", "not-found"},
                          "operation")
                .Mark();
            innerResult().code(MANAGE_DATA_NAME_NOT_FOUND);
            return false;
        }
        sourceFrame.addNumEntries(-1, ledgerManager);
        ledgerDelta.updateEntry(sourceFrame);

        auto dataFrame = DataFrame{*existingData};
        ledgerDelta.deleteEntry(dataFrame.getKey());
    }

    innerResult().code(MANAGE_DATA_SUCCESS);

    app.getMetrics()
        .NewMeter({"op-manage-data", "success", "apply"}, "operation")
        .Mark();
    return true;
}

bool
ManageDataOpFrame::doCheckValid(Application& app)
{
    if (app.getLedgerManager().getCurrentLedgerVersion() < 2)
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
