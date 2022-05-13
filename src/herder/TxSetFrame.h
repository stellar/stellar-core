#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include <deque>
#include <functional>

namespace stellar
{
class Application;
class TxSetFrame;
using TxSetFrameConstPtr = std::shared_ptr<TxSetFrame const>;

class TxSetFrame
{
  public:
    using AccountTransactionQueue = std::deque<TransactionFrameBasePtr>;
    using Transactions = std::vector<TransactionFrameBasePtr>;

    static TxSetFrameConstPtr
    makeFromTransactions(Transactions const& txs, Application& app,
                         uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset,
                         TxSetFrame::Transactions* invalidTxs = nullptr);

    static TxSetFrameConstPtr
    makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                Transactions const& txs);

    static TxSetFrameConstPtr
    makeEmpty(LedgerHeaderHistoryEntry const& lclHeader);

    static TxSetFrameConstPtr makeFromWire(Hash const& networkID,
                                           TransactionSet const& xdrTxSet);
    static TxSetFrameConstPtr
    makeFromWire(Hash const& networkID,
                 GeneralizedTransactionSet const& xdrTxSet);

    virtual ~TxSetFrame(){};

    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t> getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                        LedgerHeader const& lclHeader) const;

    // returns the hash of this tx set
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    Transactions const& getTxsInHashOrder() const;

    /*
    Build a list of transaction ready to be applied to the last closed ledger,
    based on the transaction set.

    The order satisfies:
    * transactions for an account are sorted by sequence number (ascending)
    * the order between accounts is randomized
    */
    virtual Transactions getTxsInApplyOrder() const;

    virtual bool checkValid(Application& app,
                            uint64_t lowerBoundCloseTimeOffset,
                            uint64_t upperBoundCloseTimeOffset) const;

    size_t size(LedgerHeader const& lh) const;

    size_t
    sizeTx() const
    {
        return mTxs.size();
    }

    size_t sizeOp() const;

    size_t encodedSize() const;
    // Returns the sum of all fees that this transaction set would take
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all bids for all transactions in this set
    int64_t getTotalBids() const;

    bool isGeneralizedTxSet() const;

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

    // protected:
    TxSetFrame(LedgerHeaderHistoryEntry const& lclHeader,
               Transactions const& txs);
    TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
               Transactions const& txs);

    void computeTxFees(LedgerHeader const& lclHeader) const;
    void computeContentsHash();

  private:
    bool addTxsFromXdr(Hash const& networkID,
                       xdr::xvector<TransactionEnvelope> const& txs,
                       bool useBaseFee, std::optional<uint32_t> baseFee);
    void surgePricingFilter(uint32_t opsLeft);

    bool const mIsGeneralized;

    Hash const mPreviousLedgerHash;
    Transactions mTxs;
    std::optional<Hash> mHash;

    mutable bool mFeesComputed = false;
    mutable std::unordered_map<TransactionFrameBaseConstPtr,
                               std::optional<int64_t>>
        mTxBaseFee;

    std::optional<size_t> mutable mEncodedSize;
};

} // namespace stellar
