#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/UnorderedMap.h"
#include <deque>
#include <functional>
#include <optional>
#include <variant>

namespace stellar
{
class Application;

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;
typedef std::shared_ptr<TxSetFrame const> TxSetFrameConstPtr;

class AbstractTxSetFrameForApply
{
  public:
    virtual ~AbstractTxSetFrameForApply(){};

    virtual int64_t getBaseFee(LedgerHeader const& lh) const = 0;

    virtual Hash const& getContentsHash() = 0;

    virtual Hash const& previousLedgerHash() const = 0;

    virtual size_t sizeTx() const = 0;

    virtual size_t sizeOp() const = 0;

    virtual std::vector<TransactionFrameBasePtr> sortForApply() = 0;

    virtual bool isGeneralizedTxSet() const = 0;

    virtual void toXDR(TransactionSet& set) const = 0;
    virtual void toXDR(GeneralizedTransactionSet& set) const = 0;
};

class TxSetFrame : public AbstractTxSetFrameForApply
{
    std::optional<Hash> mHash;

    // mValid caches both the last app LCL that we checked
    // vaidity for, and the result of that validity check.
    std::optional<std::pair<Hash, bool>> mValid;

    Hash mPreviousLedgerHash;

    using AccountTransactionQueue = std::deque<TransactionFrameBasePtr>;

    bool checkOrTrim(Application& app,
                     std::vector<TransactionFrameBasePtr>& trimmed,
                     bool justCheck, uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset);

    UnorderedMap<AccountID, AccountTransactionQueue> buildAccountTxQueues();
    friend struct SurgeCompare;

    void addTxs(Hash const& networkID,
                xdr::xvector<TransactionEnvelope> const& txs);

  public:
    std::vector<TransactionFrameBasePtr> mTransactions;
    bool mGeneralized = false;

    TxSetFrame(Hash const& previousLedgerHash, uint32_t ledgerVersion);

    TxSetFrame(TxSetFrame const& other) = default;

    // make it from the wire
    TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet);
    TxSetFrame(Hash const& networkID, GeneralizedTransactionSet const& xdrSet);

    virtual ~TxSetFrame(){};

    // returns the hash of this tx set
    Hash const& getContentsHash() override;

    Hash& previousLedgerHash();
    Hash const& previousLedgerHash() const override;

    virtual void sortForHash();

    std::vector<TransactionFrameBasePtr> sortForApply() override;

    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset);

    // remove invalid transaction from this set and return those removed
    // transactions
    std::vector<TransactionFrameBasePtr>
    trimInvalid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                uint64_t upperBoundCloseTimeOffset);
    void surgePricingFilter(Application& app);

    void removeTx(TransactionFrameBasePtr tx);

    void
    add(TransactionFrameBasePtr tx)
    {
        mTransactions.push_back(tx);
        mHash.reset();
        mValid.reset();
    }

    size_t size(LedgerHeader const& lh) const;

    size_t
    sizeTx() const override
    {
        return mTransactions.size();
    }

    size_t sizeOp() const override;

    // return the base fee associated with this transaction set
    int64_t getBaseFee(LedgerHeader const& lh) const override;

    // return the sum of all fees that this transaction set would take
    int64_t getTotalFees(LedgerHeader const& lh) const;

    bool isGeneralizedTxSet() const override;

    void toXDR(TransactionSet& set) const override;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const override;
};
} // namespace stellar
