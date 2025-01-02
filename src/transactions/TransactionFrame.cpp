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
#include "ledger/LedgerTypeUtils.h"
#include "ledger/SorobanMetrics.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include "transactions/MutableTransactionResult.h"
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
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include <Tracy.hpp>
#include <iterator>
#include <string>

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <xdrpp/types.h>

namespace stellar
{
namespace
{
// Limit to the maximum resource fee allowed for transaction,
// roughly 112 million lumens.
int64_t const MAX_RESOURCE_FEE = 1LL << 50;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
// Starting in protocol 23, some operation meta needs to be modified
// to be consumed by downstream systems. In particular, restoration is
// (mostly) logically a new entry creation from the perspective of ltx and
// stellar-core as a whole, but this change type is reclassified to
// LEDGER_ENTRY_RESTORED for easier consumption downstream.
LedgerEntryChanges
processOpLedgerEntryChanges(std::shared_ptr<OperationFrame const> op,
                            AbstractLedgerTxn& ltx)
{
    if (op->getOperation().body.type() != RESTORE_FOOTPRINT)
    {
        return ltx.getChanges();
    }

    auto const& hotArchiveRestores = ltx.getRestoredHotArchiveKeys();
    auto const& liveRestores = ltx.getRestoredLiveBucketListKeys();

    LedgerEntryChanges changes = ltx.getChanges();

    // Depending on whether the restored entry is still in the live
    // BucketList (has not yet been evicted), or has been evicted and is in
    // the hot archive, meta will be handled differently as follows:
    //
    // Entry restore from Hot Archive:
    // Meta before changes:
    //     Data/Code: LEDGER_ENTRY_CREATED
    //     TTL: LEDGER_ENTRY_CREATED
    // Meta after changes:
    //     Data/Code: LEDGER_ENTRY_RESTORED
    //     TTL: LEDGER_ENTRY_RESTORED
    //
    // Entry restore from Live BucketList:
    // Meta before changes:
    //     Data/Code: no meta
    //     TTL: LEDGER_ENTRY_STATE(oldValue), LEDGER_ENTRY_UPDATED(newValue)
    // Meta after changes:
    //     Data/Code: LEDGER_ENTRY_RESTORED
    //     TTL: LEDGER_ENTRY_STATE(oldValue), LEDGER_ENTRY_RESTORED(newValue)
    //
    // First, iterate through existing meta and change everything we need to
    // update.
    for (auto& change : changes)
    {
        // For entry creation meta, we only need to check for Hot Archive
        // restores
        if (change.type() == LEDGER_ENTRY_CREATED)
        {
            auto le = change.created();
            if (hotArchiveRestores.find(LedgerEntryKey(le)) !=
                hotArchiveRestores.end())
            {
                releaseAssertOrThrow(isPersistentEntry(le.data) ||
                                     le.data.type() == TTL);
                change.type(LEDGER_ENTRY_RESTORED);
                change.restored() = le;
            }
        }
        // Update meta only applies to TTL meta
        else if (change.type() == LEDGER_ENTRY_UPDATED)
        {
            if (change.updated().data.type() == TTL)
            {
                auto ttlLe = change.updated();
                if (liveRestores.find(LedgerEntryKey(ttlLe)) !=
                    liveRestores.end())
                {
                    // Update the TTL change from LEDGER_ENTRY_UPDATED to
                    // LEDGER_ENTRY_RESTORED.
                    change.type(LEDGER_ENTRY_RESTORED);
                    change.restored() = ttlLe;
                }
            }
        }
    }

    // Now we need to insert all the LEDGER_ENTRY_RESTORED changes for the
    // data entries that were not created but already existed on the live
    // BucketList. These data/code entries have not been modified (only the TTL
    // is updated), so ltx doesn't have any meta. However this is still useful
    // for downstream so we manually insert restore meta here.
    for (auto const& key : liveRestores)
    {
        if (key.type() == TTL)
        {
            continue;
        }
        releaseAssertOrThrow(isPersistentEntry(key));

        // Note: this is already in the cache since the RestoreOp loaded
        // all data keys for size calculation during apply already
        auto entry = ltx.getNewestVersion(key);

        // If TTL already exists and is just being updated, the
        // data entry must also already exist
        releaseAssertOrThrow(entry);

        LedgerEntryChange change;
        change.type(LEDGER_ENTRY_RESTORED);
        change.restored() = entry->ledgerEntry();
        changes.push_back(change);
    }

    return changes;
}
#endif

} // namespace

using namespace std;
using namespace stellar::txbridge;

TransactionFrame::TransactionFrame(Hash const& networkID,
                                   TransactionEnvelope const& envelope)
    : mEnvelope(envelope), mNetworkID(networkID)
{
    auto const& ops = mEnvelope.type() == ENVELOPE_TYPE_TX_V0
                          ? mEnvelope.v0().tx.operations
                          : mEnvelope.v1().tx.operations;

    for (uint32_t i = 0; i < ops.size(); i++)
    {
        mOperations.push_back(OperationFrame::makeHelper(ops[i], *this, i));
    }
}

Hash const&
TransactionFrame::getFullHash() const
{
    ZoneScoped;
    if (isZero(mFullHash))
    {
        mFullHash = xdrSha256(mEnvelope);
    }
    return (mFullHash);
}

Hash const&
TransactionFrame::getContentsHash() const
{
    ZoneScoped;
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

TransactionEnvelope const&
TransactionFrame::getEnvelope() const
{
    return mEnvelope;
}

#ifdef BUILD_TESTS
TransactionEnvelope&
TransactionFrame::getMutableEnvelope() const
{
    return mEnvelope;
}

void
TransactionFrame::clearCached() const
{
    Hash zero;
    mContentsHash = zero;
    mFullHash = zero;
}
#endif

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

Resource
TransactionFrame::getResources(bool useByteLimitInClassic) const
{
    auto txSize = static_cast<int64_t>(this->getSize());
    if (isSoroban())
    {
        auto r = sorobanResources();
        int64_t const opCount = 1;

        // When doing fee calculation, the rust host will include readWrite
        // entries in the read related fees. However, this resource calculation
        // is used for constructing TX sets before invoking the host, so we need
        // to sum readOnly size and readWrite size for correct resource limits
        // here.
        return Resource({opCount, r.instructions, txSize, r.readBytes,
                         r.writeBytes,
                         static_cast<int64_t>(r.footprint.readOnly.size() +
                                              r.footprint.readWrite.size()),
                         static_cast<int64_t>(r.footprint.readWrite.size())});
    }
    else if (useByteLimitInClassic)
    {
        return Resource({getNumOperations(), txSize});
    }
    else
    {
        return Resource(getNumOperations());
    }
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
TransactionFrame::getInclusionFee() const
{
    if (isSoroban())
    {
        if (declaredSorobanResourceFee() < 0)
        {
            throw std::runtime_error(
                "TransactionFrame::getInclusionFee: negative resource fee");
        }
        return getFullFee() - declaredSorobanResourceFee();
    }

    return getFullFee();
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
        int64_t adjustedFee =
            *baseFee * std::max<int64_t>(1, getNumOperations());
        int64_t maybeResourceFee =
            isSoroban() ? declaredSorobanResourceFee() : 0;

        if (applying)
        {
            return maybeResourceFee +
                   std::min<int64_t>(getInclusionFee(), adjustedFee);
        }
        else
        {
            return maybeResourceFee + adjustedFee;
        }
    }
    else
    {
        return getFullFee();
    }
}

bool
TransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                 LedgerEntryWrapper const& account,
                                 int32_t neededWeight) const
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
                                          AccountID const& accountID) const
{
    ZoneScoped;
    std::vector<Signer> signers;
    auto signerKey = KeyUtils::convertKey<SignerKey>(accountID);
    signers.push_back(Signer(signerKey, 1));
    return signatureChecker.checkSignature(signers, 0);
}

