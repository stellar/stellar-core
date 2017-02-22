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

ManageDataOpFrame::ManageDataOpFrame(Operation const& op, OperationResult& res,
                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mManageData(mOperation.body.manageDataOp())
{
}

bool
ManageDataOpFrame::doApply(Application& app, LedgerDelta& delta,
                           LedgerManager& ledgerManager)
{
    // Workaround for invalid ledger database on live servers. These databases
    // were not upgraded properly at the time of ledger 9224665 (no lastmodified
    // field in accountdata table), so MANAGE_DATA operation failed with
    // txINTERNAL_ERROR error. This result must be replicated on each catchup.
    if (app.getNetworkType() == Application::NETWORK_PUBLIC &&
        ledgerManager.getCurrentLedgerHeader().ledgerSeq == 9224665)
    {
        throw std::runtime_error(
            "MANAGE_DATA on ledger 9224665 must fail in pubnet");
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
                innerResult().code(MANAGE_DATA_LOW_RESERVE);
                return false;
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
            innerResult().code(MANAGE_DATA_NAME_NOT_FOUND);
            return false;
        }
        delta.recordEntry(*dataFrame);
        mSourceAccount->addNumEntries(-1, ledgerManager);
        mSourceAccount->storeChange(delta, db);
        dataFrame->storeDelete(delta, db);
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
