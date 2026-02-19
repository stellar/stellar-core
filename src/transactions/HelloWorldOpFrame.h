// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#ifdef XDR_HELLO_WORLD

#include "transactions/OperationFrame.h"

namespace stellar
{
class AbstractLedgerTxn;

class HelloWorldOpFrame : public OperationFrame
{
    HelloWorldResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().helloWorldResult();
    }

    ThresholdLevel getThresholdLevel() const override;

  public:
    HelloWorldOpFrame(Operation const& op, TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 OperationResult& res,
                 OperationMetaBuilder& opMeta) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static HelloWorldResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().helloWorldResult().code();
    }
};
}

#endif // XDR_HELLO_WORLD
