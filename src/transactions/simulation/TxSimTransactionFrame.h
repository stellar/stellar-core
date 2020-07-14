// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/TransactionFrame.h"

namespace stellar
{
namespace txsimulation
{

class TxSimTransactionFrame : public TransactionFrame
{
    TransactionResult mSimulationResult;
    uint32_t const mCount;

  protected:
    bool isTooEarly(LedgerTxnHeader const& header,
                    uint64_t lowerBoundCloseTimeOffset) const override;
    bool isTooLate(LedgerTxnHeader const& header,
                   uint64_t upperBoundCloseTimeOffset) const override;

    std::shared_ptr<OperationFrame> makeOperation(Operation const& op,
                                                  OperationResult& res,
                                                  size_t index) override;

    bool isBadSeq(int64_t seqNum) const override;

    int64_t getFee(LedgerHeader const& header, int64_t baseFee,
                   bool applying) const override;

    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;
    void processSeqNum(AbstractLedgerTxn& ltx) override;

  public:
    TxSimTransactionFrame(Hash const& networkID,
                          TransactionEnvelope const& envelope,
                          TransactionResult simulationResult,
                          uint32_t partition);
    TxSimTransactionFrame(TransactionFrame const&) = delete;
    TxSimTransactionFrame() = delete;

    virtual ~TxSimTransactionFrame()
    {
    }
};
}
}
