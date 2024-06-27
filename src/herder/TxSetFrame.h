#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerHashUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
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

using TxFrameList = std::vector<TransactionFrameBasePtr>;
using PerPhaseTransactionList = std::vector<TxFrameList>;

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
    PerPhaseTransactionList const& txPhases, Application& app,
    uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset
#ifdef BUILD_TESTS
    // Skips the tx set validation and preserves the pointers
    // to the passed-in transactions - use in conjunction with
    // `enforceTxsApplyOrder` argument in test-only overrides.
    ,
    bool skipValidation = false
#endif
);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    PerPhaseTransactionList const& txPhases, Application& app,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
    // Skips the tx set validation and preserves the pointers
    // to the passed-in transactions - use in conjunction with
    // `enforceTxsApplyOrder` argument in test-only overrides.
    ,
    bool skipValidation = false
#endif
);

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxFrameList txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          bool enforceTxsApplyOrder = false);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(TxFrameList txs, Application& app,
                          uint64_t lowerBoundCloseTimeOffset,
                          uint64_t upperBoundCloseTimeOffset,
                          TxFrameList& invalidTxs,
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
// this TxSetXDRFrame refers to. This is performed by `prepareForApply` method.
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
                                TxFrameList const& txs);

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

    // Returns the hash of the previous ledger that this tx set refers to.
    Hash const& previousLedgerHash() const;

    // Returns the total number of transactions in this tx set (even if it's
    // not structurally valid).
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
    PerPhaseTransactionList
    createTransactionFrames(Hash const& networkID) const;

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

// The following definitions are used to represent the 'parallel' phase of the
// transaction set.
//
// The structure of this phase is as follows:
// - The whole phase (`TxStageFrameList`) consists of several sequential
//   'stages' (`TxStageFrame`). A stage has to be executed after every
//   transaction in the previous stage has been applied.
// - A 'stage' (`TxStageFrame`) consists of several independent 'clusters'
//   (`TxClusterFrame`). Transactions in different 'clusters' are independent of
//   each other and can be applied in parallel.
// - A 'cluster' (`TxClusterFrame`) consists of transactions that should
//   generally be applied sequentially. However, not all the transactions in
//   the cluster are necessarily conflicting with each other; it is possible
//   that some, or even all transactions in the cluster structure can be applied
//   in parallel with each other (depending on their footprints).
//
// This structure mimics the XDR structure of the `ParallelTxsComponent`.
using TxClusterFrame = TxFrameList;
using TxStageFrame = std::vector<TxClusterFrame>;
using TxStageFrameList = std::vector<TxStageFrame>;

// Alias for the map from transaction to its inclusion fee as defined by the
// transaction set.
using InclusionFeeMap =
    std::unordered_map<TransactionFrameBaseConstPtr, std::optional<int64_t>>;

// `TxSetPhaseFrame` represents a single phase of the `ApplicableTxSetFrame`.
//
// Phases can only be created as a part of the `ApplicableTxSetFrame` and thus
// don't have any public constructors.
//
// Phase may either wrap the corresponding `TransactionPhase` XDR for
// generalized transactions sets, or represent all the transactions in the
// 'legacy' transaction set (which is considered to have only a single phase).
//
// This does not assume any specific order of transactions by default - the
// phase in 'apply' order has to be explicitly requested from the parent
// `ApplicableTxSetFrame` via `getPhasesInApplyOrder` method.
class TxSetPhaseFrame
{
  public:
    // Returns true when this phase can be applied in parallel.
    // Currently only Soroban phase can be parallel, and only starting from
    // PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION protocol
    bool isParallel() const;

    // Returns the parallel stages of this phase.
    //
    // This may only be called when `isParallel()` is true.
    TxStageFrameList const& getParallelStages() const;
    // Returns all the transactions in this phase if it's not parallel.
    //
    // This may only be called when `isParallel()` is false.
    TxFrameList const& getSequentialTxs() const;

    // Serializes this phase to the provided XDR.
    void toXDR(TransactionPhase& xdrPhase) const;

