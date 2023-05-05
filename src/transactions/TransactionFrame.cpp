// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TransactionFrame.h"
#include "OperationFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "database/Database.h"
#include "database/DatabaseUtils.h"
#include "herder/TxSetFrame.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionMetaFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include <Tracy.hpp>
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <numeric>

namespace stellar
{

using namespace std;
using namespace stellar::txbridge;

TransactionFrame::TransactionFrame(Hash const& networkID,
                                   TransactionEnvelope const& envelope)
    : mEnvelope(envelope), mNetworkID(networkID)
{
    // Create operation frames with dummy results. Currently the proper results
    // are initialized in `TransactionFrame::resetResults` and eventually the
    // operation frames should be decoupled from the results completely and
    // created just once.
    auto& ops = mEnvelope.type() == ENVELOPE_TYPE_TX_V0
                    ? mEnvelope.v0().tx.operations
                    : mEnvelope.v1().tx.operations;
    getResult().result.code(txFAILED);
    getResult().result.results().resize(static_cast<uint32_t>(ops.size()));

    for (size_t i = 0; i < ops.size(); i++)
    {
        mOperations.push_back(
            makeOperation(ops[i], getResult().result.results()[i], i));
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    // Initialize the fee to 0, callers will compute the fee appropriately
    mSorobanResourceFee = std::make_optional<FeePair>();
#endif
}

Hash const&
TransactionFrame::getFullHash() const
{
    if (isZero(mFullHash))
    {
        mFullHash = xdrSha256(mEnvelope);
    }
    return (mFullHash);
}

Hash const&
TransactionFrame::getContentsHash() const
{
#ifdef _DEBUG
    // force recompute
    Hash oldHash;
    std::swap(mContentsHash, oldHash);
#endif

    if (isZero(mContentsHash))
    {
        if (mEnvelope.type() == ENVELOPE_TYPE_TX_V0)
        {
            mContentsHash = sha256(xdr::xdr_to_opaque(
                mNetworkID, ENVELOPE_TYPE_TX, 0, mEnvelope.v0().tx));
        }
        else
        {
            mContentsHash = sha256(xdr::xdr_to_opaque(
                mNetworkID, ENVELOPE_TYPE_TX, mEnvelope.v1().tx));
        }
    }
#ifdef _DEBUG
    releaseAssert(isZero(oldHash) || (oldHash == mContentsHash));
#endif
    return (mContentsHash);
}

void
TransactionFrame::clearCached()
{
    Hash zero;
    mContentsHash = zero;
    mFullHash = zero;
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
void
TransactionFrame::pushContractEvents(OperationEvents const& evts)
{
    mEvents.emplace_back(evts);
}

void
TransactionFrame::pushDiagnosticEvents(OperationDiagnosticEvents const& evts)
{
    mDiagnosticEvents.emplace_back(evts);
}
#endif

TransactionEnvelope const&
TransactionFrame::getEnvelope() const
{
    return mEnvelope;
}

TransactionEnvelope&
TransactionFrame::getEnvelope()
{
    return mEnvelope;
}

SequenceNumber
TransactionFrame::getSeqNum() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0 ? mEnvelope.v0().tx.seqNum
                                                   : mEnvelope.v1().tx.seqNum;
}

AccountID
TransactionFrame::getFeeSourceID() const
{
    return getSourceID();
}

AccountID
TransactionFrame::getSourceID() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX_V0)
    {
        AccountID res;
        res.ed25519() = mEnvelope.v0().tx.sourceAccountEd25519;
        return res;
    }
    return toAccountID(mEnvelope.v1().tx.sourceAccount);
}

uint32_t
TransactionFrame::getNumOperations() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0
               ? static_cast<uint32_t>(mEnvelope.v0().tx.operations.size())
               : static_cast<uint32_t>(mEnvelope.v1().tx.operations.size());
}

std::vector<Operation> const&
TransactionFrame::getRawOperations() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0
               ? mEnvelope.v0().tx.operations
               : mEnvelope.v1().tx.operations;
}

int64_t
TransactionFrame::getFullFee() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0 ? mEnvelope.v0().tx.fee
                                                   : mEnvelope.v1().tx.fee;
}

int64_t
TransactionFrame::getFeeBid() const
{
    int64_t fullFee = getFullFee();
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (!isSoroban())
    {
        return fullFee;
    }
    // We rely here on the Soroban fee being computed at
    // this point.
    releaseAssertOrThrow(mSorobanResourceFee);
    return std::max(fullFee - mSorobanResourceFee->fee,
                    static_cast<int64_t>(0));
#else
    return fullFee;
#endif
}

int64_t
TransactionFrame::getFee(LedgerHeader const& header,
                         std::optional<int64_t> baseFee, bool applying) const
{
    if (!baseFee)
    {
        return getFullFee();
    }
    if (protocolVersionStartsFrom(header.ledgerVersion,
                                  ProtocolVersion::V_11) ||
        !applying)
    {
        int64_t feeBid = getFeeBid();
        int64_t flatFee = getFullFee() - feeBid;
        int64_t adjustedFee =
            *baseFee * std::max<int64_t>(1, getNumOperations());

        if (applying)
        {
            return flatFee + std::min<int64_t>(getFeeBid(), adjustedFee);
        }
        else
        {
            return flatFee + adjustedFee;
        }
    }
    else
    {
        return getFullFee();
    }
}

