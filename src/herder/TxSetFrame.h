// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerHeaderUtils.h"
#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "util/NonCopyable.h"
#include "util/ProtocolVersion.h"
#include "util/UnorderedMap.h"
#include "xdr/Stellar-internal.h"

#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>

namespace stellar
{
class Application;
class AbstractLedgerView;
class SorobanNetworkConfig;
class TxSetXDRFrame;
class ApplicableTxSetFrame;
class TxSetBuilder;
class TxSetSelfCheck;
using TxSetXDRFrameConstPtr = std::shared_ptr<TxSetXDRFrame const>;
using TxSetReadyCallback = std::function<void(TxSetXDRFrameConstPtr const&)>;
using ApplicableTxSetFrameConstPtr =
    std::unique_ptr<ApplicableTxSetFrame const>;

#ifdef BUILD_TESTS
namespace txtest
{
// This is used to pass the parallel order of Soroban transactions to
// closeLedger in tests. The strictOrder parameter is a requirement for this to
// be used. The nested vectors are analogous to TxStageFrameList, with the only
// difference being that this stores the indices of transactions in the Soroban
// phase passed to closeLedger, rather than the transactions themselves.
using ParallelSorobanOrder = std::vector<std::vector<std::vector<uint32_t>>>;
}
#endif

enum class TxSetPhase
{
    CLASSIC,
    SOROBAN,
    PHASE_COUNT
};

// Result codes for TxSet validation.
// These provide specific reasons for validation failures, enabling precise
// testing and diagnostics.
enum class TxSetValidationResult
{
    VALID,

    // Structural errors (XDR/phase structure)
    INCORRECT_COMPONENT_ORDER,
    DUPLICATE_COMPONENT_BASE_FEES,
    EMPTY_COMPONENT,
    EMPTY_STAGE,
    EMPTY_CLUSTER,
    UNSUPPORTED_VERSION,
    UNSUPPORTED_PHASE_VERSION,
    NON_SOROBAN_PARALLEL_PHASE,
    WRONG_PHASE_COUNT,

    // Hash/protocol mismatch
    PREVIOUS_LEDGER_HASH_MISMATCH,
    GENERALIZED_TXSET_MISMATCH,

    // Classic phase errors
    CLASSIC_PHASE_PARALLEL_NOT_ALLOWED,
    TOO_MANY_CLASSIC_TXS,

    // Soroban phase errors
    SOROBAN_PARALLEL_SUPPORT_MISMATCH,
    SOROBAN_RESOURCES_OVERFLOW,
    SOROBAN_RESOURCES_EXCEED_LIMIT,
    TOO_MANY_SOROBAN_CLUSTERS,
    SOROBAN_INSTRUCTIONS_OVERFLOW,
    SOROBAN_INSTRUCTIONS_EXCEED_LIMIT,
    SOROBAN_SEQUENTIAL_INSTRUCTIONS_OVERFLOW,

    // Transaction-level errors
    MULTIPLE_TXS_PER_SOURCE_ACCOUNT,
    INVALID_PHASE_TX_TYPE,
    TX_ORDERING_INVALID,

    // Fee errors
    COMPONENT_BASE_FEE_TOO_LOW,
    TX_FEE_BID_TOO_LOW,

    // Individual transaction validation failures
    TX_VALIDATION_FAILED,
    ACCOUNT_CANT_PAY_FEE,
};

// Returns a string representation of the validation result for logging.
std::string toString(TxSetValidationResult result);

// What validating one transaction against a ledger found: whether
// `checkValid` passed and, if it did, the available balance of the fee source
// (which the caller compares with the fees of all the transactions it pays
// for).
struct TxValidationResult
{
    bool mValid{false};
    std::optional<int64_t> mFeeSourceBalance;
};

// Validation results by full transaction hash, for transactions validated
// against one particular ledger. It only holds transactions whose validity
// does not depend on the close time of the ledger they go into (see
// `txValidityDependsOnCloseTime`), so a result is the same for every set
// built on that ledger. Whoever owns the cache makes sure it is only used
// with the ledger its results were computed on.
using TxValidationCache = UnorderedMap<Hash, TxValidationResult>;

// Whether the outcome of validating `env` can depend on the close time it is
// validated for: it has time bounds or a minimum sequence age.
bool txValidityDependsOnCloseTime(TransactionEnvelope const& env);

// The ledger state a transaction set is validated against: the ledger the
// set would apply on top of. Validation only reads the state through this
// context, so it can run wherever the context's views can be used: the main
// thread validates against the LCL (`forLCL`), ledger apply against its own
// snapshot of the previous ledger.
struct TxSetValidationContext
{
    // Header of the ledger the set applies on top of.
    LedgerHeaderHistoryEntry const& mLcl;
    // Soroban network configuration of that ledger; null before Soroban.
    SorobanNetworkConfig const* mSorobanConfig;
    // Creates a read-only view of that ledger. Called once per validation
    // task; each view is used by one thread only.
    std::function<std::unique_ptr<AbstractLedgerView const>()> mMakeView;
    // Upper bound on the number of tasks the per-transaction checks are
    // split into. 1 runs them on the calling thread.
    size_t mMaxTasks;
    // Results of earlier validations against this ledger, if any: a
    // transaction found in it is not validated again, and the results of the
    // others are added to it. Only read while the validation tasks run, and
    // only written by the calling thread.
    TxValidationCache* mValidationCache{nullptr};