bool
TransactionFrame::checkExtraSigners(SignatureChecker& signatureChecker) const
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
                                    LedgerTxnHeader const& header) const
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
            mCachedAccountPreProtocol8 = newest;
        }
        else
        {
            mCachedAccountPreProtocol8.reset();
        }
    }
    return res;
}

LedgerTxnEntry
TransactionFrame::loadAccount(AbstractLedgerTxn& ltx,
                              LedgerTxnHeader const& header,
                              AccountID const& accountID) const
{
    ZoneScoped;
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_8) &&
        mCachedAccountPreProtocol8 &&
        mCachedAccountPreProtocol8->ledgerEntry().data.account().accountID ==
            accountID)
    {
        // this is buggy caching that existed in old versions of the protocol
        auto res = stellar::loadAccount(ltx, accountID);
        if (res)
        {
            res.currentGeneralized() = *mCachedAccountPreProtocol8;
        }
        else
        {
            res = ltx.create(*mCachedAccountPreProtocol8);
        }

        auto newest = ltx.getNewestVersion(LedgerEntryKey(res.current()));
        mCachedAccountPreProtocol8 = newest;
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

SorobanResources const&
TransactionFrame::sorobanResources() const
{
    releaseAssertOrThrow(isSoroban());
    return mEnvelope.v1().tx.ext.sorobanData().resources;
}

MutableTxResultPtr
TransactionFrame::createSuccessResultWithFeeCharged(
    LedgerHeader const& header, std::optional<int64_t> baseFee,
    bool applying) const
{
    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    auto feeCharged = getFee(header, baseFee, applying);
    return MutableTxResultPtr(new MutableTransactionResult(*this, feeCharged));
}

MutableTxResultPtr
TransactionFrame::createSuccessResult() const
{
    return MutableTxResultPtr(new MutableTransactionResult(*this, 0));
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
TransactionFrame::validateSorobanResources(SorobanNetworkConfig const& config,
                                           Config const& appConfig,
                                           uint32_t protocolVersion,
                                           SorobanTxData& sorobanData) const
{
    auto const& resources = sorobanResources();
    auto const& readEntries = resources.footprint.readOnly;
    auto const& writeEntries = resources.footprint.readWrite;
    if (resources.instructions > config.txMaxInstructions())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "transaction instructions resources exceed network config limit",
            {makeU64SCVal(resources.instructions),
             makeU64SCVal(config.txMaxInstructions())});
        return false;
    }
    if (resources.readBytes > config.txMaxReadBytes())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction byte-read resources exceed network config limit",
            {makeU64SCVal(resources.readBytes),
             makeU64SCVal(config.txMaxReadBytes())});
        return false;
    }
    if (resources.writeBytes > config.txMaxWriteBytes())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction byte-write resources exceed network config limit",
            {makeU64SCVal(resources.writeBytes),
             makeU64SCVal(config.txMaxWriteBytes())});
        return false;
    }
    if (readEntries.size() + writeEntries.size() >
        config.txMaxReadLedgerEntries())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction entry-read resources exceed network config limit",
            {makeU64SCVal(readEntries.size() + writeEntries.size()),
             makeU64SCVal(config.txMaxReadLedgerEntries())});
        return false;
    }
    if (writeEntries.size() > config.txMaxWriteLedgerEntries())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction entry-write resources exceed network config limit",
            {makeU64SCVal(writeEntries.size()),
             makeU64SCVal(config.txMaxWriteLedgerEntries())});
        return false;
    }
    auto footprintKeyIsValid = [&](LedgerKey const& key) -> bool {
        switch (key.type())
        {
        case ACCOUNT:
        case CONTRACT_DATA:
        case CONTRACT_CODE:
            break;
        case TRUSTLINE:
        {
            auto const& tl = key.trustLine();
            if (!isAssetValid(tl.asset, protocolVersion) ||
                (tl.asset.type() == ASSET_TYPE_NATIVE) ||
                isIssuer(tl.accountID, tl.asset))
            {
                sorobanData.pushValidationTimeDiagnosticError(
                    appConfig, SCE_STORAGE, SCEC_INVALID_INPUT,
                    "transaction footprint contains invalid trustline asset");
                return false;
            }
            break;
        }
        case OFFER:
        case DATA:
        case CLAIMABLE_BALANCE:
        case LIQUIDITY_POOL:
        case CONFIG_SETTING:
        case TTL:
            sorobanData.pushValidationTimeDiagnosticError(
                appConfig, SCE_STORAGE, SCEC_UNEXPECTED_TYPE,
                "transaction footprint contains unsupported ledger key type",
                {makeU64SCVal(key.type())});
            return false;
        default:
            throw std::runtime_error("unknown ledger key type");
        }

        if (xdr::xdr_size(key) > config.maxContractDataKeySizeBytes())
        {
            sorobanData.pushValidationTimeDiagnosticError(
                appConfig, SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "transaction footprint key exceeds network config limit",
                {makeU64SCVal(xdr::xdr_size(key)),
                 makeU64SCVal(config.maxContractDataKeySizeBytes())});
            return false;
        }

        return true;
    };
    for (auto const& lk : readEntries)
    {
        if (!footprintKeyIsValid(lk))
        {
            return false;
        }
    }
    for (auto const& lk : writeEntries)
    {
        if (!footprintKeyIsValid(lk))
        {
            return false;
        }
    }
    auto txSize = this->getSize();
    if (txSize > config.txMaxSizeBytes())
    {
        sorobanData.pushSimpleDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "total transaction size exceeds network config limit",
            {makeU64SCVal(txSize), makeU64SCVal(config.txMaxSizeBytes())});
        return false;
    }
    return true;
}