    // Iterator over all transactions in this phase.
    // The order of iteration is defined by the parent `ApplicableTxSetFrame`.
    // If the phase is sorted for apply, then the iteration order can be used
    // to determine a stable index of every transaction in the phase, even if
    // the phase is parallel and can have certain transaction applied in
    // arbitrary order.
    class Iterator
    {
      public:
        using value_type = TransactionFrameBasePtr;
        using difference_type = std::ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        TransactionFrameBasePtr operator*() const;

        Iterator& operator++();
        Iterator operator++(int);

        bool operator==(Iterator const& other) const;
        bool operator!=(Iterator const& other) const;

      private:
        friend class TxSetPhaseFrame;

        Iterator(TxStageFrameList const& txs, size_t stageIndex);
        TxStageFrameList const& mStages;
        size_t mStageIndex = 0;
        size_t mClusterIndex = 0;
        size_t mTxIndex = 0;
    };
    Iterator begin() const;
    Iterator end() const;
    size_t sizeTx() const;
    size_t sizeOp() const;
    size_t size(LedgerHeader const& lclHeader) const;
    bool empty() const;

    // Get _inclusion_ fee map for this phase. The map contains lowest base
    // fee for each transaction (lowest base fee is identical for all
    // transactions in the same lane)
    InclusionFeeMap const& getInclusionFeeMap() const;

    std::optional<Resource> getTotalResources() const;

  private:
    friend class TxSetXDRFrame;
    friend class ApplicableTxSetFrame;

    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(PerPhaseTransactionList const& txPhases,
                              Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
                              ,
                              bool skipValidation
#endif
    );
#ifdef BUILD_TESTS
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(TxFrameList txs, Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              TxFrameList& invalidTxs,
                              bool enforceTxsApplyOrder);
#endif
    TxSetPhaseFrame(TxSetPhase phase, TxFrameList const& txs,
                    std::shared_ptr<InclusionFeeMap> inclusionFeeMap);
    TxSetPhaseFrame(TxSetPhase phase, TxStageFrameList&& txs,
                    std::shared_ptr<InclusionFeeMap> inclusionFeeMap);

    // Creates a new phase from `TransactionPhase` XDR coming from a
    // `GeneralizedTransactionSet`.
    static std::optional<TxSetPhaseFrame>
    makeFromWire(TxSetPhase phase, Hash const& networkID,
                 TransactionPhase const& xdrPhase);

    // Creates a new phase from all the transactions in the legacy
    // `TransactionSet` XDR.
    static std::optional<TxSetPhaseFrame>
    makeFromWireLegacy(LedgerHeader const& lclHeader, Hash const& networkID,
                       xdr::xvector<TransactionEnvelope> const& xdrTxs);

    // Creates a valid empty phase with given `isParallel` flag.
    static TxSetPhaseFrame makeEmpty(TxSetPhase phase, bool isParallel);

    // Returns a copy of this phase with transactions sorted for apply.
    TxSetPhaseFrame sortedForApply(Hash const& txSetHash) const;
    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;
    bool checkValidClassic(LedgerHeader const& lclHeader) const;
    bool checkValidSoroban(LedgerHeader const& lclHeader,
                           SorobanNetworkConfig const& sorobanConfig) const;