    // The LCL as seen by the main thread. Main thread only.
    static TxSetValidationContext forLCL(Application& app);
};

// The limits a transaction set is selected within: those of the ledger it
// goes on top of. Selection (surge pricing, the Soroban stages) only reads
// the ledger through this, so it can run for a ledger that is not the main
// thread's LCL, and on another thread.
struct TxSetBuildLimits
{
    // Header of the ledger the set applies on top of. Selection reads its
    // protocol version, base fee and maximum tx set size; its hashes are not
    // used.
    LedgerHeader mLclHeader;
    // Soroban network configuration of that ledger; null before Soroban.
    // Whoever makes the limits keeps it alive.
    SorobanNetworkConfig const* mSorobanConfig{nullptr};

    // Capacity of the classic phase, in operations.
    uint32_t maxTxSetSizeOps() const;
    // Whether a set selected within `other` is selected within these limits
    // too: everything selection reads is equal.
    bool sameAs(TxSetBuildLimits const& other) const;

    // The limits of the LCL as seen by the main thread. Main thread only.
    static TxSetBuildLimits forLCL(Application& app);
};

using TxFrameList = std::vector<TransactionFrameBasePtr>;
using PerPhaseTransactionList = std::vector<TxFrameList>;

// Creates a valid ApplicableTxSetFrame and corresponding TxSetXDRFrame
// from the provided transactions.
//
// Not all the transactions will be included in the result: invalid
// transactions are trimmed and optionally returned via `invalidTxs` and if
// there are too many remaining transactions surge pricing is applied.
// `closeTimeOffset` is how far ahead of the last closed ledger's apply time the
// ledger this set is built for applies. Transaction time bounds are
// whole-second quantities checked against that apply time (used as both the
// lower and the upper bound, since the close time is known exactly), which is
// why the offset is in apply-time units rather than derived from the consensus
// close time.
// The result is guaranteed to pass `checkValid` check with the same
// arguments as in this method, so additional validation is not needed.
// With `validTxTargets` (one entry per phase), the transactions of each phase
// must be in priority order, and only the prefix of phase `i` that holds
// `validTxTargets[i]` valid transactions of its generic surge pricing lane is
// validated and offered to selection (see `TxSetUtils::trimInvalidPrefix`).
// The transactions past that prefix are left out without being validated, so
// they appear neither in the result nor in `invalidTxs`.
//
// **Note**: the output `ApplicableTxSetFrame` will *not* contain the input
// transaction pointers.
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    PerPhaseTransactionList const& txPhases, Application& app,
    ApplyTimeOffset closeTimeOffset
#ifdef BUILD_TESTS
    // `enforceTxsApplyOrder` forces the transactions to be ordered the same
    // way as in the provided container. This is used by the test `closeLedger`
    // helpers with the `strictOrder` flag enabled.
    // `disableTxValidationForLegacyScenario` is used in the same helpers
    // and it disables the input transaction validation.
    ,
    bool enforceTxsApplyOrder = false,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder = {},
    bool disableTxValidationForLegacyScenario = false
#endif
);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    PerPhaseTransactionList const& txPhases, Application& app,
    ApplyTimeOffset closeTimeOffset,
    PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
    // `enforceTxsApplyOrder` forces the transactions to be ordered the same
    // way as in the provided container. This is used by the test `closeLedger`
    // helpers with the `strictOrder` flag enabled.
    // `disableTxValidationForLegacyScenario` is used in the same helpers
    // and it disables the input transaction validation.
    ,
    bool enforceTxsApplyOrder = false,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder = {},
    bool disableTxValidationForLegacyScenario = false