void
TransactionFrame::addSignature(SecretKey const& secretKey)
{
    auto sig = SignatureUtils::sign(secretKey, getContentsHash());
    addSignature(sig);
}

void
TransactionFrame::addSignature(DecoratedSignature const& signature)
{
    clearCached();
    getSignatures(mEnvelope).push_back(signature);
}

bool
TransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                 LedgerTxnEntry const& account,
                                 int32_t neededWeight)
{
    ZoneScoped;
    auto& acc = account.current().data.account();
    std::vector<Signer> signers;
    if (acc.thresholds[0])
    {
        auto signerKey = KeyUtils::convertKey<SignerKey>(acc.accountID);
        signers.push_back(Signer(signerKey, acc.thresholds[0]));
    }
    signers.insert(signers.end(), acc.signers.begin(), acc.signers.end());

    return signatureChecker.checkSignature(signers, neededWeight);
}

bool
TransactionFrame::checkSignatureNoAccount(SignatureChecker& signatureChecker,
                                          AccountID const& accountID)
{
    ZoneScoped;
    std::vector<Signer> signers;
    auto signerKey = KeyUtils::convertKey<SignerKey>(accountID);
    signers.push_back(Signer(signerKey, 1));
    return signatureChecker.checkSignature(signers, 0);
}

bool
TransactionFrame::checkExtraSigners(SignatureChecker& signatureChecker)
{
    ZoneScoped;
    if (extraSignersExist())
    {
        auto const& extraSigners = mEnvelope.v1().tx.cond.v2().extraSigners;
        std::vector<Signer> signers;

        std::transform(extraSigners.begin(), extraSigners.end(),
                       std::back_inserter(signers),
                       [](SignerKey const& k) { return Signer(k, 1); });

        // Sanity check for the int32 cast below
        static_assert(decltype(PreconditionsV2::extraSigners)::max_size() <=
                      INT32_MAX);

        // We want to verify that there is a signature for each extraSigner, so
        // we assign a weight of 1 to each key, and set the neededWeight to the
        // number of extraSigners
        return signatureChecker.checkSignature(
            signers, static_cast<int32_t>(signers.size()));
    }
    return true;
}

LedgerTxnEntry
TransactionFrame::loadSourceAccount(AbstractLedgerTxn& ltx,
                                    LedgerTxnHeader const& header)
{
    ZoneScoped;
    auto res = loadAccount(ltx, header, getSourceID());
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8))
    {
        // this is buggy caching that existed in old versions of the protocol
        if (res)
        {
            auto newest = ltx.getNewestVersion(LedgerEntryKey(res.current()));
            mCachedAccount = newest;
        }
        else
        {
            mCachedAccount.reset();
        }
    }
    return res;
}

LedgerTxnEntry
TransactionFrame::loadAccount(AbstractLedgerTxn& ltx,
                              LedgerTxnHeader const& header,
                              AccountID const& accountID)
{
    ZoneScoped;
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8) &&
        mCachedAccount &&
        mCachedAccount->ledgerEntry().data.account().accountID == accountID)
    {
        // this is buggy caching that existed in old versions of the protocol
        auto res = stellar::loadAccount(ltx, accountID);
        if (res)
        {
            res.currentGeneralized() = *mCachedAccount;
        }
        else
        {
            res = ltx.create(*mCachedAccount);
        }

        auto newest = ltx.getNewestVersion(LedgerEntryKey(res.current()));
        mCachedAccount = newest;
        return res;
    }
    else
    {
        return stellar::loadAccount(ltx, accountID);
    }
}

bool
TransactionFrame::hasDexOperations() const
{
    for (auto const& op : mOperations)
    {
        if (op->isDexOperation())
        {
            return true;
        }
    }
    return false;
}

