#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/InternalLedgerEntry.h"
#include "ledger/NetworkConfig.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "rust/RustBridge.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMetaFrame.h"
#include "util/GlobalChecks.h"
#include "util/types.h"
#include "xdr/Stellar-ledger.h"

#include <memory>
#include <optional>
#include <set>

namespace soci
{
class session;
}

/*
A transaction in its exploded form.
We can get it in from the DB or from the wire
*/
namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class OperationFrame;
class LedgerManager;
class LedgerTxnEntry;
class LedgerTxnHeader;
class SecretKey;
class SignatureChecker;
class MutableTransactionResultBase;
class SorobanTxData;
class XDROutputFileStream;
class SHA256;
class AppConnector;

class TransactionFrame;
using TransactionFramePtr = std::shared_ptr<TransactionFrame>;

class TransactionFrame : public TransactionFrameBase
{
  private:
    uint32_t getSize() const;

  protected:
#ifdef BUILD_TESTS
    mutable
#else
    const
#endif
        TransactionEnvelope mEnvelope;

    // Only used to preserve a bug before Protocol 8
    mutable std::shared_ptr<InternalLedgerEntry const>
        mCachedAccountPreProtocol8;

    Hash const& mNetworkID;     // used to change the way we compute signatures
    mutable Hash mContentsHash; // the hash of the contents
    mutable Hash mFullHash;     // the hash of the contents and the sig.

    std::vector<std::shared_ptr<OperationFrame const>> mOperations;

    LedgerTxnEntry loadSourceAccount(AbstractLedgerTxn& ltx,
                                     LedgerTxnHeader const& header) const;
    friend class LedgerTxnReadOnly;

    enum ValidationType
    {
        kInvalid,             // transaction is not valid at all
        kInvalidUpdateSeqNum, // transaction is invalid but its sequence number
                              // should be updated
        kInvalidPostAuth,     // transaction is invalid but its sequence number
                              // should be updated and one-time signers removed
        kMaybeValid
    };

    virtual bool isTooEarly(LedgerHeaderWrapper const& header,
                            uint64_t lowerBoundCloseTimeOffset) const;
    virtual bool isTooLate(LedgerHeaderWrapper const& header,
                           uint64_t upperBoundCloseTimeOffset) const;

    bool isTooEarlyForAccount(LedgerHeaderWrapper const& header,
                              LedgerEntryWrapper const& sourceAccount,
                              uint64_t lowerBoundCloseTimeOffset) const;

    bool commonValidPreSeqNum(AppConnector& app,
                              std::optional<SorobanNetworkConfig> const& cfg,
                              LedgerSnapshot const& ls, bool chargeFee,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              std::optional<FeePair> sorobanResourceFee,
                              MutableTxResultPtr txResult) const;

    virtual bool isBadSeq(LedgerHeaderWrapper const& header,
                          int64_t seqNum) const;

    ValidationType commonValid(AppConnector& app,
                               std::optional<SorobanNetworkConfig> const& cfg,
                               SignatureChecker& signatureChecker,
                               LedgerSnapshot const& ls, SequenceNumber current,
                               bool applying, bool chargeFee,
                               uint64_t lowerBoundCloseTimeOffset,
                               uint64_t upperBoundCloseTimeOffset,
                               std::optional<FeePair> sorobanResourceFee,
                               MutableTxResultPtr txResult) const;

    void removeOneTimeSignerFromAllSourceAccounts(AbstractLedgerTxn& ltx) const;

    void removeAccountSigner(AbstractLedgerTxn& ltxOuter,
                             AccountID const& accountID,
                             SignerKey const& signerKey) const;

    bool applyOperations(SignatureChecker& checker, AppConnector& app,
                         AbstractLedgerTxn& ltx, TransactionMetaFrame& meta,
                         MutableTransactionResultBase& txResult,
                         Hash const& sorobanBasePrngSeed) const;

    void processSeqNum(AbstractLedgerTxn& ltx) const;

    bool processSignatures(ValidationType cv,
                           SignatureChecker& signatureChecker,
                           AbstractLedgerTxn& ltxOuter,
                           MutableTransactionResultBase& txResult) const;

    std::optional<TimeBounds const> const getTimeBounds() const;
    std::optional<LedgerBounds const> const getLedgerBounds() const;
    bool extraSignersExist() const;

    bool validateSorobanOpsConsistency() const;
    bool validateSorobanResources(SorobanNetworkConfig const& config,
                                  Config const& appConfig,
                                  uint32_t protocolVersion,
                                  SorobanTxData& sorobanData) const;
    int64_t refundSorobanFee(AbstractLedgerTxn& ltx, AccountID const& feeSource,
                             MutableTransactionResultBase& txResult) const;
    void updateSorobanMetrics(AppConnector& app) const;
#ifdef BUILD_TESTS
  public:
#endif
    FeePair
    computePreApplySorobanResourceFee(uint32_t protocolVersion,
                                      SorobanNetworkConfig const& sorobanConfig,
                                      Config const& cfg) const;