#endif
    ,
    TxSetReadyCallback const& onTxSetReady = {},
    std::vector<size_t> const& validTxTargets = {},
    // Results of earlier validations against the LCL (see
    // `TxValidationCache`); extended with this build's.
    TxValidationCache* validationCache = nullptr,
    // The builder checks every set it makes: it reads the XDR back and
    // validates the structure of the result (`TxSetSelfCheck`). Without
    // `deferredSelfCheck` it does so before returning, and throws if the
    // check fails. With it, only the structure of the selection is validated
    // before returning (`TxSetBuilder::checkSelection`; a failure throws);
    // the check of the written set is not run: it is returned here for the
    // caller to run (on any thread), so that it does not hold up the use of
    // the set.
    std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck = nullptr);

#ifdef BUILD_TESTS
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
    bool enforceTxsApplyOrder = false,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder = {},
    bool disableTxValidationForLegacyScenario = false);
std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
makeTxSetFromTransactions(
    TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
    TxFrameList& invalidTxs, bool enforceTxsApplyOrder = false,
    txtest::ParallelSorobanOrder const& parallelSorobanOrder = {},
    bool disableTxValidationForLegacyScenario = false);
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

    // Creates a valid empty TxSetXDRFrame from the previous ledger hash and
    // protocol version. Used for empty-tx-set values where the full header
    // may not be available.
    static TxSetXDRFrameConstPtr makeEmpty(Hash const& previousLedgerHash,
                                           uint32 previousLedgerVersion);

    // `makeFromWire` methods create a TxSetXDRFrame from the XDR messages.
    // These methods don't perform any validation on the XDR.
    static TxSetXDRFrameConstPtr makeFromWire(TransactionSet const& xdrTxSet);
    static TxSetXDRFrameConstPtr
    makeFromWire(GeneralizedTransactionSet const& xdrTxSet);
    // Take ownership of the XDR instead of copying it (a large set has
    // thousands of nested allocations).
    static TxSetXDRFrameConstPtr makeFromWire(TransactionSet&& xdrTxSet);
    static TxSetXDRFrameConstPtr
    makeFromWire(GeneralizedTransactionSet&& xdrTxSet);

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
    // The generalized set's XDR without copying it; the frame is immutable.
    GeneralizedTransactionSet const& getGeneralizedXDR() const;

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
    ApplicableTxSetFrameConstPtr
    prepareForApply(Application& app, LedgerHeader const& lclHeader) const;

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

    // Calls `f` with the envelope of every transaction in the set, in the
    // order createTransactionFrames returns their frames, without building
    // any frames (which copies every envelope).
    void forEachTransactionEnvelope(
        std::function<void(TransactionEnvelope const&)> const& f) const;

    // Full hashes (`TransactionFrameBase::getFullHash`) of every transaction
    // in the set, in the same order, hashed directly from the XDR.
    std::vector<Hash> getTransactionFullHashes() const;

#ifdef BUILD_TESTS
    mutable ApplicableTxSetFrameConstPtr mApplicableTxSetOverride;

    StellarMessage toStellarMessage() const;
#endif

  private:
    TxSetXDRFrame(TransactionSet const& xdrTxSet);
    TxSetXDRFrame(GeneralizedTransactionSet const& xdrTxSet);
    TxSetXDRFrame(TransactionSet&& xdrTxSet);
    TxSetXDRFrame(GeneralizedTransactionSet&& xdrTxSet);

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

    std::optional<Resource> getTotalResources(uint32_t ledgerVersion) const;

  private:
    friend class TxSetXDRFrame;
    friend class ApplicableTxSetFrame;

    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(
        PerPhaseTransactionList const& txPhases, Application& app,
        ApplyTimeOffset closeTimeOffset,
        PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
        ,
        bool enforceTxsApplyOrder,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder,
        bool disableTxValidationForLegacyScenario
#endif
        ,
        TxSetReadyCallback const& onTxSetReady,
        std::vector<size_t> const& validTxTargets,
        TxValidationCache* validationCache,
        std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck);
    friend class TxSetBuilder;