bool
TransactionFrame::isSoroban() const
{
    return !mOperations.empty() && mOperations[0]->isSoroban();
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
SorobanResources const&
TransactionFrame::sorobanResources() const
{
    releaseAssertOrThrow(isSoroban());
    return mEnvelope.v1().tx.ext.sorobanData().resources;
}
#endif

std::shared_ptr<OperationFrame>
TransactionFrame::makeOperation(Operation const& op, OperationResult& res,
                                size_t index)
{
    return OperationFrame::makeHelper(op, res, *this,
                                      static_cast<uint32_t>(index));
}

void
TransactionFrame::resetResults(LedgerHeader const& header,
                               std::optional<int64_t> baseFee, bool applying)
{
    auto& ops = mEnvelope.type() == ENVELOPE_TYPE_TX_V0
                    ? mEnvelope.v0().tx.operations
                    : mEnvelope.v1().tx.operations;

    // pre-allocates the results for all operations
    getResult().result.code(txSUCCESS);
    getResult().result.results().resize(static_cast<uint32_t>(ops.size()));

    mOperations.clear();

    // bind operations to the results
    for (size_t i = 0; i < ops.size(); i++)
    {
        mOperations.push_back(
            makeOperation(ops[i], getResult().result.results()[i], i));
    }

    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    getResult().feeCharged = getFee(header, baseFee, applying);
}

std::optional<TimeBounds const> const
TransactionFrame::getTimeBounds() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX_V0)
    {
        return mEnvelope.v0().tx.timeBounds ? std::optional<TimeBounds const>(
                                                  *mEnvelope.v0().tx.timeBounds)
                                            : std::optional<TimeBounds const>();
    }
    else
    {
        auto const& cond = mEnvelope.v1().tx.cond;
        switch (cond.type())
        {
        case PRECOND_NONE:
        {
            return std::optional<TimeBounds const>();
        }
        case PRECOND_TIME:
        {
            return std::optional<TimeBounds const>(cond.timeBounds());
        }
        case PRECOND_V2:
        {
            return cond.v2().timeBounds
                       ? std::optional<TimeBounds const>(*cond.v2().timeBounds)
                       : std::optional<TimeBounds const>();
        }
        default:
            throw std::runtime_error("unknown condition type");
        }
    }
}

std::optional<LedgerBounds const> const
TransactionFrame::getLedgerBounds() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX)
    {
        auto const& cond = mEnvelope.v1().tx.cond;
        if (cond.type() == PRECOND_V2 && cond.v2().ledgerBounds)
        {
            return std::optional<LedgerBounds const>(*cond.v2().ledgerBounds);
        }
    }

    return std::optional<LedgerBounds const>();
}

Duration
TransactionFrame::getMinSeqAge() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX)
    {
        auto& cond = mEnvelope.v1().tx.cond;
        return cond.type() == PRECOND_V2 ? cond.v2().minSeqAge : 0;
    }

    return 0;
}

uint32
TransactionFrame::getMinSeqLedgerGap() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX)
    {
        auto& cond = mEnvelope.v1().tx.cond;
        return cond.type() == PRECOND_V2 ? cond.v2().minSeqLedgerGap : 0;
    }

    return 0;
}

std::optional<SequenceNumber const> const
TransactionFrame::getMinSeqNum() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX)
    {
        auto& cond = mEnvelope.v1().tx.cond;
        if (cond.type() == PRECOND_V2 && cond.v2().minSeqNum)
        {
            return std::optional<SequenceNumber const>(*cond.v2().minSeqNum);
        }
    }

    return std::optional<SequenceNumber const>();
}

bool
TransactionFrame::extraSignersExist() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX &&
           mEnvelope.v1().tx.cond.type() == PRECOND_V2 &&
           !mEnvelope.v1().tx.cond.v2().extraSigners.empty();
}

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
bool
TransactionFrame::validateSorobanOpsConsistency() const
{
    bool hasSorobanOp = mOperations[0]->isSoroban();
    for (auto const& op : mOperations)
    {
        bool isSorobanOp = op->isSoroban();
        // Mixing Soroban ops with non-Soroban ops is not allowed.
        if (isSorobanOp != hasSorobanOp)
        {
            return false;
        }
    }
    // Only one operation is allowed per Soroban transaction.
    if (hasSorobanOp && mOperations.size() != 1)
    {
        return false;
    }
    return true;
}

bool
TransactionFrame::validateSorobanResources(
    SorobanNetworkConfig const& config) const
{
    auto const& resources = sorobanResources();
    auto const& readEntries = resources.footprint.readOnly;
    auto const& writeEntries = resources.footprint.readWrite;
    if (resources.instructions > config.txMaxInstructions())
    {
        return false;
    }
    if (resources.readBytes > config.txMaxReadBytes())
    {
        return false;
    }
    if (resources.writeBytes > config.txMaxWriteBytes())
    {
        return false;
    }
    if (resources.extendedMetaDataSizeBytes >
        config.txMaxExtendedMetaDataSizeBytes())
    {
        return false;
    }
    if (readEntries.size() + writeEntries.size() >
            config.txMaxReadLedgerEntries() ||
        writeEntries.size() > config.txMaxWriteLedgerEntries())
    {
        return false;
    }
    for (auto const& lk : readEntries)
    {
        if (xdr::xdr_size(lk) > config.maxContractDataKeySizeBytes())
        {
            return false;
        }
    }
    for (auto const& lk : writeEntries)
    {
        if (xdr::xdr_size(lk) > config.maxContractDataKeySizeBytes())
        {
            return false;
        }
    }
    auto txSize = xdr::xdr_size(mEnvelope.v1().tx);
    if (txSize > config.txMaxSizeBytes())
    {
        return false;
    }
    return true;
}

void
TransactionFrame::maybeComputeSorobanResourceFee(
    uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
    Config const& cfg)
{
    maybeComputeSorobanResourceFee(protocolVersion, sorobanConfig, cfg, false);
}

