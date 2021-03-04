#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/UnorderedMap.h"
#include "util/optional.h"
#include <deque>
#include <functional>

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
    virtual void toXDR(TransactionSet& set) = 0;
};

class TxSetFrame : public AbstractTxSetFrameForApply
{
    optional<Hash> mHash{nullptr};

    // mValid caches both the last app LCL that we checked
    // vaidity for, and the result of that validity check.
    optional<std::pair<Hash, bool>> mValid{nullptr};

    Hash mPreviousLedgerHash;

    using AccountTransactionQueue = std::deque<TransactionFrameBasePtr>;

    bool checkOrTrim(Application& app,
                     std::vector<TransactionFrameBasePtr>& trimmed,
                     bool justCheck, uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset);

    UnorderedMap<AccountID, AccountTransactionQueue> buildAccountTxQueues();
    friend struct SurgeCompare;

  public:
    std::vector<TransactionFrameBasePtr> mTransactions;

    TxSetFrame(Hash const& previousLedgerHash);

    TxSetFrame(TxSetFrame const& other) = default;

    // make it from the wire
    TxSetFrame(Hash const& networkID, TransactionSet const& xdrSet);

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
    void toXDR(TransactionSet& set) override;
};
} // namespace stellar