int64_t
TransactionFrame::refundSorobanFee(AbstractLedgerTxn& ltxOuter,
                                   AccountID const& feeSource,
                                   MutableTransactionResultBase& txResult) const
{
    ZoneScoped;
    auto const feeRefund = txResult.getSorobanData()->getSorobanFeeRefund();
    if (feeRefund == 0)
    {
        return 0;
    }

    LedgerTxn ltx(ltxOuter);
    auto header = ltx.loadHeader();
    // The fee source could be from a Fee-bump, so it needs to be forwarded here
    // instead of using TransactionFrame's getFeeSource() method
    auto feeSourceAccount = loadAccount(ltx, header, feeSource);
    if (!feeSourceAccount)
    {
        // Account was merged (shouldn't be possible)
        return 0;
    }

    if (!addBalance(header, feeSourceAccount, feeRefund))
    {
        // Liabilities in the way of the refund, just skip.
        return 0;
    }

    txResult.refundSorobanFee(feeRefund, header.current().ledgerVersion);
    header.current().feePool -= feeRefund;
    ltx.commit();

    return feeRefund;
}

void
TransactionFrame::updateSorobanMetrics(AppConnector& app) const
{
    releaseAssertOrThrow(isSoroban());
    SorobanMetrics& metrics = app.getSorobanMetrics();
    auto txSize = static_cast<int64_t>(this->getSize());
    auto r = sorobanResources();
    // update the tx metrics
    metrics.mTxSizeByte.Update(txSize);
    // accumulate the ledger-wide metrics, which will get emitted at the ledger
    // close
    metrics.accumulateLedgerTxCount(1);
    metrics.accumulateLedgerCpuInsn(r.instructions);
    metrics.accumulateLedgerTxsSizeByte(txSize);
    metrics.accumulateLedgerReadEntry(static_cast<int64_t>(
        r.footprint.readOnly.size() + r.footprint.readWrite.size()));
    metrics.accumulateLedgerReadByte(r.readBytes);
    metrics.accumulateLedgerWriteEntry(
        static_cast<int64_t>(r.footprint.readWrite.size()));
    metrics.accumulateLedgerWriteByte(r.writeBytes);
}

FeePair
TransactionFrame::computeSorobanResourceFee(
    uint32_t protocolVersion, SorobanResources const& txResources,
    uint32_t txSize, uint32_t eventsSize,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg)
{
    ZoneScoped;
    releaseAssertOrThrow(
        protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION));
    CxxTransactionResources cxxResources{};
    cxxResources.instructions = txResources.instructions;

    cxxResources.read_entries =
        static_cast<uint32>(txResources.footprint.readOnly.size());
    cxxResources.write_entries =
        static_cast<uint32>(txResources.footprint.readWrite.size());

    cxxResources.read_bytes = txResources.readBytes;
    cxxResources.write_bytes = txResources.writeBytes;

    cxxResources.transaction_size_bytes = txSize;
    cxxResources.contract_events_size_bytes = eventsSize;

    // This may throw, but only in case of the Core version misconfiguration.
    return rust_bridge::compute_transaction_resource_fee(
        cfg.CURRENT_LEDGER_PROTOCOL_VERSION, protocolVersion, cxxResources,
        sorobanConfig.rustBridgeFeeConfiguration());
}