void
TransactionFrame::maybeComputeSorobanResourceFee(
    uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
    Config const& cfg, bool applyRefund)
{
    if (!isSoroban())
    {
        return;
    }
    // At this point the frame might not have been validated yet, so
    // the resources might not be present or Soroban might not be supported
    // at all. Hence just set the resource fees to 0 and rely on validation
    // checks to not use this.
    if (protocolVersionIsBefore(protocolVersion, SOROBAN_PROTOCOL_VERSION) ||
        mEnvelope.type() != ENVELOPE_TYPE_TX || mEnvelope.v1().tx.ext.v() != 1)
    {
        mSorobanResourceFee = std::make_optional<FeePair>();
        return;
    }
    CxxTransactionResources cxxResources;
    auto const& txResources = sorobanResources();
    cxxResources.instructions = txResources.instructions;

    cxxResources.read_entries = txResources.footprint.readOnly.size() +
                                txResources.footprint.readWrite.size();
    cxxResources.write_entries = txResources.footprint.readWrite.size();

    cxxResources.read_bytes = txResources.readBytes;
    cxxResources.write_bytes = txResources.writeBytes;

    cxxResources.transaction_size_bytes = xdr::xdr_size(mEnvelope.v1().tx);

    cxxResources.metadata_size_bytes = txResources.extendedMetaDataSizeBytes;

    if (applyRefund)
    {
        // It is possible that consumed metadata size is higher than the
        // declared size (in such a case the transaction will fail). We
        // still don't want to overcharge the fees though.
        if (cxxResources.metadata_size_bytes > mConsumedSorobanMetadataSize)
        {
            cxxResources.metadata_size_bytes = mConsumedSorobanMetadataSize;
        }
    }

    // NB: We recompute this on-demand in case if the fees change between
    // the ledger where the transaction has been accepted and the ledger where
    // it is being applied.
    // This may throw, but only in case of the Core version misconfiguration.
    mSorobanResourceFee = std::make_optional<FeePair>(
        rust_bridge::compute_transaction_resource_fee(
            cfg.CURRENT_LEDGER_PROTOCOL_VERSION, protocolVersion, cxxResources,
            sorobanConfig.rustBridgeFeeConfiguration()));
    // This is the fee computation invariant.
    releaseAssertOrThrow(mSorobanResourceFee->fee >=
                         mSorobanResourceFee->refundable_fee);
}
void
TransactionFrame::consumeRefundableSorobanResource(uint32_t metadataSizeBytes)
{
    mConsumedSorobanMetadataSize += metadataSizeBytes;
}

#endif

bool
TransactionFrame::isTooEarly(LedgerTxnHeader const& header,
                             uint64_t lowerBoundCloseTimeOffset) const
{
    auto const tb = getTimeBounds();
    if (tb)
    {
        uint64 closeTime = header.current().scpValue.closeTime;
        if (tb->minTime &&
            (tb->minTime > (closeTime + lowerBoundCloseTimeOffset)))
        {
            return true;
        }
    }

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        auto const lb = getLedgerBounds();
        return lb && lb->minLedger > header.current().ledgerSeq;
    }

    return false;
}

bool
TransactionFrame::isTooLate(LedgerTxnHeader const& header,
                            uint64_t upperBoundCloseTimeOffset) const
{
    auto const tb = getTimeBounds();
    if (tb)
    {
        // Prior to consensus, we can pass in an upper bound estimate on when we
        // expect the ledger to close so we don't accept transactions that will
        // expire by the time they are applied
        uint64 closeTime = header.current().scpValue.closeTime;
        if (tb->maxTime &&
            (tb->maxTime < (closeTime + upperBoundCloseTimeOffset)))
        {
            return true;
        }
    }

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        auto const lb = getLedgerBounds();
        return lb && lb->maxLedger != 0 &&
               lb->maxLedger <= header.current().ledgerSeq;
    }
    return false;
}

bool
TransactionFrame::isTooEarlyForAccount(LedgerTxnHeader const& header,
                                       LedgerTxnEntry const& sourceAccount,
                                       uint64_t lowerBoundCloseTimeOffset) const
{
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_19))
    {
        return false;
    }

    auto accountEntry = [&]() -> AccountEntry const& {
        return sourceAccount.current().data.account();
    };

    auto accSeqTime = hasAccountEntryExtV3(accountEntry())
                          ? getAccountEntryExtensionV3(accountEntry()).seqTime
                          : 0;
    auto minSeqAge = getMinSeqAge();

    auto lowerBoundCloseTime =
        header.current().scpValue.closeTime + lowerBoundCloseTimeOffset;
    if (minSeqAge > lowerBoundCloseTime ||
        lowerBoundCloseTime - minSeqAge < accSeqTime)
    {
        return true;
    }

    auto accSeqLedger =
        hasAccountEntryExtV3(accountEntry())
            ? getAccountEntryExtensionV3(accountEntry()).seqLedger
            : 0;
    auto minSeqLedgerGap = getMinSeqLedgerGap();

    auto ledgerSeq = header.current().ledgerSeq;
    if (minSeqLedgerGap > ledgerSeq ||
        ledgerSeq - minSeqLedgerGap < accSeqLedger)
    {
        return true;
    }

    return false;
}

