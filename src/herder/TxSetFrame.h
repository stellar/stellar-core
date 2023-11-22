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
#include <variant>

namespace stellar
{
class Application;
class TxSetXDRFrame;
class ApplicableTxSetFrame;
using TxSetXDRFrameConstPtr = std::shared_ptr<TxSetXDRFrame const>;
using ApplicableTxSetFrameConstPtr =
    std::unique_ptr<ApplicableTxSetFrame const>;

enum class TxSetPhase
{
    CLASSIC,
    SOROBAN,
    PHASE_COUNT
};

using TxSetTransactions = std::vector<TransactionFrameBasePtr>;
using TxSetPhaseTransactions = std::vector<TxSetTransactions>;

std::string getTxSetPhaseName(TxSetPhase phase);

// Creates a valid ApplicableTxSetFrame and corresponding TxSetXDRFrame
// from the provided transactions.
//
// Not all the transactions will be included in the result: invalid
// transactions are trimmed and optionally returned via `invalidTxs` and if
// there are too many remaining transactions surge pricing is applied.
// The result is guaranteed to pass `checkValid` check with the same
// arguments as in this method, so additional validation is not needed.
//
// **Note**: the output `ApplicableTxSetFrame` will *not* contain the input
// transaction pointers.
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxSetPhaseTransactions const& txPhases, Application& app,
    uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
    // Skips the tx set validation and preserves the pointers
    // to the passed-in transactions - use in conjunction with
    // `orderOverride` argument in test-only overrides.
    ,
    bool skipValidation = false
#endif
);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxSetPhaseTransactions const& txPhases, Application& app,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    TxSetPhaseTransactions& invalidTxsPerPhase
#ifdef BUILD_TESTS
    // Skips the tx set validation and preserves the pointers
    // to the passed-in transactions - use in conjunction with
    // `orderOverride` argument in test-only overrides.
    ,
    bool skipValidation = false
#endif
);

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxSetTransactions txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          bool enforceTxsApplyOrder = false);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxSetTransactions txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          TxSetTransactions& invalidTxs,
                          bool enforceTxsApplyOrder = false);
#endif

// `TxSetFrame` is a wrapper around `TransactionSet` or
// `GeneralizedTransactionSet` XDR.
//
// TxSetXDRFrame doesn't try to interpret the XDR it wraps and might even
// store structurally invalid XDR. Thus its safe to use at
// overlay layer to simply exchange the messages, cache them etc.
//
// Before even trying to validate and apply a TxSetXDRFrame it has
// to be interpreted and prepared for apply using the ledger state
// this TxSetXDRFrame refers to. This is typically performed by
// `prepareForApply` method.
class TxSetXDRFrame : public NonMovableOrCopyable
{
  public:
    // Creates a valid empty TxSetXDRFrame pointing at provided `lclHeader`.
    static TxSetXDRFrameConstPtr
    makeEmpty(LedgerHeaderHistoryEntry const& lclHeader);

    // `makeFromWire` methods create a TxSetXDRFrame from the XDR messages.
    // These methods don't perform any validation on the XDR.
    static TxSetXDRFrameConstPtr makeFromWire(TransactionSet const& xdrTxSet);
    static TxSetXDRFrameConstPtr
    makeFromWire(GeneralizedTransactionSet const& xdrTxSet);

    static TxSetXDRFrameConstPtr
    makeFromStoredTxSet(StoredTransactionSet const& storedSet);

    // Creates a legacy (non-generalized) TxSetXDRFrame from the
    // transactions that are trusted to be valid. Validation and filtering
    // are not performed.
    // This should be *only* used for building the legacy TxSetFrames from
    // historical transactions.
    static TxSetXDRFrameConstPtr
    makeFromHistoryTransactions(Hash const& previousLedgerHash,
                                TxSetTransactions const& txs);

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;
    void storeXDR(StoredTransactionSet& txSet) const;

    ~TxSetXDRFrame() = default;

    // Interprets this transaction set using the current ledger state and
    // returns a frame suitable for being applied to the ledger.
    //
    // Returns `nullptr` in case if transaction set can't be interpreted,
    // for example if XDR of this `TxSetFrame` is malformed.
    //
    // Note, that the output tx set is still not necessarily valid; it is
    // only truly safe to be applied when `applicableTxSetFrame->checkValid()`
    // returns `true`.
    //
    // This may *only* be called when LCL hash matches the `previousLedgerHash`
    // of this `TxSetFrame` - tx sets with a wrong ledger hash shouldn't even
    // be attempted to be interpreted.
    ApplicableTxSetFrameConstPtr prepareForApply(Application& app) const;

    bool isGeneralizedTxSet() const;

    // Returns the hash of this tx set.
    Hash const& getContentsHash() const;

    Hash const& previousLedgerHash() const;

    size_t sizeTxTotal() const;

    // Gets the size of this transaction set in operations.
    // Since this isn't guaranteed to even be valid XDR, this should
    // only be used for the logging (or testing) purpose. In any other
    // context, `ApplicableTxSetFrame::sizeOpTotal()` should be used.
    size_t sizeOpTotalForLogging() const;

    // Returns the size of this transaction set when encoded to XDR.
    size_t encodedSize() const;

