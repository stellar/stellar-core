// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{

class SimulationTransactionFrame : public TransactionFrame
{
    TransactionResult mSimulationResult;

  protected:
    bool isTooEarly(LedgerTxnHeader const& header) const override;
    bool isTooLate(LedgerTxnHeader const& header) const override;

    std::shared_ptr<OperationFrame> makeOperation(Operation const& op,
                                                  OperationResult& res,
                                                  size_t index) override;

    bool isBadSeq(int64_t seqNum) const override;

    int64_t getFee(LedgerHeader const& header, int64_t baseFee) const override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;

  public:
    SimulationTransactionFrame(Hash const& networkID,
                               TransactionEnvelope const& envelope,
                               TransactionResult simulationResult);
    SimulationTransactionFrame(TransactionFrame const&) = delete;
    SimulationTransactionFrame() = delete;

    virtual ~SimulationTransactionFrame()
    {
    }

    static TransactionFramePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& envelope,
                            TransactionResult simulationResult);
};
}