int64
TransactionFrame::declaredSorobanResourceFee() const
{
    releaseAssertOrThrow(isSoroban());
    return mEnvelope.v1().tx.ext.sorobanData().resourceFee;
}

FeePair
TransactionFrame::computePreApplySorobanResourceFee(
    uint32_t protocolVersion, SorobanNetworkConfig const& sorobanConfig,
    Config const& cfg) const
{
    ZoneScoped;
    releaseAssertOrThrow(isSoroban());
    // We always use the declared resource value for the resource fee
    // computation. The refunds are performed as a separate operation that
    // doesn't involve modifying any transaction fees.
    return computeSorobanResourceFee(
        protocolVersion, sorobanResources(),
        static_cast<uint32>(
            getResources(false).getVal(Resource::Type::TX_BYTE_SIZE)),
        0, sorobanConfig, cfg);
}

bool
TransactionFrame::isTooEarly(LedgerHeaderWrapper const& header,
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
TransactionFrame::isTooLate(LedgerHeaderWrapper const& header,
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
TransactionFrame::isTooEarlyForAccount(LedgerHeaderWrapper const& header,
                                       LedgerEntryWrapper const& sourceAccount,
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
TransactionFrame::commonValidPreSeqNum(
    AppConnector& app, std::optional<SorobanNetworkConfig> const& cfg,
    LedgerSnapshot const& ls, bool chargeFee,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    std::optional<FeePair> sorobanResourceFee,
    MutableTxResultPtr txResult) const
{
    ZoneScoped;
    releaseAssertOrThrow(txResult);
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    uint32_t ledgerVersion = ls.getLedgerHeader().current().ledgerVersion;
    if ((protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13) &&
         (mEnvelope.type() == ENVELOPE_TYPE_TX ||
          hasMuxedAccount(mEnvelope))) ||
        (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_13) &&
         mEnvelope.type() == ENVELOPE_TYPE_TX_V0))
    {
        txResult->setInnermostResultCode(txNOT_SUPPORTED);
        return false;
    }

    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_19) &&
        mEnvelope.type() == ENVELOPE_TYPE_TX &&
        mEnvelope.v1().tx.cond.type() == PRECOND_V2)
    {
        txResult->setInnermostResultCode(txNOT_SUPPORTED);
        return false;
    }

    if (extraSignersExist())
    {
        auto const& extraSigners = mEnvelope.v1().tx.cond.v2().extraSigners;

        static_assert(decltype(PreconditionsV2::extraSigners)::max_size() == 2);
        if (extraSigners.size() == 2 && extraSigners[0] == extraSigners[1])
        {
            txResult->setInnermostResultCode(txMALFORMED);
            return false;
        }

        for (auto const& signer : extraSigners)
        {
            if (signer.type() == SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD &&
                signer.ed25519SignedPayload().payload.empty())
            {
                txResult->setInnermostResultCode(txMALFORMED);
                return false;
            }
        }
    }

    if (getNumOperations() == 0)
    {
        txResult->setInnermostResultCode(txMISSING_OPERATION);
        return false;
    }

    if (!validateSorobanOpsConsistency())
    {
        txResult->setInnermostResultCode(txMALFORMED);
        return false;
    }
    if (isSoroban())
    {
        if (protocolVersionIsBefore(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
        {
            txResult->setInnermostResultCode(txMALFORMED);
            return false;
        }

        releaseAssert(cfg);
        if (!checkSorobanResourceAndSetError(app, cfg.value(), ledgerVersion,
                                             txResult))
        {
            return false;
        }

        auto const& sorobanData = mEnvelope.v1().tx.ext.sorobanData();
        auto& sorobanTxData = *txResult->getSorobanData();
        if (sorobanData.resourceFee > getFullFee())
        {
            sorobanTxData.pushValidationTimeDiagnosticError(
                app.getConfig(), SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "transaction `sorobanData.resourceFee` is higher than the "
                "full transaction fee",
                {makeU64SCVal(sorobanData.resourceFee),
                 makeU64SCVal(getFullFee())});

            txResult->setInnermostResultCode(txSOROBAN_INVALID);
            return false;
        }
        releaseAssertOrThrow(sorobanResourceFee);
        if (sorobanResourceFee->refundable_fee >
            INT64_MAX - sorobanResourceFee->non_refundable_fee)
        {
            sorobanTxData.pushValidationTimeDiagnosticError(
                app.getConfig(), SCE_STORAGE, SCEC_INVALID_INPUT,
                "transaction resource fees cannot be added",
                {makeU64SCVal(sorobanResourceFee->refundable_fee),
                 makeU64SCVal(sorobanResourceFee->non_refundable_fee)});

            txResult->setInnermostResultCode(txSOROBAN_INVALID);
            return false;
        }
        auto const resourceFees = sorobanResourceFee->refundable_fee +
                                  sorobanResourceFee->non_refundable_fee;
        if (sorobanData.resourceFee < resourceFees)
        {
            sorobanTxData.pushValidationTimeDiagnosticError(
                app.getConfig(), SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "transaction `sorobanData.resourceFee` is lower than the "
                "actual Soroban resource fee",
                {makeU64SCVal(sorobanData.resourceFee),
                 makeU64SCVal(resourceFees)});

            txResult->setInnermostResultCode(txSOROBAN_INVALID);
            return false;
        }

        // check for duplicates
        UnorderedSet<LedgerKey> set;
        auto checkDuplicates =
            [&](xdr::xvector<stellar::LedgerKey> const& keys) -> bool {
            for (auto const& lk : keys)
            {
                if (!set.emplace(lk).second)
                {
                    sorobanTxData.pushValidationTimeDiagnosticError(
                        app.getConfig(), SCE_STORAGE, SCEC_INVALID_INPUT,
                        "Found duplicate key in the Soroban footprint; every "
                        "key across read-only and read-write footprints has to "
                        "be unique.",
                        {});

                    txResult->setInnermostResultCode(txSOROBAN_INVALID);
                    return false;
                }
            }
            return true;
        };

        if (!checkDuplicates(sorobanData.resources.footprint.readOnly) ||
            !checkDuplicates(sorobanData.resources.footprint.readWrite))
        {
            return false;
        }
    }
    else
    {
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21))
        {
            if (mEnvelope.type() == ENVELOPE_TYPE_TX &&
                mEnvelope.v1().tx.ext.v() != 0)
            {
                txResult->setInnermostResultCode(txMALFORMED);
                return false;
            }
        }
    }

    auto header = ls.getLedgerHeader();
    if (isTooEarly(header, lowerBoundCloseTimeOffset))
    {
        txResult->setInnermostResultCode(txTOO_EARLY);
        return false;
    }
    if (isTooLate(header, upperBoundCloseTimeOffset))
    {
        txResult->setInnermostResultCode(txTOO_LATE);
        return false;
    }

    if (chargeFee &&
        getInclusionFee() < getMinInclusionFee(*this, header.current()))
    {
        txResult->setInnermostResultCode(txINSUFFICIENT_FEE);
        return false;
    }
    if (!chargeFee && getInclusionFee() < 0)
    {
        txResult->setInnermostResultCode(txINSUFFICIENT_FEE);
        return false;
    }

    if (!ls.getAccount(header, *this))
    {
        txResult->setInnermostResultCode(txNO_ACCOUNT);
        return false;
    }

    return true;
}