bool
TransactionFrame::commonValidPreSeqNum(Application& app, AbstractLedgerTxn& ltx,
                                       bool chargeFee,
                                       uint64_t lowerBoundCloseTimeOffset,
                                       uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    if ((protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13) &&
         (mEnvelope.type() == ENVELOPE_TYPE_TX ||
          hasMuxedAccount(mEnvelope))) ||
        (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_13) &&
         mEnvelope.type() == ENVELOPE_TYPE_TX_V0))
    {
        getResult().result.code(txNOT_SUPPORTED);
        return false;
    }

    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_19) &&
        mEnvelope.type() == ENVELOPE_TYPE_TX &&
        mEnvelope.v1().tx.cond.type() == PRECOND_V2)
    {
        getResult().result.code(txNOT_SUPPORTED);
        return false;
    }

    if (extraSignersExist())
    {
        auto const& extraSigners = mEnvelope.v1().tx.cond.v2().extraSigners;

        static_assert(decltype(PreconditionsV2::extraSigners)::max_size() == 2);
        if (extraSigners.size() == 2 && extraSigners[0] == extraSigners[1])
        {
            getResult().result.code(txMALFORMED);
            return false;
        }

        for (auto const& signer : extraSigners)
        {
            if (signer.type() == SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD &&
                signer.ed25519SignedPayload().payload.empty())
            {
                getResult().result.code(txMALFORMED);
                return false;
            }
        }
    }

    if (getNumOperations() == 0)
    {
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (!validateSorobanOpsConsistency())
    {
        getResult().result.code(txMALFORMED);
        return false;
    }
    if (isSoroban())
    {
        if (protocolVersionIsBefore(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
        {
            getResult().result.code(txMALFORMED);
            return false;
        }
        if (mEnvelope.type() != ENVELOPE_TYPE_TX ||
            mEnvelope.v1().tx.ext.v() != 1)
        {
            getResult().result.code(txMALFORMED);
            return false;
        }
        auto const& sorobanConfig =
            app.getLedgerManager().getSorobanNetworkConfig(ltx);
        if (!validateSorobanResources(sorobanConfig))
        {
            getResult().result.code(txSOROBAN_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        // Full fee has to be greater than the resource fee or
        // tx-specified refundable fee.
        if (getFullFee() < mSorobanResourceFee->fee ||
            getFullFee() < mEnvelope.v1().tx.ext.sorobanData().refundableFee)
        {
            getResult().result.code(txINSUFFICIENT_FEE);
            return false;
        }
        // Refundable fee shouldn't exceed tx-specified refundable fee.
        if (mEnvelope.v1().tx.ext.sorobanData().refundableFee <
            mSorobanResourceFee->refundable_fee)
        {
            getResult().result.code(txINSUFFICIENT_FEE);
            return false;
        }
    }
#endif
    auto header = ltx.loadHeader();
    if (isTooEarly(header, lowerBoundCloseTimeOffset))
    {
        getResult().result.code(txTOO_EARLY);
        return false;
    }
    if (isTooLate(header, upperBoundCloseTimeOffset))
    {
        getResult().result.code(txTOO_LATE);
        return false;
    }

    if (chargeFee && getFeeBid() < getMinFee(*this, header.current()))
    {
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }
    if (!chargeFee && getFeeBid() < 0)
    {
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!loadSourceAccount(ltx, header))
    {
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    return true;
}

void
TransactionFrame::processSeqNum(AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    auto header = ltx.loadHeader();
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10))
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        if (sourceAccount.current().data.account().seqNum > getSeqNum())
        {
            throw std::runtime_error("unexpected sequence number");
        }
        sourceAccount.current().data.account().seqNum = getSeqNum();

        maybeUpdateAccountOnLedgerSeqUpdate(header, sourceAccount);
    }
}

bool
TransactionFrame::processSignatures(ValidationType cv,
                                    SignatureChecker& signatureChecker,
                                    AbstractLedgerTxn& ltxOuter)
{
    ZoneScoped;
    bool maybeValid = (cv == ValidationType::kMaybeValid);
    uint32_t ledgerVersion = ltxOuter.loadHeader().current().ledgerVersion;
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
    {
        return maybeValid;
    }

    // check if we need to fast fail and use the original error code
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_13) &&
        !maybeValid)
    {
        removeOneTimeSignerFromAllSourceAccounts(ltxOuter);
        return false;
    }
    // older versions of the protocol only fast fail in a subset of cases
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13) &&
        cv < ValidationType::kInvalidPostAuth)
    {
        return false;
    }

    bool allOpsValid = true;
    {
        // scope here to avoid potential side effects of loading source accounts
        LedgerTxn ltx(ltxOuter);
        for (auto& op : mOperations)
        {
            if (!op->checkSignature(signatureChecker, ltx, false))
            {
                allOpsValid = false;
            }
        }
    }

    removeOneTimeSignerFromAllSourceAccounts(ltxOuter);

    if (!allOpsValid)
    {
        markResultFailed();
        return false;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        getResult().result.code(txBAD_AUTH_EXTRA);
        return false;
    }

    return maybeValid;
}

