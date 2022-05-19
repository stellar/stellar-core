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

    // Creates a valid TxSetFrame from the provided transactions.
    // Not all the transactions will be included in the result: invalid
    // transactions are trimmed and optionally returned via `invalidTxs` and if
    // there are too many remaining transactions surge pricing is applied.
    // The result is guaranteed to pass `checkValid` check with the same
    // arguments as in this method, so additional validation is not needed.
    static TxSetFrameConstPtr
    makeFromTransactions(Transactions const& txs, Application& app,
                         uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset,
                         TxSetFrame::Transactions* invalidTxs = nullptr);

    // Creates a legacy (non-generalized) TxSetFrame from the transactions that
    // are trusted to be valid. Validation and filtering are not performed.
    // This should be *only* used for building the legacy TxSetFrames from
    // historical transactions.
    static TxSetFrameConstPtr
    makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                Transactions const& txs);

    // Creates a valid empty TxSetFrame.
    static TxSetFrameConstPtr
    makeEmpty(LedgerHeaderHistoryEntry const& lclHeader);

    // Creates a TxSetFrame from the XDR message.
    // As the message is not trusted, it has to be validated via `checkValid`.
    static TxSetFrameConstPtr makeFromWire(Hash const& networkID,
                                           TransactionSet const& xdrTxSet);
    static TxSetFrameConstPtr
    makeFromWire(Hash const& networkID,
                 GeneralizedTransactionSet const& xdrTxSet);

    TxSetFrame(TxSetFrame const& other) = delete;
    TxSetFrame(TxSetFrame&& other) = delete;
    TxSetFrame& operator=(TxSetFrame const& other) = delete;
    TxSetFrame& operator=(TxSetFrame&& other) = delete;

    virtual ~TxSetFrame(){};

    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t> getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                        LedgerHeader const& lclHeader) const;

    // Returns the hash of this tx set.
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    // Gets all the transactions belonging to this frame sorted by their full
    // hashes.
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

    // Returns the size of this transaction set when encoded to XDR.
    size_t encodedSize() const;

    // Returns the sum of all fees that this transaction set would take.
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all bids for all transactions in this set.
    int64_t getTotalBids() const;

    // Returns whether this transaction set is generalized, i.e. representable
    // by GeneralizedTransactionSet XDR.
    bool isGeneralizedTxSet() const;

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

  protected:
    TxSetFrame(LedgerHeaderHistoryEntry const& lclHeader,
               Transactions const& txs);
    TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
               Transactions const& txs);

    // Computes the fees for transactions in this set.
    // This has to be `const` in combination with `mutable` fee-related fields
    // in order to accommodate one specific case: legacy (non-generalized) tx
    // sets received from the peers don't include the fee information and we
    // don't have immediate access to the ledger header needed to compute them.
    // Hence we lazily compute the fees in `getTxBaseFee` for such TxSetFrames.
    // This can be cleaned up after the protocol migration as non-generalized tx
    // sets won't exist in the network anymore.
    void computeTxFees(LedgerHeader const& lclHeader) const;
    void computeContentsHash();

    std::optional<Hash> mHash;

  private:
    bool addTxsFromXdr(Hash const& networkID,
                       xdr::xvector<TransactionEnvelope> const& txs,
                       bool useBaseFee, std::optional<uint32_t> baseFee);
    void surgePricingFilter(uint32_t opsLeft);

    bool const mIsGeneralized;

    Hash const mPreviousLedgerHash;
    Transactions mTxs;

    mutable bool mFeesComputed = false;
    mutable std::unordered_map<TransactionFrameBaseConstPtr,
                               std::optional<int64_t>>
        mTxBaseFee;

    std::optional<size_t> mutable mEncodedSize;
};

} // namespace stellar