void
TransactionFrame::processSeqNum(AbstractLedgerTxn& ltx) const
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
TransactionFrame::processSignatures(
    ValidationType cv, SignatureChecker& signatureChecker,
    AbstractLedgerTxn& ltxOuter, MutableTransactionResultBase& txResult) const
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

    // From protocol 10-13, there's a dangling reference bug where we check op
    // signatures even if no OperationResult object exists. This check ensures
    // opResult actually exists.
    if (auto code = txResult.getInnermostResult().result.code();
        code == txSUCCESS || code == txFAILED)
    {
        // scope here to avoid potential side effects of loading source accounts
        LedgerTxn ltx(ltxOuter);
        LedgerSnapshot ltxState(ltx);
        for (size_t i = 0; i < mOperations.size(); ++i)
        {
            auto const& op = mOperations[i];
            auto& opResult = txResult.getOpResultAt(i);
            if (!op->checkSignature(signatureChecker, ltxState, opResult,
                                    false))
            {
                allOpsValid = false;
            }
        }
    }

    removeOneTimeSignerFromAllSourceAccounts(ltxOuter);

    if (!allOpsValid)
    {
        txResult.setInnermostResultCode(txFAILED);
        return false;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        txResult.setInnermostResultCode(txBAD_AUTH_EXTRA);
        return false;
    }

    return maybeValid;
}