    // Creates transaction frames for all the transactions in the set, grouped
    // by phase.
    // This is only necessary to serve a very specific use case of updating
    // the transaction queue with wired tx sets. Otherwise, use
    // getTransactionsForPhase() in `ApplicableTxSetFrame`.
    TxSetPhaseTransactions createTransactionFrames(Hash const& networkID) const;

#ifdef BUILD_TESTS
    mutable ApplicableTxSetFrameConstPtr mApplicableTxSetOverride;

    StellarMessage toStellarMessage() const;
#endif

  private:
    TxSetXDRFrame(TransactionSet const& xdrTxSet);
    TxSetXDRFrame(GeneralizedTransactionSet const& xdrTxSet);

    std::variant<TransactionSet, GeneralizedTransactionSet> mXDRTxSet;
    size_t mEncodedSize{};
    Hash mHash;
};

// Transaction set that is suitable for being applied to the ledger.
//
// This is not necessarily a fully *valid* transaction set: further validation
// should typically be performed via `checkValid` before actual application.
//
// `ApplicableTxSetFrame` can only be built from `TxSetFrame`, either via
// constructing it with `makeFromTransactions` (for the transaction sets
// generated for nomination), or via `prepareForApply` (for arbitrary
// transaction sets).
class ApplicableTxSetFrame
{
  public:
    // Returns the base fee for the transaction or std::nullopt when the
    // transaction is not discounted.
    std::optional<int64_t> getTxBaseFee(TransactionFrameBaseConstPtr const& tx,
                                        LedgerHeader const& lclHeader) const;

    // Gets all the transactions belonging to this frame in arbitrary order.
    TxSetTransactions const& getTxsForPhase(TxSetPhase phase) const;

    // Build a list of transaction ready to be applied to the last closed
    // ledger, based on the transaction set.
    //
    // The order satisfies:
    // * transactions for an account are sorted by sequence number (ascending)
    // * the order between accounts is randomized
    TxSetTransactions getTxsInApplyOrder() const;

    // Checks if this tx set frame is valid in the context of the current LCL.
    // This can be called when LCL does not match `previousLedgerHash`, but
    // then validation will never pass.
    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;

    size_t size(LedgerHeader const& lh,
                std::optional<TxSetPhase> phase = std::nullopt) const;

    size_t
    sizeTx(TxSetPhase phase) const
    {
        return mTxPhases.at(static_cast<size_t>(phase)).size();
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

    size_t sizeOp(TxSetPhase phase) const;
    size_t sizeOpTotal() const;

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

    Hash const& getContentsHash() const;

    // This shouldn't be needed for the regular flows, but is useful
    // to cover XDR roundtrips in tests.
#ifndef BUILD_TESTS
  private:
#endif
    TxSetXDRFrameConstPtr toWireTxSetFrame() const;

  private:
    friend class TxSetXDRFrame;
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(TxSetPhaseTransactions const& txPhases,
                              Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              TxSetPhaseTransactions& invalidTxsPerPhase
#ifdef BUILD_TESTS
                              ,
                              bool skipValidation
#endif
    );
#ifdef BUILD_TESTS
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(TxSetTransactions txs, Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              TxSetTransactions& invalidTxs,
                              bool enforceTxsApplyOrder);
#endif

    ApplicableTxSetFrame(Application& app,
                         LedgerHeaderHistoryEntry const& lclHeader,
                         TxSetPhaseTransactions const& txs,
                         std::optional<Hash> contentsHash);
    ApplicableTxSetFrame(Application& app, bool isGeneralized,
                         Hash const& previousLedgerHash,
                         TxSetPhaseTransactions const& txs,
                         std::optional<Hash> contentsHash);
    ApplicableTxSetFrame(ApplicableTxSetFrame const&) = default;
    ApplicableTxSetFrame(ApplicableTxSetFrame&&) = default;
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

    bool addTxsFromXdr(Hash const& networkID,
                       xdr::xvector<TransactionEnvelope> const& txs,
                       bool useBaseFee, std::optional<int64_t> baseFee,
                       TxSetPhase phase);
    void applySurgePricing(Application& app);

    void computeTxFeesForNonGeneralizedSet(LedgerHeader const& lclHeader,
                                           int64_t lowestBaseFee,
                                           bool enableLogging) const;

    void computeTxFees(TxSetPhase phase, LedgerHeader const& ledgerHeader,
                       SurgePricingLaneConfig const& surgePricingConfig,
                       std::vector<int64_t> const& lowestLaneFee,
                       std::vector<bool> const& hadTxNotFittingLane) const;
    std::optional<Resource> getTxSetSorobanResource() const;

    // Get _inclusion_ fee map for a given phase. The map contains lowest base
    // fee for each transaction (lowest base fee is identical for all
    // transactions in the same lane)
    std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>&
    getInclusionFeeMap(TxSetPhase phase) const;

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

    bool const mIsGeneralized;
    Hash const mPreviousLedgerHash;
    // There can only be 1 phase (classic) prior to protocol 20.
    // Starting protocol 20, there will be 2 phases (classic and soroban).
    std::vector<TxSetTransactions> mTxPhases;

    mutable std::vector<bool> mFeesComputed;
    mutable std::vector<std::unordered_map<TransactionFrameBaseConstPtr,
                                           std::optional<int64_t>>>
        mPhaseInclusionFeeMap;

    std::optional<Hash> mContentsHash;
#ifdef BUILD_TESTS
    mutable std::optional<TxSetTransactions> mApplyOrderOverride;
#endif
};

} // namespace stellar