bool
TransactionFrame::isBadSeq(LedgerTxnHeader const& header, int64_t seqNum) const
{
    if (getSeqNum() == getStartingSequenceNumber(header))
    {
        return true;
    }

    // If seqNum == INT64_MAX, seqNum >= getSeqNum() is guaranteed to be true
    // because SequenceNumber is int64, so isBadSeq will always return true in
    // that case.
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        // Check if we need to relax sequence number checking
        auto minSeqNum = getMinSeqNum();
        if (minSeqNum)
        {
            return seqNum < *minSeqNum || seqNum >= getSeqNum();
        }
    }

    // If we get here, we need to do the strict seqnum check
    return seqNum == INT64_MAX || seqNum + 1 != getSeqNum();
}

TransactionFrame::ValidationType
TransactionFrame::commonValid(Application& app,
                              SignatureChecker& signatureChecker,
                              AbstractLedgerTxn& ltxOuter,
                              SequenceNumber current, bool applying,
                              bool chargeFee,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    LedgerTxn ltx(ltxOuter);
    ValidationType res = ValidationType::kInvalid;

    if (applying &&
        (lowerBoundCloseTimeOffset != 0 || upperBoundCloseTimeOffset != 0))
    {
        throw std::logic_error(
            "Applying transaction with non-current closeTime");
    }

    if (!commonValidPreSeqNum(app, ltx, chargeFee, lowerBoundCloseTimeOffset,
                              upperBoundCloseTimeOffset))
    {
        return res;
    }

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    // in older versions, the account's sequence number is updated when taking
    // fees
    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_10) ||
        !applying)
    {
        if (current == 0)
        {
            current = sourceAccount.current().data.account().seqNum;
        }
        if (isBadSeq(header, current))
        {
            getResult().result.code(txBAD_SEQ);
            return res;
        }
    }

    res = ValidationType::kInvalidUpdateSeqNum;

    if (isTooEarlyForAccount(header, sourceAccount, lowerBoundCloseTimeOffset))
    {
        getResult().result.code(txBAD_MIN_SEQ_AGE_OR_GAP);
        return res;
    }

    if (!checkSignature(
            signatureChecker, sourceAccount,
            sourceAccount.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        getResult().result.code(txBAD_AUTH);
        return res;
    }

    if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                  ProtocolVersion::V_19) &&
        !checkExtraSigners(signatureChecker))
    {
        getResult().result.code(txBAD_AUTH);
        return res;
    }

    res = ValidationType::kInvalidPostAuth;

    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    uint32_t feeToPay =
        (applying && protocolVersionStartsFrom(header.current().ledgerVersion,
                                               ProtocolVersion::V_9))
            ? 0
            : static_cast<uint32_t>(getFeeBid());
    // don't let the account go below the reserve after accounting for
    // liabilities
    if (chargeFee && getAvailableBalance(header, sourceAccount) < feeToPay)
    {
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return res;
    }

    return ValidationType::kMaybeValid;
}

void
TransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                   std::optional<int64_t> baseFee)
{
    ZoneScoped;
    mCachedAccount.reset();

    auto header = ltx.loadHeader();
    resetResults(header.current(), baseFee, true);

    auto sourceAccount = loadSourceAccount(ltx, header);
    if (!sourceAccount)
    {
        throw std::runtime_error("Unexpected database state");
    }

    auto& acc = sourceAccount.current().data.account();

    int64_t& fee = getResult().feeCharged;
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.current().feePool += fee;
    }
    // in v10 we update sequence numbers during apply
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_10))
    {
        if (acc.seqNum + 1 != getSeqNum())
        {
            // this should not happen as the transaction set is sanitized for
            // sequence numbers
            throw std::runtime_error("Unexpected account state");
        }
        acc.seqNum = getSeqNum();
    }
}

void
TransactionFrame::removeOneTimeSignerFromAllSourceAccounts(
    AbstractLedgerTxn& ltx) const
{
    auto ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    if (ledgerVersion == 7)
    {
        return;
    }

    UnorderedSet<AccountID> accounts{getSourceID()};
    for (auto& op : mOperations)
    {
        accounts.emplace(op->getSourceID());
    }

    auto signerKey = SignerKeyUtils::preAuthTxKey(*this);
    for (auto const& accountID : accounts)
    {
        removeAccountSigner(ltx, accountID, signerKey);
    }
}

void
TransactionFrame::removeAccountSigner(AbstractLedgerTxn& ltxOuter,
                                      AccountID const& accountID,
                                      SignerKey const& signerKey) const
{
    ZoneScoped;
    LedgerTxn ltx(ltxOuter);

    auto account = stellar::loadAccount(ltx, accountID);
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto header = ltx.loadHeader();
    auto& signers = account.current().data.account().signers;
    auto findRes = findSignerByKey(signers.begin(), signers.end(), signerKey);
    if (findRes.second)
    {
        removeSignerWithPossibleSponsorship(ltx, header, findRes.first,
                                            account);
        ltx.commit();
    }
}

