// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <optional>

#include "ledger/LedgerHashUtils.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/TxResource.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <optional>

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase>;
using TransactionFrameBaseConstPtr =
    std::shared_ptr<TransactionFrameBase const>;

class TransactionFrameBase
{
  public:
    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual bool apply(Application& app, AbstractLedgerTxn& ltx,
                       TransactionMetaFrame& meta,
                       Hash const& sorobanBasePrngSeed = Hash{}) = 0;

    virtual bool checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                            SequenceNumber current,
                            uint64_t lowerBoundCloseTimeOffset,
                            uint64_t upperBoundCloseTimeOffset) = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

    // Returns the total fee of this transaction, including the 'flat',
    // non-market part.
    virtual int64_t getFullFee() const = 0;
    // Returns the part of the full fee used to make decisions as to
    // whether this transaction should be included into ledger.
    virtual int64_t getInclusionFee() const = 0;
    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const = 0;

    virtual Hash const& getContentsHash() const = 0;
    virtual Hash const& getFullHash() const = 0;

    virtual uint32_t getNumOperations() const = 0;
    virtual Resource getResources(bool useByteLimitInClassic) const = 0;

    virtual std::vector<Operation> const& getRawOperations() const = 0;

    virtual TransactionResult& getResult() = 0;
    virtual TransactionResultCode getResultCode() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;
    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;
    virtual std::optional<SequenceNumber const> const getMinSeqNum() const = 0;
    virtual Duration getMinSeqAge() const = 0;
    virtual uint32 getMinSeqLedgerGap() const = 0;

    virtual void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const = 0;
    virtual void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const = 0;

    virtual void processFeeSeqNum(AbstractLedgerTxn& ltx,
                                  std::optional<int64_t> baseFee) = 0;

    virtual void processPostApply(Application& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& meta) = 0;

    virtual StellarMessage toStellarMessage() const = 0;

    virtual bool hasDexOperations() const = 0;

    virtual bool isSoroban() const = 0;
    virtual SorobanResources const& sorobanResources() const = 0;
    virtual xdr::xvector<DiagnosticEvent> const&
    getDiagnosticEvents() const = 0;
    virtual int64 declaredSorobanResourceFee() const = 0;
    virtual bool XDRProvidesValidFee() const = 0;
};
}