bool
TransactionFrame::isBadSeq(LedgerHeaderWrapper const& header,
                           int64_t seqNum) const
{
    if (getSeqNum() == getStartingSequenceNumber(header.current()))
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
TransactionFrame::commonValid(AppConnector& app,
                              std::optional<SorobanNetworkConfig> const& cfg,
                              SignatureChecker& signatureChecker,
                              LedgerSnapshot const& ls, SequenceNumber current,
                              bool applying, bool chargeFee,
                              uint64_t lowerBoundCloseTimeOffset,
                              uint64_t upperBoundCloseTimeOffset,
                              std::optional<FeePair> sorobanResourceFee,
                              MutableTxResultPtr txResult) const
{
    ZoneScoped;
    releaseAssertOrThrow(txResult);
    ValidationType res = ValidationType::kInvalid;

    auto validate = [this, &signatureChecker, applying,
                     lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset, &app,
                     chargeFee, sorobanResourceFee, txResult, &current, &res,
                     cfg](LedgerSnapshot const& ls) {
        if (applying &&
            (lowerBoundCloseTimeOffset != 0 || upperBoundCloseTimeOffset != 0))
        {
            throw std::logic_error(
                "Applying transaction with non-current closeTime");
        }

        if (!commonValidPreSeqNum(
                app, cfg, ls, chargeFee, lowerBoundCloseTimeOffset,
                upperBoundCloseTimeOffset, sorobanResourceFee, txResult))
        {
            return;
        }

        auto header = ls.getLedgerHeader();
        auto const sourceAccount = ls.getAccount(header, *this);

        // in older versions, the account's sequence number is updated when
        // taking fees
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
                txResult->setInnermostResultCode(txBAD_SEQ);
                return;
            }
        }

        res = ValidationType::kInvalidUpdateSeqNum;

        if (isTooEarlyForAccount(header, sourceAccount,
                                 lowerBoundCloseTimeOffset))
        {
            txResult->setInnermostResultCode(txBAD_MIN_SEQ_AGE_OR_GAP);
            return;
        }

        if (!checkSignature(signatureChecker, sourceAccount,
                            sourceAccount.current()
                                .data.account()
                                .thresholds[THRESHOLD_LOW]))
        {
            txResult->setInnermostResultCode(txBAD_AUTH);
            return;
        }

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_19) &&
            !checkExtraSigners(signatureChecker))
        {
            txResult->setInnermostResultCode(txBAD_AUTH);
            return;
        }

        res = ValidationType::kInvalidPostAuth;

        // if we are in applying mode fee was already deduced from signing
        // account balance, if not, we need to check if after that deduction
        // this account will still have minimum balance
        uint32_t feeToPay = (applying && protocolVersionStartsFrom(
                                             header.current().ledgerVersion,
                                             ProtocolVersion::V_9))
                                ? 0
                                : static_cast<uint32_t>(getFullFee());
        // don't let the account go below the reserve after accounting for
        // liabilities
        if (chargeFee &&
            getAvailableBalance(header.current(), sourceAccount.current()) <
                feeToPay)
        {
            txResult->setInnermostResultCode(txINSUFFICIENT_BALANCE);
            return;
        }

        res = ValidationType::kMaybeValid;
    };

    // Older protocol versions contain buggy account loading code,
    // so preserve nested LedgerTxn to avoid writing to the ledger
    if (protocolVersionIsBefore(ls.getLedgerHeader().current().ledgerVersion,
                                ProtocolVersion::V_8) &&
        applying)
    {
        ls.executeWithMaybeInnerSnapshot(validate);
    }
    else
    {
        // Validate using read-only snapshot
        validate(ls);
    }
    return res;
}

MutableTxResultPtr
TransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                   std::optional<int64_t> baseFee) const
{
    ZoneScoped;
    mCachedAccountPreProtocol8.reset();

    auto header = ltx.loadHeader();
    auto txResult =
        createSuccessResultWithFeeCharged(header.current(), baseFee, true);
    releaseAssert(txResult);

    auto sourceAccount = loadSourceAccount(ltx, header);
    if (!sourceAccount)
    {
        throw std::runtime_error("Unexpected database state");
    }

    auto& acc = sourceAccount.current().data.account();

    int64_t& fee = txResult->getInnermostResult().feeCharged;
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

    return txResult;
}

bool
TransactionFrame::XDRProvidesValidFee() const
{
    if (isSoroban())
    {
        if (mEnvelope.type() != ENVELOPE_TYPE_TX ||
            mEnvelope.v1().tx.ext.v() != 1)
        {
            return false;
        }
        int64_t resourceFee = declaredSorobanResourceFee();
        if (resourceFee < 0 || resourceFee > MAX_RESOURCE_FEE)
        {
            return false;
        }
    }
    return true;
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
    for (auto const& op : mOperations)
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

MutableTxResultPtr
TransactionFrame::checkValidWithOptionallyChargedFee(
    AppConnector& app, LedgerSnapshot const& ls, SequenceNumber current,
    bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset) const
{
    ZoneScoped;
    mCachedAccountPreProtocol8.reset();

    if (!XDRProvidesValidFee())
    {
        auto txResult = createSuccessResult();
        txResult->setInnermostResultCode(txMALFORMED);
        return txResult;
    }

    int64_t minBaseFee = ls.getLedgerHeader().current().baseFee;
    if (!chargeFee)
    {
        minBaseFee = 0;
    }

    auto txResult = createSuccessResultWithFeeCharged(
        ls.getLedgerHeader().current(), minBaseFee, false);
    releaseAssert(txResult);
    SignatureChecker signatureChecker{
        ls.getLedgerHeader().current().ledgerVersion, getContentsHash(),
        getSignatures(mEnvelope)};

    std::optional<FeePair> sorobanResourceFee;
    std::optional<SorobanNetworkConfig> sorobanConfig;
    if (protocolVersionStartsFrom(ls.getLedgerHeader().current().ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION) &&
        isSoroban())
    {
        sorobanConfig =
            app.getLedgerManager().getSorobanNetworkConfigReadOnly();
        sorobanResourceFee = computePreApplySorobanResourceFee(
            ls.getLedgerHeader().current().ledgerVersion, sorobanConfig.value(),
            app.getConfig());
    }
    bool res = commonValid(app, sorobanConfig, signatureChecker, ls, current,
                           false, chargeFee, lowerBoundCloseTimeOffset,
                           upperBoundCloseTimeOffset, sorobanResourceFee,
                           txResult) == ValidationType::kMaybeValid;
    if (res)
    {
        for (size_t i = 0; i < mOperations.size(); ++i)
        {
            auto const& op = mOperations[i];
            auto& opResult = txResult->getOpResultAt(i);

            if (!op->checkValid(app, signatureChecker, sorobanConfig, ls, false,
                                opResult, txResult->getSorobanData()))
            {
                // it's OK to just fast fail here and not try to call
                // checkValid on all operations as the resulting object
                // is only used by applications
                txResult->setInnermostResultCode(txFAILED);
                return txResult;
            }
        }

        if (!signatureChecker.checkAllSignaturesUsed())
        {
            txResult->setInnermostResultCode(txBAD_AUTH_EXTRA);
        }
    }

    return txResult;
}

MutableTxResultPtr
TransactionFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                             SequenceNumber current,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset) const
{
    // Subtle: this check has to happen in `checkValid` and not
    // `checkValidWithOptionallyChargedFee` in order to not validate the
    // envelope XDR twice for the fee bump transactions (they use
    // `checkValidWithOptionallyChargedFee` for the inner tx).
    if (!isTransactionXDRValidForProtocol(
            ls.getLedgerHeader().current().ledgerVersion, app.getConfig(),
            mEnvelope))
    {
        auto txResult = createSuccessResult();
        txResult->setResultCode(txMALFORMED);
        return txResult;
    }
    return checkValidWithOptionallyChargedFee(app, ls, current, true,
                                              lowerBoundCloseTimeOffset,
                                              upperBoundCloseTimeOffset);
}

