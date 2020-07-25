// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"

namespace stellar
{
namespace txsimulation
{

class TxSimFeeBumpTransactionFrame : public FeeBumpTransactionFrame
{
    TransactionResult mSimulationResult;

  public:
    TxSimFeeBumpTransactionFrame(Hash const& networkID,
                                 TransactionEnvelope const& envelope,
                                 TransactionResult simulationResult,
                                 uint32_t partition);
    TxSimFeeBumpTransactionFrame(TransactionFrame const&) = delete;
    TxSimFeeBumpTransactionFrame() = delete;

    virtual ~TxSimFeeBumpTransactionFrame()
    {
    }

    int64_t getFee(LedgerHeader const& header, int64_t baseFee,
                   bool applying) const override;
    void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) override;
};
}
}