  public:
    TransactionFrame(Hash const& networkID,
                     TransactionEnvelope const& envelope);
    TransactionFrame(TransactionFrame const&) = delete;
    TransactionFrame() = delete;

    virtual ~TransactionFrame()
    {
    }

    Hash const& getFullHash() const override;
    Hash const& getContentsHash() const override;
    TransactionEnvelope const& getEnvelope() const override;

    std::vector<std::shared_ptr<OperationFrame const>> const&
    getOperations() const
    {
        return mOperations;
    }

#ifdef BUILD_TESTS
    TransactionEnvelope& getMutableEnvelope() const override;
    void clearCached() const override;

    bool
    isTestTx() const override
    {
        return false;
    }
#endif

    SequenceNumber getSeqNum() const override;

    AccountID getFeeSourceID() const override;
    AccountID getSourceID() const override;

    uint32_t getNumOperations() const override;
    Resource getResources(bool useByteLimitInClassic) const override;

    std::vector<Operation> const& getRawOperations() const override;

    int64_t getFullFee() const override;
    int64_t getInclusionFee() const override;

    virtual int64_t getFee(LedgerHeader const& header,
                           std::optional<int64_t> baseFee,
                           bool applying) const override;

    bool checkSignature(SignatureChecker& signatureChecker,
                        LedgerEntryWrapper const& account,
                        int32_t neededWeight) const;

    bool checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                 AccountID const& accountID) const;
    bool checkExtraSigners(SignatureChecker& signatureChecker) const;

    MutableTxResultPtr checkValidWithOptionallyChargedFee(
        AppConnector& app, LedgerSnapshot const& ls, SequenceNumber current,
        bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
        uint64_t upperBoundCloseTimeOffset) const;
    MutableTxResultPtr
    checkValid(AppConnector& app, LedgerSnapshot const& ls,
               SequenceNumber current, uint64_t lowerBoundCloseTimeOffset,
               uint64_t upperBoundCloseTimeOffset) const override;
    bool checkSorobanResourceAndSetError(
        AppConnector& app, SorobanNetworkConfig const& cfg,
        uint32_t ledgerVersion, MutableTxResultPtr txResult) const override;

    MutableTxResultPtr createSuccessResult() const override;

    MutableTxResultPtr
    createSuccessResultWithFeeCharged(LedgerHeader const& header,
                                      std::optional<int64_t> baseFee,
                                      bool applying) const override;

    void
    insertKeysForFeeProcessing(UnorderedSet<LedgerKey>& keys) const override;
    void insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                              LedgerKeyMeter* lkMeter) const override;

    // collect fee, consume sequence number
    MutableTxResultPtr
    processFeeSeqNum(AbstractLedgerTxn& ltx,
                     std::optional<int64_t> baseFee) const override;

    // apply this transaction to the current ledger
    // returns true if successfully applied
    bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, MutableTxResultPtr txResult,
               bool chargeFee, Hash const& sorobanBasePrngSeed) const;
    bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
               TransactionMetaFrame& meta, MutableTxResultPtr txResult,
               Hash const& sorobanBasePrngSeed = Hash{}) const override;

    // Performs the necessary post-apply transaction processing.
    // This has to be called after both `processFeeSeqNum` and
    // `apply` have been called.
    // Currently this only takes care of Soroban fee refunds.
    void processPostApply(AppConnector& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          MutableTxResultPtr txResult) const override;

    // TransactionFrame specific function that allows fee bumps to forward a
    // different account for the refund. It also returns the refund so
    // FeeBumpTransactionFrame can adjust feeCharged.
    int64_t processRefund(AppConnector& app, AbstractLedgerTxn& ltx,
                          TransactionMetaFrame& meta,
                          AccountID const& feeSource,
                          MutableTransactionResultBase& txResult) const;

    // version without meta
    bool apply(AppConnector& app, AbstractLedgerTxn& ltx,
               MutableTxResultPtr txResult,
               Hash const& sorobanBasePrngSeed) const;

    std::shared_ptr<StellarMessage const> toStellarMessage() const override;

    LedgerTxnEntry loadAccount(AbstractLedgerTxn& ltx,
                               LedgerTxnHeader const& header,
                               AccountID const& accountID) const;

    std::optional<SequenceNumber const> const getMinSeqNum() const override;
    Duration getMinSeqAge() const override;
    uint32 getMinSeqLedgerGap() const override;

    bool hasDexOperations() const override;

    bool isSoroban() const override;
    SorobanResources const& sorobanResources() const override;

    static FeePair computeSorobanResourceFee(
        uint32_t protocolVersion, SorobanResources const& txResources,
        uint32_t txSize, uint32_t eventsSize,
        SorobanNetworkConfig const& sorobanConfig, Config const& cfg);
    virtual int64 declaredSorobanResourceFee() const override;
    virtual bool XDRProvidesValidFee() const override;

#ifdef BUILD_TESTS
    friend class TransactionTestFrame;
#endif
};
}