    bool txsAreValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset) const;

    TxSetPhase mPhase;

    TxStageFrameList mStages;
    std::shared_ptr<InclusionFeeMap> mInclusionFeeMap;
    bool mIsParallel;
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
    std::optional<int64_t>
    getTxBaseFee(TransactionFrameBaseConstPtr const& tx) const;

    // Gets the phase frame for the given phase in arbitrary order.
    TxSetPhaseFrame const& getPhase(TxSetPhase phase) const;

    // Gets all the phases of this transaction set with transactions in
    // arbitrary order.
    std::vector<TxSetPhaseFrame> const& getPhases() const;

    // Gets all the phases of this transaction set, each phase with
    // transactions sorted for apply.
    //
    // For the generalized transaction sets, the order is defined by shuffling
    // all the transactions that are applied sequentially relatively to each
    // other using the hash of the transaction set.
    //
    // For the legacy transaction sets, the apply order satisfies :
    // - Transactions for an account are sorted by sequence number (ascending).
    // - The order between accounts is randomized.
    std::vector<TxSetPhaseFrame> const& getPhasesInApplyOrder() const;

    // Checks if this transaction set frame is valid in the context of the
    // current LCL.
    // This can be called when LCL does not match `previousLedgerHash`, but
    // then validation will never pass.
    bool checkValid(Application& app, uint64_t lowerBoundCloseTimeOffset,
                    uint64_t upperBoundCloseTimeOffset) const;

    // Returns the size of this whole transaction set, or the specified phase
    // in operations or transactions (for older protocol versions).
    size_t size(LedgerHeader const& lh,
                std::optional<TxSetPhase> phase = std::nullopt) const;

    // Returns the total number of transactions in the given phase.
    size_t sizeTx(TxSetPhase phase) const;
    // Returns the total number of transactions in this tx set.
    size_t sizeTxTotal() const;

    // Returns the total number of operations in the given phase.
    size_t sizeOp(TxSetPhase phase) const;
    // Returns the total number of operations in this tx set.
    size_t sizeOpTotal() const;

    // Returns whether this transaction set is empty.
    bool
    empty() const
    {
        return sizeTxTotal() == 0;
    }

    // Returns the number of phases in this tx set.
    size_t
    numPhases() const
    {
        return mPhases.size();
    }

    // Returns the sum of all fees that this transaction set would take.
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all _inclusion fee_ bids for all transactions in this
    // set.
    int64_t getTotalInclusionFees() const;

    // Returns whether this transaction set is generalized, i.e. representable
    // by `GeneralizedTransactionSet` XDR.
    bool isGeneralizedTxSet() const;

    // Returns a short description of this transaction set for logging.
    std::string summary() const;

    // Returns the hash of this transaction set.
    Hash const& getContentsHash() const;

    // Converts this transaction set to XDR.
    // This shouldn't be exposed for the regular flows, but is useful to expose
    // to cover XDR roundtrips in tests.
#ifndef BUILD_TESTS
  private:
#endif
    TxSetXDRFrameConstPtr toWireTxSetFrame() const;

  private:
    friend class TxSetXDRFrame;

    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(PerPhaseTransactionList const& txPhases,
                              Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
                              ,
                              bool skipValidation
#endif
    );
#ifdef BUILD_TESTS
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(TxFrameList txs, Application& app,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              TxFrameList& invalidTxs,
                              bool enforceTxsApplyOrder);
#endif

    ApplicableTxSetFrame(Application& app,
                         LedgerHeaderHistoryEntry const& lclHeader,
                         std::vector<TxSetPhaseFrame> const& phases,
                         std::optional<Hash> contentsHash);
    ApplicableTxSetFrame(Application& app, bool isGeneralized,
                         Hash const& previousLedgerHash,
                         std::vector<TxSetPhaseFrame> const& phases,
                         std::optional<Hash> contentsHash);
    ApplicableTxSetFrame(ApplicableTxSetFrame const&) = default;
    ApplicableTxSetFrame(ApplicableTxSetFrame&&) = default;

    void toXDR(TransactionSet& set) const;
    void toXDR(GeneralizedTransactionSet& generalizedTxSet) const;

    bool const mIsGeneralized;
    Hash const mPreviousLedgerHash;

    // All the phases of this transaction set.
    //
    // There can only be 1 phase (classic) prior to protocol 20.
    // Starting with protocol 20, there are 2 phases (classic and Soroban).
    std::vector<TxSetPhaseFrame> const mPhases;

    // The phases with transactions sorted for apply.
    //
    // This is `mutable` because we want to do the sorting lazily only for the
    // transaction sets that are actually applied.
    mutable std::vector<TxSetPhaseFrame> mApplyOrderPhases;

    std::optional<Hash> mContentsHash;
};

} // namespace stellar