bool
TransactionFrame::checkSorobanResourceAndSetError(
    AppConnector& app, SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
    MutableTxResultPtr txResult) const
{
    if (!validateSorobanResources(cfg, app.getConfig(), ledgerVersion,
                                  *txResult->getSorobanData()))
    {
        txResult->setInnermostResultCode(txSOROBAN_INVALID);
        return false;
    }
    return true;
}

void
TransactionFrame::insertKeysForFeeProcessing(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(accountKey(getSourceID()));
}

void
TransactionFrame::insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                       LedgerKeyMeter* lkMeter) const
{
    for (auto const& op : mOperations)
    {
        if (!(getSourceID() == op->getSourceID()))
        {
            keys.emplace(accountKey(op->getSourceID()));
        }
        op->insertLedgerKeysToPrefetch(keys);
    }

    if (lkMeter)
    {
        auto const& resources = sorobanResources();
        keys.insert(resources.footprint.readOnly.begin(),
                    resources.footprint.readOnly.end());
        keys.insert(resources.footprint.readWrite.begin(),
                    resources.footprint.readWrite.end());
        auto insertTTLKey = [&](LedgerKey const& lk) {
            if (lk.type() == CONTRACT_DATA || lk.type() == CONTRACT_CODE)
            {
                keys.insert(getTTLKey(lk));
            }
        };
        // TTL keys must be prefetched for soroban entries.
        std::for_each(resources.footprint.readOnly.begin(),
                      resources.footprint.readOnly.end(), insertTTLKey);
        std::for_each(resources.footprint.readWrite.begin(),
                      resources.footprint.readWrite.end(), insertTTLKey);
        lkMeter->addTxn(resources);
    }
}