#ifdef BUILD_TESTS
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(
        TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
        TxFrameList& invalidTxs, bool enforceTxsApplyOrder,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder,
        bool disableTxValidationForLegacyScenario);
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
                    uint64_t upperBoundCloseTimeOffset,
                    bool txsAreValidated = false) const;
    TxSetValidationResult
    checkValidWithResult(Application& app, uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset,
                         bool txsAreValidated,
                         UnorderedMap<AccountID, int64_t>& accountFeeMap) const;
    TxSetValidationResult
    checkValidWithResult(Application& app, TxSetValidationContext const& ctx,
                         uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset,
                         bool txsAreValidated,
                         UnorderedMap<AccountID, int64_t>& accountFeeMap) const;
    TxSetValidationResult
    checkValidClassic(LedgerHeader const& lclHeader) const;
    TxSetValidationResult
    checkValidSoroban(LedgerHeader const& lclHeader,
                      SorobanNetworkConfig const& sorobanConfig) const;
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
    // Same as checkValid, but returns a specific error code instead of bool.
    TxSetValidationResult
    checkValidWithResult(Application& app, uint64_t lowerBoundCloseTimeOffset,
                         uint64_t upperBoundCloseTimeOffset) const;
    // Same as checkValidWithResult, but against the ledger state described by
    // `ctx` instead of the main thread's LCL. Safe to call off the main
    // thread as long as `ctx` is.
    TxSetValidationResult
    checkValidWithResult(Application& app, TxSetValidationContext const& ctx,
                         uint64_t lowerBoundCloseTimeOffset,
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

    // Returns the sum of all fees that this transaction set would take. Clamps
    // at INT64_MAX.
    int64_t getTotalFees(LedgerHeader const& lh) const;

    // Returns the sum of all _inclusion fee_ bids for all transactions in this
    // set. Clamps at INT64_MAX.
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
    bool checkValidInternal(Application& app,
                            uint64_t lowerBoundCloseTimeOffset,
                            uint64_t upperBoundCloseTimeOffset,
                            bool txsAreValidated) const;
    TxSetValidationResult checkValidInternalWithResult(
        Application& app, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset, bool txsAreValidated) const;
    TxSetValidationResult checkValidInternalWithResult(
        Application& app, TxSetValidationContext const& ctx,
        uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
        bool txsAreValidated) const;

  private:
    friend class TxSetXDRFrame;

    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(
        PerPhaseTransactionList const& txPhases, Application& app,
        ApplyTimeOffset closeTimeOffset,
        PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
        ,
        bool enforceTxsApplyOrder,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder,
        bool disableTxValidationForLegacyScenario
#endif
        ,
        TxSetReadyCallback const& onTxSetReady,
        std::vector<size_t> const& validTxTargets,
        TxValidationCache* validationCache,
        std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck);
    friend class TxSetBuilder;
#ifdef BUILD_TESTS
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(
        TxFrameList txs, Application& app, ApplyTimeOffset closeTimeOffset,
        TxFrameList& invalidTxs, bool enforceTxsApplyOrder,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder,
        bool disableTxValidationForLegacyScenario);
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

    // Everything `checkValidInternalWithResult` checks about the phases of a
    // set, i.e. all of it but the set's parent and kind: one transaction per
    // source account across the phases, and each phase's fees, limits and
    // (unless `txsAreValidated`) transactions. Used on its own for a
    // selection that has no parent yet.
    static TxSetValidationResult
    checkPhasesValid(Application& app, TxSetValidationContext const& ctx,
                     std::vector<TxSetPhaseFrame> const& phases,
                     bool isGeneralized, uint64_t lowerBoundCloseTimeOffset,
                     uint64_t upperBoundCloseTimeOffset, bool txsAreValidated);

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

// The check a builder makes of a set it just built, before the set is relied
// on: the XDR is read back into an applicable set, which must have the shape
// of the selection it was written from and pass the structural validation of
// a set on top of the ledger it was built for (limits, fees, order; the
// transactions themselves were validated by the selection).
//
// It holds copies of everything it reads, so it can run on any thread while
// the main thread goes on with the set.
class TxSetSelfCheck
{
  public:
    // Runs the check. Returns why the set must not be used, or nullopt if it
    // passed. `app` is only read for the network ID and the configuration.
    std::optional<std::string> run(Application& app) const;

    TxSetXDRFrameConstPtr const&
    getTxSet() const
    {
        return mTxSet;
    }

#ifdef BUILD_TESTS
    // The same check of another set: for tests that make a builder produce
    // a broken one.
    std::shared_ptr<TxSetSelfCheck const>
    withTxSetForTesting(TxSetXDRFrameConstPtr const& txSet) const
    {
        std::shared_ptr<TxSetSelfCheck> res(new TxSetSelfCheck(*this));
        res->mTxSet = txSet;
        return res;
    }
#endif

  private:
    friend class TxSetBuilder;
    TxSetSelfCheck() = default;

    TxSetXDRFrameConstPtr mTxSet;
    // The ledger the set was built on top of.
    LedgerHeaderHistoryEntry mLcl;
    std::shared_ptr<SorobanNetworkConfig const> mSorobanConfig;
    // Number of transactions per phase of the selection.
    std::vector<size_t> mPhaseSizes;
    uint64_t mCloseTimeOffset{0};
};

// A generalized transaction set that has been selected but does not name the
// ledger it goes on top of yet: its phases and its XDR with an empty
// `previousLedgerHash`. A leader selects the set for ledger N+1 while ledger N
// is still going through consensus, when N's header hash does not exist; the
// set is completed by `TxSetBuilder::stamp` once N has closed.
struct UnstampedTxSet
{
    std::vector<TxSetPhaseFrame> mPhases;
    GeneralizedTransactionSet mXDR;

    size_t sizeTxTotal() const;
};

// The steps `makeTxSetFromTransactions` is made of, for callers that run them
// apart from each other.
class TxSetBuilder
{
  public:
    // Trims the invalid transactions of every phase and applies surge
    // pricing, as `makeTxSetFromTransactions` does, but against an arbitrary
    // ledger: validity is read through `ctx`, limits from `limits`. Nothing
    // of the main thread's or of ledger apply's state is read, so this may
    // run on any thread (`ctx` then has to ask for a single validation task).
    // `timerPrefix` names the stage timers
    // (`herder.txset.<timerPrefix>-trim` and `-select`).
    static std::vector<TxSetPhaseFrame> selectPhases(
        PerPhaseTransactionList const& txPhases, Application& app,
        TxSetBuildLimits const& limits, TxSetValidationContext const& ctx,
        ApplyTimeOffset closeTimeOffset, PerPhaseTransactionList& invalidTxs,
        std::vector<size_t> const& validTxTargets,
        std::string const& timerPrefix,
        // Randomizes the order of transactions with equal fee
        // rates. Drawn from the global random engine (main thread
        // only) unless provided.
        std::optional<size_t> tieBreakSeed
#ifdef BUILD_TESTS
        ,
        bool enforceTxsApplyOrder = false,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder = {},
        bool disableTxValidationForLegacyScenario = false
#endif
    );

    // The structural validation of a selection (ApplicableTxSetFrame's, less
    // the parent hash): what a set made of `phases` would be rejected for,
    // whatever its parent and its bytes. Cheap next to the check of the
    // written set (TxSetSelfCheck): it reads the frames the selection
    // already holds. Any thread.
    static TxSetValidationResult
    checkSelection(std::vector<TxSetPhaseFrame> const& phases, Application& app,
                   TxSetValidationContext const& ctx, bool isGeneralized,
                   ApplyTimeOffset closeTimeOffset);

    // Writes the XDR of a selection, without a parent, and checks its shape
    // for a ledger of protocol `ledgerVersion` (throws if it is malformed).
    // Generalized sets only. Any thread.
    static UnstampedTxSet makeUnstamped(std::vector<TxSetPhaseFrame>&& phases,
                                        uint32_t ledgerVersion);

    // Completes a set selected ahead of time for the ledger after `lcl`,
    // which must be the main thread's LCL: names `lcl` as the parent and
    // computes the set's hash. The self-check of the result is returned in
    // `selfCheck` and has not been run. Main thread only.
    static std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    stamp(UnstampedTxSet&& txSet, Application& app,
          LedgerHeaderHistoryEntry const& lcl, ApplyTimeOffset closeTimeOffset,
          std::shared_ptr<TxSetSelfCheck const>& selfCheck);

  private:
    friend std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    makeTxSetFromTransactions(
        PerPhaseTransactionList const& txPhases, Application& app,
        ApplyTimeOffset closeTimeOffset,
        PerPhaseTransactionList& invalidTxsPerPhase
#ifdef BUILD_TESTS
        ,
        bool enforceTxsApplyOrder,
        txtest::ParallelSorobanOrder const& parallelSorobanOrder,
        bool disableTxValidationForLegacyScenario
#endif
        ,
        TxSetReadyCallback const& onTxSetReady,
        std::vector<size_t> const& validTxTargets,
        TxValidationCache* validationCache,
        std::shared_ptr<TxSetSelfCheck const>* deferredSelfCheck);
    friend class TxSetSelfCheck;

    static std::shared_ptr<TxSetSelfCheck const>
    makeSelfCheck(Application& app, TxSetXDRFrameConstPtr const& txSet,
                  LedgerHeaderHistoryEntry const& lcl,
                  std::vector<TxSetPhaseFrame> const& phases,
                  ApplyTimeOffset closeTimeOffset);
    // The check itself. `roundTripped`, if given, receives the applicable
    // set read back from the XDR.
    static std::optional<std::string>
    checkBuiltTxSet(Application& app, TxSetXDRFrame const& txSet,
                    TxSetValidationContext const& ctx,
                    std::vector<size_t> const& phaseSizes,
                    uint64_t closeTimeOffset,
                    ApplicableTxSetFrameConstPtr* roundTripped);
};

} // namespace stellar
