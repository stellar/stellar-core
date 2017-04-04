// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageDataOpFrame.h"
#include "database/Database.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/types.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

namespace
{

OperationResult
makeResult(ManageDataResultCode code)
{
    auto result = OperationResult{};
    result.code(opINNER);
    result.tr().type(MANAGE_DATA);
    result.tr().manageDataResult().code(code);
    return result;
}
}

ManageDataOpFrame::ManageDataOpFrame(Operation const& op,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, parentTx), mManageData(mOperation.body.manageDataOp())
{
}

OperationResult
ManageDataOpFrame::doApply(Application& app, LedgerDelta& delta,
                           LedgerManager& ledgerManager)
{
    if (app.getLedgerManager().getCurrentLedgerVersion() == 3)
    {
        throw std::runtime_error(
            "MANAGE_DATA not supported on ledger version 3");
    }

    Database& db = ledgerManager.getDatabase();

    auto dataFrame =
        DataFrame::loadData(mSourceAccount->getID(), mManageData.dataName, db);

    if (mManageData.dataValue)
    {
        if (!dataFrame)
        { // create a new data entry

            if (!mSourceAccount->addNumEntries(1, ledgerManager))
            {
                app.getMetrics()
                    .NewMeter({"op-manage-data", "invalid", "low reserve"},
                              "operation")
                    .Mark();
                return makeResult(MANAGE_DATA_LOW_RESERVE);
            }

            dataFrame = std::make_shared<DataFrame>();
            dataFrame->getData().accountID = mSourceAccount->getID();
            dataFrame->getData().dataName = mManageData.dataName;
            dataFrame->getData().dataValue = *mManageData.dataValue;

            dataFrame->storeAdd(delta, db);
            mSourceAccount->storeChange(delta, db);
        }
        else
        { // modify an existing entry
            delta.recordEntry(*dataFrame);
            dataFrame->getData().dataValue = *mManageData.dataValue;
            dataFrame->storeChange(delta, db);
        }
    }
    else
    { // delete an existing piece of data

        if (!dataFrame)
        {
            app.getMetrics()
                .NewMeter({"op-manage-data", "invalid", "not-found"},
                          "operation")
                .Mark();
            return makeResult(MANAGE_DATA_NAME_NOT_FOUND);
        }
        delta.recordEntry(*dataFrame);
        mSourceAccount->addNumEntries(-1, ledgerManager);
        mSourceAccount->storeChange(delta, db);
        dataFrame->storeDelete(delta, db);
    }

    app.getMetrics()
        .NewMeter({"op-manage-data", "success", "apply"}, "operation")
        .Mark();
    return makeResult(MANAGE_DATA_SUCCESS);
}

OperationResult
ManageDataOpFrame::doCheckValid(Application& app)
{
    if (app.getLedgerManager().getCurrentLedgerVersion() < 2)
    {
        app.getMetrics()
            .NewMeter(
                {"op-set-options", "invalid", "invalid-data-old-protocol"},
                "operation")
            .Mark();
        return makeResult(MANAGE_DATA_NOT_SUPPORTED_YET);
    }

    if ((mManageData.dataName.size() < 1) ||
        (!isString32Valid(mManageData.dataName)))
    {
        app.getMetrics()
            .NewMeter({"op-set-options", "invalid", "invalid-data-name"},
                      "operation")
            .Mark();
        return makeResult(MANAGE_DATA_INVALID_NAME);
    }

    return makeResult(MANAGE_DATA_SUCCESS);
}
}