bool
TransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                        MutableTxResultPtr txResult,
                        Hash const& sorobanBasePrngSeed) const
{
    TransactionMetaFrame tm(ltx.loadHeader().current().ledgerVersion);
    return apply(app, ltx, tm, txResult, sorobanBasePrngSeed);
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaFrame& outerMeta,
                                  MutableTransactionResultBase& txResult,
                                  Hash const& sorobanBasePrngSeed) const
{
    ZoneScoped;
#ifdef BUILD_TESTS
    auto const& result = txResult.getReplayTransactionResult();
    if (result && result->result.code() != txSUCCESS)
    {
        // Sub-zone for skips
        ZoneScopedN("skipped failed");
        CLOG_DEBUG(Tx, "Skipping replay of failed transaction: tx {}",
                   binToHex(getContentsHash()));
        txResult.setResultCode(result->result.code());
        // results field is only active if code is txFAILED or txSUCCESS
        if (result->result.code() == txFAILED)
        {
            txResult.getResult().result.results() = result->result.results();
        }
        return false;
    }
#endif

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

        uint64_t opNum{0};
        for (size_t i = 0; i < mOperations.size(); ++i)
        {
            auto time = opTimer.TimeScope();

            auto const& op = mOperations[i];
            auto& opResult = txResult.getOpResultAt(i);

            LedgerTxn ltxOp(ltxTx);
            Hash subSeed = sorobanBasePrngSeed;
            // If op can use the seed, we need to compute a sub-seed for it.
            if (op->isSoroban())
            {
                SHA256 subSeedSha;
                subSeedSha.add(sorobanBasePrngSeed);
                subSeedSha.add(xdr::xdr_to_opaque(opNum));
                subSeed = subSeedSha.finish();
            }
            ++opNum;

            bool txRes = op->apply(app, signatureChecker, ltxOp, subSeed,
                                   opResult, txResult.getSorobanData());

            if (!txRes)
            {
                success = false;
            }

            // The operation meta will be empty if the transaction
            // doesn't succeed so we may as well not do any work in that
            // case
            if (success)
            {
                app.checkOnOperationApply(op->getOperation(), opResult,
                                          ltxOp.getDelta());

                LedgerEntryChanges changes;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                if (protocolVersionStartsFrom(
                        ledgerVersion,
                        LiveBucket::
                            FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                {
                    changes = processOpLedgerEntryChanges(op, ltxOp);
                }
                else
#endif
                {
                    changes = ltxOp.getChanges();
                }
                operationMetas.emplace_back(changes);
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
                    txResult.setInnermostResultCode(txBAD_AUTH_EXTRA);

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
                txResult.setInnermostResultCode(txBAD_SPONSORSHIP);
                return false;
            }

            ltxTx.commit();
            // commit -> propagate the meta to the outer scope
            outerMeta.pushOperationMetas(std::move(operationMetas));
            outerMeta.pushTxChangesAfter(std::move(changesAfter));

            if (protocolVersionStartsFrom(ledgerVersion,
                                          SOROBAN_PROTOCOL_VERSION) &&
                isSoroban())
            {
                txResult.getSorobanData()->publishSuccessDiagnosticsToMeta(
                    outerMeta, app.getConfig());
            }
        }
        else
        {
            txResult.setInnermostResultCode(txFAILED);
            if (protocolVersionStartsFrom(ledgerVersion,
                                          SOROBAN_PROTOCOL_VERSION) &&
                isSoroban())
            {
                // If transaction fails, we don't charge for any
                // refundable resources.
                auto preApplyFee = computePreApplySorobanResourceFee(
                    ledgerVersion, app.getSorobanNetworkConfigForApply(),
                    app.getConfig());

                txResult.getSorobanData()->setSorobanFeeRefund(
                    declaredSorobanResourceFee() -
                    preApplyFee.non_refundable_fee);

                txResult.getSorobanData()->publishFailureDiagnosticsToMeta(
                    outerMeta, app.getConfig());
            }
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
    txResult.setInnermostResultCode(txINTERNAL_ERROR);

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
TransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                        TransactionMetaFrame& meta, MutableTxResultPtr txResult,
                        bool chargeFee, Hash const& sorobanBasePrngSeed) const
{
    ZoneScoped;
    try
    {
        mCachedAccountPreProtocol8.reset();
        uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
        std::unique_ptr<SignatureChecker> signatureChecker;
#ifdef BUILD_TESTS
        // If the txResult has a replay result (catchup in skip mode is
        // enabled),
        //  we do not perform signature verification.
        if (txResult->getReplayTransactionResult())
        {
            signatureChecker = std::make_unique<AlwaysValidSignatureChecker>(
                ledgerVersion, getContentsHash(), getSignatures(mEnvelope));
        }
        else
        {
#endif // BUILD_TESTS
            signatureChecker = std::make_unique<SignatureChecker>(
                ledgerVersion, getContentsHash(), getSignatures(mEnvelope));
#ifdef BUILD_TESTS
        }
#endif // BUILD_TESTS

        //  when applying, a failure during tx validation means that
        //  we'll skip trying to apply operations but we'll still
        //  process the sequence number if needed
        std::optional<FeePair> sorobanResourceFee;
        std::optional<SorobanNetworkConfig> sorobanConfig;
        if (protocolVersionStartsFrom(ledgerVersion,
                                      SOROBAN_PROTOCOL_VERSION) &&
            isSoroban())
        {
            sorobanConfig = app.getSorobanNetworkConfigForApply();
            sorobanResourceFee = computePreApplySorobanResourceFee(
                ledgerVersion, *sorobanConfig, app.getConfig());

            auto& sorobanData = *txResult->getSorobanData();
            sorobanData.setSorobanConsumedNonRefundableFee(
                sorobanResourceFee->non_refundable_fee);
            sorobanData.setSorobanFeeRefund(
                declaredSorobanResourceFee() -
                sorobanResourceFee->non_refundable_fee);
        }
        LedgerTxn ltxTx(ltx);
        LedgerSnapshot ltxStmt(ltxTx);
        auto cv =
            commonValid(app, sorobanConfig, *signatureChecker, ltxStmt, 0, true,
                        chargeFee, 0, 0, sorobanResourceFee, txResult);
        if (cv >= ValidationType::kInvalidUpdateSeqNum)
        {
            processSeqNum(ltxTx);
        }

        bool signaturesValid =
            processSignatures(cv, *signatureChecker, ltxTx, *txResult);

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
                if (isSoroban())
                {
                    updateSorobanMetrics(app);
                }

                ok = applyOperations(*signatureChecker, app, ltx, meta,
                                     *txResult, sorobanBasePrngSeed);
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
TransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                        TransactionMetaFrame& meta, MutableTxResultPtr txResult,
                        Hash const& sorobanBasePrngSeed) const
{
    return apply(app, ltx, meta, txResult, true, sorobanBasePrngSeed);
}

void
TransactionFrame::processPostApply(AppConnector& app,
                                   AbstractLedgerTxn& ltxOuter,
                                   TransactionMetaFrame& meta,
                                   MutableTxResultPtr txResult) const
{
    releaseAssertOrThrow(txResult);
    processRefund(app, ltxOuter, meta, getSourceID(), *txResult);
}

// This is a TransactionFrame specific function that should only be used by
// FeeBumpTransactionFrame to forward a different account for the refund.
int64_t
TransactionFrame::processRefund(AppConnector& app, AbstractLedgerTxn& ltxOuter,
                                TransactionMetaFrame& meta,
                                AccountID const& feeSource,
                                MutableTransactionResultBase& txResult) const
{
    ZoneScoped;

    if (!isSoroban())
    {
        return 0;
    }
    // Process Soroban resource fee refund (this is independent of the
    // transaction success).
    LedgerTxn ltx(ltxOuter);
    int64_t refund = refundSorobanFee(ltx, feeSource, txResult);
    meta.pushTxChangesAfter(ltx.getChanges());
    ltx.commit();

    return refund;
}

std::shared_ptr<StellarMessage const>
TransactionFrame::toStellarMessage() const
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(TRANSACTION);
    msg->transaction() = mEnvelope;
    return msg;
}

uint32_t
TransactionFrame::getSize() const
{
    ZoneScoped;
    return static_cast<uint32_t>(xdr::xdr_size(mEnvelope));
}
} // namespace stellar
