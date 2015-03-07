#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC


#include <memory>
#include "ledger/LedgerMaster.h"
#include "ledger/AccountFrame.h"
#include "generated/StellarXDR.h"
#include "util/types.h"

namespace stellar
{
    class Application;
    class LedgerMaster;
    class LedgerDelta;

    class TransactionFrame;

    class OperationFrame
    {
    protected:
        Operation const& mOperation;
        TransactionFrame &mParentTx;
        AccountFrame::pointer mSourceAccount;
        OperationResult &mResult;

        bool checkSignature();

        virtual bool doCheckValid(Application& app) = 0;
        virtual bool doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster) = 0;
        virtual int32_t getNeededThreshold();

    public:

        static std::shared_ptr<OperationFrame> makeHelper(Operation const& op,
            OperationResult &res, TransactionFrame &parentTx);

        OperationFrame(Operation const& op, OperationResult &res, TransactionFrame& parentTx);
        OperationFrame(OperationFrame const &) = delete;

        AccountFrame& getSourceAccount() { assert(mSourceAccount); return *mSourceAccount; }

        uint256 const& getSourceID();

        // load account if needed
        // returns true on success
        bool loadAccount(Application& app);

        OperationResult &getResult() { return mResult; }
        OperationResultCode getResultCode();

        bool checkValid(Application& app);

        bool apply(LedgerDelta& delta, Application& app);

        Operation const& getOperation() const { return mOperation; }
    };
}