bool
TransactionFrame::checkValidWithOptionallyChargedFee(
    Application& app, AbstractLedgerTxn& ltxOuter, SequenceNumber current,
    bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    mCachedAccount.reset();

    LedgerTxn ltx(ltxOuter);
    int64_t minBaseFee = ltx.loadHeader().current().baseFee;
    if (!chargeFee)
    {
        minBaseFee = 0;
    }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    maybeComputeSorobanResourceFee(
        ltx.loadHeader().current().ledgerVersion,
        app.getLedgerManager().getSorobanNetworkConfig(ltx), app.getConfig(),
        false);

#endif
    resetResults(ltx.loadHeader().current(), minBaseFee, false);

    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(),
                                      getSignatures(mEnvelope)};
    bool res =
        commonValid(app, signatureChecker, ltx, current, false, chargeFee,
                    lowerBoundCloseTimeOffset,
                    upperBoundCloseTimeOffset) == ValidationType::kMaybeValid;
    if (res)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(app, signatureChecker, ltx, false))
            {
                // it's OK to just fast fail here and not try to call
                // checkValid on all operations as the resulting object
                // is only used by applications
                markResultFailed();
                return false;
            }
        }

        if (!signatureChecker.checkAllSignaturesUsed())
        {
            res = false;
            getResult().result.code(txBAD_AUTH_EXTRA);
        }
    }
    return res;
}

bool
TransactionFrame::checkValid(Application& app, AbstractLedgerTxn& ltxOuter,
                             SequenceNumber current,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset)
{
    return checkValidWithOptionallyChargedFee(app, ltxOuter, current, true,
                                              lowerBoundCloseTimeOffset,
                                              upperBoundCloseTimeOffset);
}

void
TransactionFrame::insertKeysForFeeProcessing(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(accountKey(getSourceID()));
}

void
TransactionFrame::insertKeysForTxApply(UnorderedSet<LedgerKey>& keys) const
{
    for (auto const& op : mOperations)
    {
        if (!(getSourceID() == op->getSourceID()))
        {
            keys.emplace(accountKey(op->getSourceID()));
        }
        op->insertLedgerKeysToPrefetch(keys);
    }
}

