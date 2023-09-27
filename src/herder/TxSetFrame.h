#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/NonCopyable.h"
#include "xdr/Stellar-internal.h"

#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

namespace stellar
{
class Application;
class TxSetFrame;
using TxSetFrameConstPtr = std::shared_ptr<TxSetFrame const>;

class TxSetFrame : public NonMovableOrCopyable
{
  public:
    enum Phase
    {
        CLASSIC,
        SOROBAN,
        PHASE_COUNT
    };

    using Transactions = std::vector<TransactionFrameBasePtr>;
    using TxPhases = std::vector<Transactions>;

    static std::string getPhaseName(Phase phase);

    // Creates a valid TxSetFrame from the provided transactions.
    // Not all the transactions will be included in the result: invalid
    // transactions are trimmed and optionally returned via `invalidTxs` and if
    // there are too many remaining transactions surge pricing is applied.
    // The result is guaranteed to pass `checkValid` check with the same
    // arguments as in this method, so additional validation is not needed.
    //
    // **Note**: the output `TxSetFrame` will *not* contain the input
    // transaction pointers.
    static TxSetFrameConstPtr
    makeFromTransactions(TxPhases const& txPhases, Application& app,
                         uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset,
                         bool forceIsNotGeneralized = false);
    static TxSetFrameConstPtr makeFromTransactions(
        TxPhases const& txPhases, Application& app,
        uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
        TxPhases& invalidTxsPerPhase, bool forceIsNotGeneralized = false);

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
    static TxSetFrameConstPtr makeFromWire(Application& app,
                                           TransactionSet const& xdrTxSet);
    static TxSetFrameConstPtr
    makeFromWire(Application& app, GeneralizedTransactionSet const& xdrTxSet);

    // Creates a TxSetFrame from StoredTransactionSet (internally persisted tx
    // set format).
    static TxSetFrameConstPtr
    makeFromStoredTxSet(StoredTransactionSet const& storedSet,
                        Application& app);

    virtual ~TxSetFrame(){};

    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t> getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                        LedgerHeader const& lclHeader) const;

    // Returns the hash of this tx set.
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    // Gets all the transactions belonging to this frame in arbitrary order.
    Transactions const& getTxsForPhase(Phase phase) const;

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

    size_t size(LedgerHeader const& lh,
                std::optional<Phase> phase = std::nullopt) const;

    size_t
    sizeTx(Phase phase) const
    {
        return mTxPhases.at(phase).size();
    }
    size_t sizeTxTotal() const;

    bool
    empty() const
    {
        return sizeTxTotal() == 0;
    }

    size_t
    numPhases() const
    {
        return mTxPhases.size();
    }

    size_t sizeOp(Phase phase) const;
    size_t sizeOpTotal() const;

    // Returns the size of this transaction set when encoded to XDR.
    size_t encodedSize() const;

    // Returns the sum of all fees that this transaction set would take.
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all _inclusion fee_ bids for all transactions in this
    // set.
    int64_t getTotalInclusionFees() const;

    // Returns whether this transaction set is generalized, i.e. representable
    // by GeneralizedTransactionSet XDR.
    bool isGeneralizedTxSet() const;

    // Returns a short description of this transaction set.
    std::string summary() const;

    virtual void toXDR(TransactionSet& set) const;
    virtual void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

#ifdef BUILD_TESTS
    // Test helper that only checks the XDR structure validitiy without
    // validating internal transactions.
    virtual bool checkValidStructure() const;
    void computeContentsHashForTesting();
    static TxSetFrameConstPtr makeFromTransactions(
        Transactions txs, Application& app, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset, bool forceIsNotGeneralized = false);
    static TxSetFrameConstPtr makeFromTransactions(
        Transactions txs, Application& app, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset, Transactions& invalidTxs,
        bool forceIsNotGeneralized = false);
#endif

  protected:
    TxSetFrame(LedgerHeaderHistoryEntry const& lclHeader, TxPhases const& txs,
               bool forceIsNotGeneralized = false);
    TxSetFrame(bool isGeneralized, Hash const& previousLedgerHash,
               TxPhases const& txs);

    // Computes the fees for transactions in this set based on information from
    // the non-generalized tx set.
    // This has to be `const` in combination with `mutable` fee-related fields
    // in order to accommodate one specific case: legacy (non-generalized) tx
    // sets received from the peers don't include the fee information and we
    // don't have immediate access to the ledger header needed to compute them.
    // Hence we lazily compute the fees in `getTxBaseFee` for such TxSetFrames.
    // This can be cleaned up after the protocol migration as non-generalized tx
    // sets won't exist in the network anymore.
    void computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader) const;

    void computeContentsHash();

    std::optional<Hash> mHash;
    std::optional<size_t> mutable mEncodedSize;

  private:
    bool addTxsFromXdr(Application& app,
                       xdr::xvector<TransactionEnvelope> const& txs,
                       bool useBaseFee, std::optional<int64_t> baseFee,
                       Phase phase);
    void applySurgePricing(Application& app);

    void computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader,
                                           int64_t lowestBaseFee,
                                           bool enableLogging) const;

    void computeTxFees(TxSetFrame::Phase phase,
                       LedgerHeader const& ledgerHeader,
                       SurgePricingLaneConfig const& surgePricingConfig,
                       std::vector<int64_t> const& lowestLaneFee,
                       std::vector<bool> const& hadTxNotFittingLane);

    bool const mIsGeneralized;

    Hash const mPreviousLedgerHash;
    // There can only be 1 phase (classic) prior to protocol 20.
    // Starting protocol 20, there will be 2 phases (classic and soroban).
    std::vector<Transactions> mTxPhases;

    mutable std::vector<bool> mFeesComputed;
    mutable std::unordered_map<TransactionFrameBaseConstPtr,
                               std::optional<int64_t>>
        mTxBaseFeeClassic;
    mutable std::unordered_map<TransactionFrameBaseConstPtr,
                               std::optional<int64_t>>
        mTxBaseFeeSoroban;
    std::optional<Resource> getTxSetSorobanResource() const;

    // Get _inclusion_ fee map for a given phase. The map contains lowest base
    // fee for each transaction (lowest base fee is identical for all
    // transactions in the same lane)
    std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
    getInclusionFeeMap(Phase phase) const;
};

} // namespace stellar
