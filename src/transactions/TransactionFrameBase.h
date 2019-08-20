// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "overlay/StellarXDR.h"

#include <unordered_set>

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;

class TransactionFrameBase;
using TransactionFrameBasePtr = std::shared_ptr<TransactionFrameBase>;

class TransactionFrameBase
    : public std::enable_shared_from_this<TransactionFrameBase>
{
  public:
    virtual bool apply(Application& app, AbstractLedgerTxn& ltx,
                       TransactionMetaV1& meta) = 0;

    virtual bool checkValid(AbstractLedgerTxn& ltxOuter,
                            SequenceNumber current) = 0;

    virtual TransactionEnvelope const& getEnvelope() const = 0;

    virtual int64_t getFeeBid() const = 0;
    virtual int64_t getMinFee(LedgerHeader const& header) const = 0;
    virtual int64_t getFee(LedgerHeader const& header,
                           int64_t baseFee) const = 0;

    virtual Hash const& getContentsHash() const = 0;
    virtual Hash const& getFullHash() const = 0;
    virtual Hash const& getInnerHash() const = 0;

    virtual size_t getOperationCountForApply() const = 0;
    virtual size_t getOperationCountForValidation() const = 0;

    virtual TransactionResult& getResult() = 0;

    virtual TransactionResultCode getResultCode() const = 0;

    virtual SequenceNumber getSeqNum() const = 0;

    virtual AccountID getFeeSourceID() const = 0;
    virtual AccountID getSourceID() const = 0;

    virtual void
    insertLedgerKeysToPrefetch(std::unordered_set<LedgerKey>& keys) const = 0;

    static TransactionFrameBasePtr
    makeTransactionFromWire(Hash const& networkID,
                            TransactionEnvelope const& env);

    virtual void processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee) = 0;

    virtual StellarMessage toStellarMessage() const = 0;

    virtual std::vector<TransactionFrameBasePtr> transactionsToApply() = 0;
};
}