void
TransactionFrame::markResultFailed()
{
    // Changing "code" normally causes the XDR structure to be destructed,
    // then a different XDR structure is constructed. However, txFAILED and
    // txSUCCESS have the same underlying field number so this does not
    // occur.
    getResult().result.code(txFAILED);
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx)
{
    TransactionMetaFrame tm(ltx.loadHeader().current().ledgerVersion);
    return apply(app, ltx, tm);
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  Application& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& outerMeta)
{
    ZoneScoped;
    auto& internalErrorCounter = app.getMetrics().NewCounter(
        {"ledger", "transaction", "internal-error"});
    bool reportInternalErrOnException = true;
    try
    {
        bool success = true;

        xdr::xvector<OperationMeta> operationMetas;
        operationMetas.reserve(getNumOperations());

        // shield outer scope of any side effects with LedgerTxn
        LedgerTxn ltxTx(ltx);
        uint32_t ledgerVersion = ltxTx.loadHeader().current().ledgerVersion;
        // We do not want to increase the internal-error metric count for
        // older ledger versions. The minimum ledger version for which we
        // start internal-error counting is defined in the app config.
        reportInternalErrOnException =
            ledgerVersion >=
            app.getConfig().LEDGER_PROTOCOL_MIN_VERSION_INTERNAL_ERROR_REPORT;
        auto& opTimer =
            app.getMetrics().NewTimer({"ledger", "operation", "apply"});

        for (auto& op : mOperations)
        {
            auto time = opTimer.TimeScope();
            LedgerTxn ltxOp(ltxTx);
            bool txRes = op->apply(app, signatureChecker, ltxOp);

            if (!txRes)
            {
                success = false;
            }
            if (success)
            {
                app.getInvariantManager().checkOnOperationApply(
                    op->getOperation(), op->getResult(), ltxOp.getDelta());

                // The operation meta will be empty if the transaction
                // doesn't succeed so we may as well not do any work in that
                // case
                operationMetas.emplace_back(ltxOp.getChanges());
            }

            if (txRes ||
                protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_14))
            {
                ltxOp.commit();
            }
        }

        if (success)
        {
            LedgerEntryChanges changesAfter;

            if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
            {
                if (!signatureChecker.checkAllSignaturesUsed())
                {
                    getResult().result.code(txBAD_AUTH_EXTRA);
                    // this should never happen: malformed transaction
                    // should not be accepted by nodes
                    return false;
                }

                // if an error occurred, it is responsibility of account's
                // owner to remove that signer
                LedgerTxn ltxAfter(ltxTx);
                removeOneTimeSignerFromAllSourceAccounts(ltxAfter);
                changesAfter = ltxAfter.getChanges();
                ltxAfter.commit();
            }
            else if (protocolVersionStartsFrom(ledgerVersion,
                                               ProtocolVersion::V_14) &&
                     ltxTx.hasSponsorshipEntry())
            {
                getResult().result.code(txBAD_SPONSORSHIP);
                return false;
            }

            ltxTx.commit();
            // commit -> propagate the meta to the outer scope
            outerMeta.pushOperationMetas(std::move(operationMetas));
            outerMeta.pushTxChangesAfter(std::move(changesAfter));
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            auto isSorobanTx = isSoroban();
            if ((isSorobanTx && mOperations.size() != mEvents.size()) ||
                (!isSorobanTx && !mEvents.empty()))
            {
                throw std::runtime_error("unexpected events size");
            }

            outerMeta.pushContractEvents(std::move(mEvents));
            outerMeta.pushDiagnosticEvents(std::move(mDiagnosticEvents));
#endif
        }
        else
        {
            markResultFailed();
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            outerMeta.pushDiagnosticEvents(std::move(mDiagnosticEvents));
#endif
        }
        return success;
    }
    catch (InvariantDoesNotHold& e)
    {
        printErrorAndAbort("Invariant failure while applying operations: ",
                           e.what());
    }
    catch (std::bad_alloc& e)
    {
        printErrorAndAbort("Exception while applying operations: ", e.what());
    }
    catch (std::exception& e)
    {
        if (reportInternalErrOnException)
        {
            CLOG_ERROR(Tx, "Exception while applying operations ({}, {}): {}",
                       xdr_to_string(getFullHash(), "fullHash"),
                       xdr_to_string(getContentsHash(), "contentsHash"),
                       e.what());
        }
        else
        {
            CLOG_INFO(Tx,
                      "Exception occurred on outdated protocol version "
                      "while applying operations ({}, {}): {}",
                      xdr_to_string(getFullHash(), "fullHash"),
                      xdr_to_string(getContentsHash(), "contentsHash"),
                      e.what());
        }
    }
    catch (...)
    {
        if (reportInternalErrOnException)
        {
            CLOG_ERROR(Tx,
                       "Unknown exception while applying operations ({}, {})",
                       xdr_to_string(getFullHash(), "fullHash"),
                       xdr_to_string(getContentsHash(), "contentsHash"));
        }
        else
        {
            CLOG_INFO(Tx,
                      "Unknown exception on outdated protocol version "
                      "while applying operations ({}, {})",
                      xdr_to_string(getFullHash(), "fullHash"),
                      xdr_to_string(getContentsHash(), "contentsHash"));
        }
    }
    if (app.getConfig().HALT_ON_INTERNAL_TRANSACTION_ERROR)
    {
        printErrorAndAbort("Encountered an exception while applying "
                           "operations, see logs for details.");
    }
    // This is only reachable if an exception is thrown
    getResult().result.code(txINTERNAL_ERROR);

    // We only increase the internal-error metric count if the ledger is a
    // newer version.
    if (reportInternalErrOnException)
    {
        internalErrorCounter.inc();
    }

    // operations and txChangesAfter should already be empty at this point
    outerMeta.clearOperationMetas();
    outerMeta.clearTxChangesAfter();
    return false;
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                        TransactionMetaFrame& meta, bool chargeFee)
{
    ZoneScoped;
    try
    {
        mCachedAccount.reset();
        uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
        SignatureChecker signatureChecker{ledgerVersion, getContentsHash(),
                                          getSignatures(mEnvelope)};

        LedgerTxn ltxTx(ltx);
        //  when applying, a failure during tx validation means that
        //  we'll skip trying to apply operations but we'll still
        //  process the sequence number if needed
        auto cv =
            commonValid(app, signatureChecker, ltxTx, 0, true, chargeFee, 0, 0);
        if (cv >= ValidationType::kInvalidUpdateSeqNum)
        {
            processSeqNum(ltxTx);
        }

        bool signaturesValid = processSignatures(cv, signatureChecker, ltxTx);

        meta.pushTxChangesBefore(ltxTx.getChanges());
        ltxTx.commit();

        bool ok = signaturesValid && cv == ValidationType::kMaybeValid;
        try
        {
            // This should only throw if the logging during exception
            // handling for applyOperations throws. In that case, we may not
            // have the correct TransactionResult so we must crash.
            if (ok)
            {
                ok = applyOperations(signatureChecker, app, ltx, meta);
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                // Recompute the Soroban resource fee based on the actual
                // resource consumption.
                maybeComputeSorobanResourceFee(
                    ledgerVersion,
                    app.getLedgerManager().getSorobanNetworkConfig(ltx),
                    app.getConfig(), true);
#endif
            }
            return ok;
        }
        catch (std::exception& e)
        {
            printErrorAndAbort("Exception while applying operations: ",
                               e.what());
        }
        catch (...)
        {
            printErrorAndAbort("Unknown exception while applying operations");
        }
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("Exception after processing fees but before "
                           "processing sequence number: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort("Unknown exception after processing fees but before "
                           "processing sequence number");
    }
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                        TransactionMetaFrame& meta)
{
    return apply(app, ltx, meta, true);
}

StellarMessage
TransactionFrame::toStellarMessage() const
{
    StellarMessage msg(TRANSACTION);
    msg.transaction() = mEnvelope;
    return msg;
}
}
