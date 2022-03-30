#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include <deque>
#include <functional>
#include <variant>

namespace stellar
{
class Application;

// A wrapper for a set of transactions that if valid, maintains the hash order
class TxSetFrame
{
  public:
    virtual ~AbstractTxSetFrameForApply(){};

    virtual std::optional<int64_t>
    getTxBaseFee(TransactionFrameBaseConstPtr const& tx) const = 0;

    virtual Hash const& getContentsHash() = 0;

    virtual Hash const& previousLedgerHash() const = 0;

    virtual size_t sizeTx() const = 0;

    virtual size_t sizeOp() const = 0;

    virtual size_t encodedSize() const = 0;

    virtual std::vector<TransactionFrameBasePtr> sortForApply() = 0;

    virtual bool isGeneralizedTxSet() const = 0;

    virtual void toXDR(TransactionSet& set) const = 0;
    virtual void toXDR(GeneralizedTransactionSet& set) const = 0;

    virtual void computeTxFees(LedgerHeader const& lh) = 0;

    virtual bool feesComputed() const = 0;
};

typedef std::shared_ptr<AbstractTxSetFrameForApply const>
    TxSetFrameBaseConstPtr;

bool validateTxSetXDRStructure(GeneralizedTransactionSet const& txSet);

class TxSetFrame : public AbstractTxSetFrameForApply
{
    std::optional<Hash> mHash;

    // mValid caches both the last app LCL that we checked
    // vaidity for, and the result of that validity check.
    std::optional<std::pair<Hash, bool>> mValid;

    Hash mPreviousLedgerHash;

    bool mGeneralized = false;

    std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>
        mTxBaseFee;

    std::optional<size_t> mutable mEncodedSize;

    using AccountTransactionQueue = std::deque<TransactionFrameBasePtr>;
    using Transactions = std::vector<TransactionFrameBasePtr>;

    TxSetFrame(Hash const& previousLedgerHash,
               Transactions const& transactions);

    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t>
    getTxBaseFee(TransactionFrameBaseConstPtr const& tx) const override;

    // make it from the wire
    TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet);
    TxSetFrame(Hash const& networkID, GeneralizedTransactionSet const& xdrSet);

    TxSetFrame(TxSetFrame const& other) = default;

    virtual ~TxSetFrame(){};

    // returns the hash of this tx set
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    Transactions const& getTxsInHashOrder() const;

    virtual Transactions getTxsInApplyOrder() const;

    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;

    void surgePricingFilter(Application& app);

    void computeTxFees(LedgerHeader const& lh) override;

    // remove invalid transaction from this set and return those removed
    // transactions
    std::vector<TransactionFrameBasePtr>
    trimInvalid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                uint64_t upperBoundCloseTimeOffset);

    void removeTx(TransactionFrameBasePtr tx);

    void add(TransactionFrameBasePtr tx);

    size_t size(LedgerHeader const& lh) const;

    size_t
    sizeTx() const
    {
        return mTxs.size();
    }

    size_t sizeOp() const;

    int64_t getBaseFee(LedgerHeader const& lh) const override;
    size_t encodedSize() const override;
    // Returns the sum of all fees that this transaction set would take
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all bids for all transactions in this set
    int64_t getTotalBids() const;

    bool isGeneralizedTxSet() const override;

    void toXDR(TransactionSet& set) const override;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const override;

    bool feesComputed() const override;
};

using TxSetFrameConstPtr = std::shared_ptr<TxSetFrame const>;

} // namespace stellar
