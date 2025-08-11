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
#include "transactions/EventManager.h"
#include "transactions/LumenEventReconciler.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/ParallelApplyUtils.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionMeta.h"
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
#include <xdrpp/types.h>

namespace stellar
{
namespace
{
// Limit to the maximum resource fee allowed for transaction,
// roughly 112 million lumens.
int64_t const MAX_RESOURCE_FEE = 1LL << 50;

uint32_t
getNumDiskReadEntries(SorobanResources const& resources,
                      SorobanTransactionData::_ext_t const& ext,
                      bool isRestoreFootprintOp)
{
    // All restoreOp entries require disk reads
    if (isRestoreFootprintOp)
    {
        return resources.footprint.readWrite.size();
    }

    // First count classic entry reads
    uint32_t count = 0;
    auto countClassic = [&count](auto const& keys) {
        for (auto const& key : keys)
        {
            if (!isSorobanEntry(key))
            {
                ++count;
            }
        }
    };

    countClassic(resources.footprint.readOnly);
    countClassic(resources.footprint.readWrite);

    // Next, count soroban on-disk entries. Only archived entries are on disk,
    // and they have to be marked in the readWrite footprint, so we can just
    // count the number of marked entries directly.
    if (ext.v() == 1)
    {
        count += ext.resourceExt().archivedSorobanEntries.size();
    }

    return count;
}
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

MuxedAccount
TransactionFrame::getSourceAccount() const
{
    if (mEnvelope.type() == ENVELOPE_TYPE_TX_V0)
    {
        MuxedAccount acc(CryptoKeyType::KEY_TYPE_ED25519);
        acc.ed25519() = mEnvelope.v0().tx.sourceAccountEd25519;
        return acc;
    }
    return mEnvelope.v1().tx.sourceAccount;
}

uint32_t
TransactionFrame::getNumOperations() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0
               ? static_cast<uint32_t>(mEnvelope.v0().tx.operations.size())
               : static_cast<uint32_t>(mEnvelope.v1().tx.operations.size());
}

std::vector<std::shared_ptr<OperationFrame const>> const&
TransactionFrame::getOperationFrames() const
{
    return mOperations;
}

Resource
TransactionFrame::getResources(bool useByteLimitInClassic,
                               uint32_t ledgerVersion) const
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
        int64_t diskReadEntries;
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23))
        {
            diskReadEntries = getNumDiskReadEntries(r, getResourcesExt(),
                                                    isRestoreFootprintTx());
        }
        else
        {
            diskReadEntries =
                r.footprint.readOnly.size() + r.footprint.readWrite.size();
        }

        return Resource({opCount, r.instructions, txSize, r.diskReadBytes,
                         r.writeBytes, diskReadEntries,
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

bool
TransactionFrame::validateSorobanMemoForFlooding() const
{
    if (!isSoroban())
    {
        return true;
    }

    auto const& ops = getRawOperations();
    if (ops.size() != 1)
    {
        return true;
    }

    auto const& op = ops.at(0);
    if (op.body.type() != INVOKE_HOST_FUNCTION)
    {
        return true;
    }

    if (getMemo().type() != MemoType::MEMO_NONE ||
        getSourceAccount().type() == CryptoKeyType::KEY_TYPE_MUXED_ED25519 ||
        (op.sourceAccount &&
         op.sourceAccount->type() == CryptoKeyType::KEY_TYPE_MUXED_ED25519))
    {
        return false;
    }
    return true;
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

SorobanTransactionData::_ext_t const&
TransactionFrame::getResourcesExt() const
{
    releaseAssertOrThrow(isSoroban());
    return mEnvelope.v1().tx.ext.sorobanData().ext;
}

MutableTxResultPtr
TransactionFrame::createTxErrorResult(TransactionResultCode txErrorCode) const
{
    return MutableTransactionResult::createTxError(txErrorCode);
}

MutableTxResultPtr
TransactionFrame::createValidationSuccessResult() const
{
    return MutableTransactionResult::createSuccess(*this, 0);
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

Memo const&
TransactionFrame::getMemo() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0 ? mEnvelope.v0().tx.memo
                                                   : mEnvelope.v1().tx.memo;
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
TransactionFrame::checkSorobanResources(
    SorobanNetworkConfig const& config, uint32_t ledgerVersion,
    DiagnosticEventManager& diagnosticEvents) const
{
    auto const& resources = sorobanResources();
    auto const& readEntries = resources.footprint.readOnly;
    auto const& writeEntries = resources.footprint.readWrite;

    if (resources.instructions > config.txMaxInstructions())
    {
        diagnosticEvents.pushError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "transaction instructions resources exceed network config limit",
            {makeU64SCVal(resources.instructions),
             makeU64SCVal(config.txMaxInstructions())});
        return false;
    }
    if (resources.diskReadBytes > config.txMaxDiskReadBytes())
    {
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction byte-read resources exceed network config limit",
            {makeU64SCVal(resources.diskReadBytes),
             makeU64SCVal(config.txMaxDiskReadBytes())});
        return false;
    }
    if (resources.writeBytes > config.txMaxWriteBytes())
    {
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
            "transaction byte-write resources exceed network config limit",
            {makeU64SCVal(resources.writeBytes),
             makeU64SCVal(config.txMaxWriteBytes())});
        return false;
    }

    uint32_t numDiskReads;
    if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23))
    {
        numDiskReads = getNumDiskReadEntries(resources, getResourcesExt(),
                                             isRestoreFootprintTx());

        auto totalReads = resources.footprint.readOnly.size() +
                          resources.footprint.readWrite.size();
        if (totalReads > config.txMaxFootprintEntries())
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "the number of entries in transaction footprint exceeds the "
                "network config limit",
                {makeU64SCVal(totalReads),
                 makeU64SCVal(config.txMaxFootprintEntries())});
            return false;
        }
    }
    else
    {
        numDiskReads = readEntries.size() + writeEntries.size();
    }

    if (numDiskReads > config.txMaxDiskReadEntries())
    {
        std::string errorMessage;
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23))
        {
            errorMessage = "transaction entry-disk-read resources exceed "
                           "network config limit";
        }
        else
        {
            errorMessage = "transaction entry-read resources exceed network "
                           "config limit";
        }
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_EXCEEDED_LIMIT, errorMessage.c_str(),
            {makeU64SCVal(numDiskReads),
             makeU64SCVal(config.txMaxDiskReadEntries())});
        return false;
    }
    if (writeEntries.size() > config.txMaxWriteLedgerEntries())
    {
        diagnosticEvents.pushError(
            SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
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
            if (!isAssetValid(tl.asset, ledgerVersion) ||
                (tl.asset.type() == ASSET_TYPE_NATIVE) ||
                isIssuer(tl.accountID, tl.asset))
            {
                diagnosticEvents.pushError(
                    SCE_STORAGE, SCEC_INVALID_INPUT,
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
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_UNEXPECTED_TYPE,
                "transaction footprint contains unsupported ledger key type",
                {makeU64SCVal(key.type())});
            return false;
        default:
            throw std::runtime_error("unknown ledger key type");
        }

        if (xdr::xdr_size(key) > config.maxContractDataKeySizeBytes())
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
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
        diagnosticEvents.pushError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "total transaction size exceeds network config limit",
            {makeU64SCVal(txSize), makeU64SCVal(config.txMaxSizeBytes())});
        return false;
    }

    // Check that archived indexes are valid if they are present in the TX
    auto const& resourcesExt = getResourcesExt();
    if (resourcesExt.v() == 1)
    {
        if (protocolVersionIsBefore(ledgerVersion,
                                    AUTO_RESTORE_PROTOCOL_VERSION))
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_UNEXPECTED_TYPE,
                "protocol version does not support SorobanResourcesExtV0");
            return false;
        }

        std::optional<uint32_t> lastValue = std::nullopt;
        auto const& archivedEntryIndexes =
            resourcesExt.resourceExt().archivedSorobanEntries;
        for (auto const index : archivedEntryIndexes)
        {
            // Check that indexes are sorted
            if (lastValue && index <= lastValue)
            {
                diagnosticEvents.pushError(
                    SCE_STORAGE, SCEC_INVALID_INPUT,
                    "archivedSorobanEntries must be sorted in ascending order");
                return false;
            }
            lastValue = index;

            // Check that index is in bounds
            if (index >= resources.footprint.readWrite.size())
            {
                diagnosticEvents.pushError(
                    SCE_STORAGE, SCEC_INVALID_INPUT,
                    "archivedSorobanEntries index is out of bounds",
                    {makeU64SCVal(index),
                     makeU64SCVal(resources.footprint.readWrite.size())});
                return false;
            }

            // Finally, check that the index points to a persistent entry
            auto const& key = resources.footprint.readWrite.at(index);
            if (!isPersistentEntry(key))
            {
                diagnosticEvents.pushError(
                    SCE_STORAGE, SCEC_INVALID_INPUT,
                    "archivedSorobanEntries index points to a non-persistent "
                    "entry",
                    {makeU64SCVal(index), makeU64SCVal(key.type())});
                return false;
            }
        }
    }

    return true;
}
int64_t
TransactionFrame::refundSorobanFee(AbstractLedgerTxn& ltxOuter,
                                   AccountID const& feeSource,
                                   MutableTransactionResultBase& txResult) const
{
    ZoneScoped;
    auto const& refundableFeeTracker = txResult.getRefundableFeeTracker();
    if (!refundableFeeTracker)
    {
        return 0;
    }
    auto const feeRefund = refundableFeeTracker->getFeeRefund();
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
        // Account was merged
        return 0;
    }

    if (!addBalance(header, feeSourceAccount, feeRefund))
    {
        // Liabilities in the way of the refund, just skip.
        return 0;
    }

    txResult.finalizeFeeRefund(header.current().ledgerVersion);
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
    auto const& r = sorobanResources();
    // update the tx metrics
    metrics.mTxSizeByte.Update(txSize);
    // accumulate the ledger-wide metrics, which will get emitted at the ledger
    // close
    metrics.accumulateLedgerTxCount(getNumOperations());
    metrics.accumulateLedgerCpuInsn(r.instructions);
    metrics.accumulateLedgerTxsSizeByte(txSize);
    metrics.accumulateLedgerReadEntry(static_cast<int64_t>(
        r.footprint.readOnly.size() + r.footprint.readWrite.size()));
    metrics.accumulateLedgerReadByte(r.diskReadBytes);
    metrics.accumulateLedgerWriteEntry(
        static_cast<int64_t>(r.footprint.readWrite.size()));
    metrics.accumulateLedgerWriteByte(r.writeBytes);
}

FeePair
TransactionFrame::computeSorobanResourceFee(
    uint32_t protocolVersion, SorobanResources const& txResources,
    uint32_t txSize, uint32_t eventsSize,
    SorobanNetworkConfig const& sorobanConfig, Config const& cfg,
    SorobanTransactionData::_ext_t const& ext, bool isRestoreFootprintOp)
{
    ZoneScoped;
    releaseAssertOrThrow(
        protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION));
    CxxTransactionResources cxxResources{};
    cxxResources.instructions = txResources.instructions;

    if (protocolVersionStartsFrom(protocolVersion, ProtocolVersion::V_23))
    {
        cxxResources.disk_read_entries =
            getNumDiskReadEntries(txResources, ext, isRestoreFootprintOp);
    }
    else
    {
        cxxResources.disk_read_entries =
            static_cast<uint32>(txResources.footprint.readOnly.size());
    }

    cxxResources.write_entries =
        static_cast<uint32>(txResources.footprint.readWrite.size());

    cxxResources.disk_read_bytes = txResources.diskReadBytes;
    cxxResources.write_bytes = txResources.writeBytes;

    cxxResources.transaction_size_bytes = txSize;
    cxxResources.contract_events_size_bytes = eventsSize;

    // This may throw, but only in case of the Core version misconfiguration.
    return rust_bridge::compute_transaction_resource_fee(
        cfg.CURRENT_LEDGER_PROTOCOL_VERSION, protocolVersion, cxxResources,
        sorobanConfig.rustBridgeFeeConfiguration(protocolVersion));
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
        static_cast<uint32>(getResources(false, protocolVersion)
                                .getVal(Resource::Type::TX_BYTE_SIZE)),
        0, sorobanConfig, cfg, getResourcesExt(), isRestoreFootprintTx());
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

std::optional<LedgerEntryWrapper>
TransactionFrame::commonValidPreSeqNum(
    AppConnector& app, std::optional<SorobanNetworkConfig> const& cfg,
    LedgerSnapshot const& ls, bool chargeFee,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    std::optional<FeePair> sorobanResourceFee,
    MutableTransactionResultBase& txResult,
    DiagnosticEventManager& diagnosticEvents) const
{
    ZoneScoped;
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    uint32_t ledgerVersion = ls.getLedgerHeader().current().ledgerVersion;
    if ((protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13) &&
         (mEnvelope.type() == ENVELOPE_TYPE_TX ||
          hasMuxedAccount(mEnvelope))) ||
        (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_13) &&
         mEnvelope.type() == ENVELOPE_TYPE_TX_V0))
    {
        txResult.setInnermostError(txNOT_SUPPORTED);
        return std::nullopt;
    }

    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_19) &&
        mEnvelope.type() == ENVELOPE_TYPE_TX &&
        mEnvelope.v1().tx.cond.type() == PRECOND_V2)
    {
        txResult.setInnermostError(txNOT_SUPPORTED);
        return std::nullopt;
    }

    if (extraSignersExist())
    {
        auto const& extraSigners = mEnvelope.v1().tx.cond.v2().extraSigners;

        static_assert(decltype(PreconditionsV2::extraSigners)::max_size() == 2);
        if (extraSigners.size() == 2 && extraSigners[0] == extraSigners[1])
        {
            txResult.setInnermostError(txMALFORMED);
            return std::nullopt;
        }

        for (auto const& signer : extraSigners)
        {
            if (signer.type() == SIGNER_KEY_TYPE_ED25519_SIGNED_PAYLOAD &&
                signer.ed25519SignedPayload().payload.empty())
            {
                txResult.setInnermostError(txMALFORMED);
                return std::nullopt;
            }
        }
    }

    if (getNumOperations() == 0)
    {
        txResult.setInnermostError(txMISSING_OPERATION);
        return std::nullopt;
    }

    if (!validateSorobanOpsConsistency())
    {
        txResult.setInnermostError(txMALFORMED);
        return std::nullopt;
    }
    if (isSoroban())
    {
        if (protocolVersionIsBefore(ledgerVersion, SOROBAN_PROTOCOL_VERSION))
        {
            txResult.setInnermostError(txMALFORMED);
            return std::nullopt;
        }

        releaseAssert(cfg);
        if (!checkSorobanResources(cfg.value(), ledgerVersion,
                                   diagnosticEvents))
        {
            txResult.setInnermostError(txSOROBAN_INVALID);
            return std::nullopt;
        }

        auto const& sorobanData = mEnvelope.v1().tx.ext.sorobanData();
        bool validateResourceFee = true;
        // Starting from protocol 23 allow fee bump transactions to have an
        // inner transaction with insufficient full fee.
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_23))
        {
            validateResourceFee = chargeFee;
        }
        if (validateResourceFee && sorobanData.resourceFee > getFullFee())
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "transaction `sorobanData.resourceFee` is higher than the "
                "full transaction fee",
                {makeU64SCVal(sorobanData.resourceFee),
                 makeU64SCVal(getFullFee())});

            txResult.setInnermostError(txSOROBAN_INVALID);
            return std::nullopt;
        }
        releaseAssertOrThrow(sorobanResourceFee);
        if (sorobanResourceFee->refundable_fee >
            INT64_MAX - sorobanResourceFee->non_refundable_fee)
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_INVALID_INPUT,
                "transaction resource fees cannot be added",
                {makeU64SCVal(sorobanResourceFee->refundable_fee),
                 makeU64SCVal(sorobanResourceFee->non_refundable_fee)});

            txResult.setInnermostError(txSOROBAN_INVALID);
            return std::nullopt;
        }
        auto const resourceFees = sorobanResourceFee->refundable_fee +
                                  sorobanResourceFee->non_refundable_fee;
        if (sorobanData.resourceFee < resourceFees)
        {
            diagnosticEvents.pushError(
                SCE_STORAGE, SCEC_EXCEEDED_LIMIT,
                "transaction `sorobanData.resourceFee` is lower than the "
                "actual Soroban resource fee",
                {makeU64SCVal(sorobanData.resourceFee),
                 makeU64SCVal(resourceFees)});

            txResult.setInnermostError(txSOROBAN_INVALID);
            return std::nullopt;
        }

        // Check for duplicate entries both within each key-set of the footprint
        // and between both key-sets. Note this also ensures that the RW and RO
        // footprints are disjoint.
        UnorderedSet<LedgerKey> set;
        auto checkDuplicates =
            [&](xdr::xvector<stellar::LedgerKey> const& keys) -> bool {
            for (auto const& lk : keys)
            {
                if (!set.emplace(lk).second)
                {
                    diagnosticEvents.pushError(
                        SCE_STORAGE, SCEC_INVALID_INPUT,
                        "Found duplicate key in the Soroban footprint; every "
                        "key across read-only and read-write footprints has to "
                        "be unique.",
                        {});

                    txResult.setInnermostError(txSOROBAN_INVALID);
                    return false;
                }
            }
            return true;
        };

        if (!checkDuplicates(sorobanData.resources.footprint.readOnly) ||
            !checkDuplicates(sorobanData.resources.footprint.readWrite))
        {
            return std::nullopt;
        }
    }
    else
    {
        if (protocolVersionStartsFrom(ledgerVersion, ProtocolVersion::V_21))
        {
#ifndef BUILD_TESTS
            if (mEnvelope.type() == ENVELOPE_TYPE_TX &&
                mEnvelope.v1().tx.ext.v() != 0)
            {
                txResult.setInnermostError(txMALFORMED);
                return std::nullopt;
            }
#endif
        }
    }

    auto header = ls.getLedgerHeader();
    if (isTooEarly(header, lowerBoundCloseTimeOffset))
    {
        txResult.setInnermostError(txTOO_EARLY);
        return std::nullopt;
    }
    if (isTooLate(header, upperBoundCloseTimeOffset))
    {
        txResult.setInnermostError(txTOO_LATE);
        return std::nullopt;
    }

    if (chargeFee &&
        getInclusionFee() < getMinInclusionFee(*this, header.current()))
    {
        txResult.setInnermostError(txINSUFFICIENT_FEE);
        return std::nullopt;
    }
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_23) &&
        !chargeFee && getInclusionFee() < 0)
    {
        txResult.setInnermostError(txINSUFFICIENT_FEE);
        return std::nullopt;
    }

    auto sourceAccount = ls.getAccount(header, *this);
    if (!sourceAccount)
    {
        txResult.setInnermostError(txNO_ACCOUNT);
        return std::nullopt;
    }

    return sourceAccount;
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
    if (auto code = txResult.getInnermostResultCode();
        code == txSUCCESS || code == txFAILED)
    {
        LedgerSnapshot ls(ltxOuter);
        for (size_t i = 0; i < mOperations.size(); ++i)
        {
            auto const& op = mOperations[i];
            auto& opResult = txResult.getOpResultAt(i);
            if (!op->checkSignature(signatureChecker, ls, opResult, false))
            {
                allOpsValid = false;
            }
        }
    }

    removeOneTimeSignerFromAllSourceAccounts(ltxOuter);

    if (!allOpsValid)
    {
        txResult.setInnermostError(txFAILED);
        return false;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        txResult.setInnermostError(txBAD_AUTH_EXTRA);
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
                              MutableTransactionResultBase& txResult,
                              DiagnosticEventManager& diagnosticEvents) const
{
    ZoneScoped;
    ValidationType res = ValidationType::kInvalid;

    auto validate = [this, &signatureChecker, applying,
                     lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset, &app,
                     chargeFee, sorobanResourceFee, &txResult,
                     &diagnosticEvents, &current, &res,
                     &cfg](LedgerSnapshot const& ls) {
        if (applying &&
            (lowerBoundCloseTimeOffset != 0 || upperBoundCloseTimeOffset != 0))
        {
            throw std::logic_error(
                "Applying transaction with non-current closeTime");
        }

        // Get the source account during commonValidPreSeqNum to avoid redundant
        // account loading
        auto sourceAccount = commonValidPreSeqNum(
            app, cfg, ls, chargeFee, lowerBoundCloseTimeOffset,
            upperBoundCloseTimeOffset, sorobanResourceFee, txResult,
            diagnosticEvents);

        if (!sourceAccount)
        {
            return;
        }

        auto header = ls.getLedgerHeader();

        // in older versions, the account's sequence number is updated when
        // taking fees
        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_10) ||
            !applying)
        {
            if (current == 0)
            {
                current = sourceAccount->current().data.account().seqNum;
            }
            if (isBadSeq(header, current))
            {
                txResult.setInnermostError(txBAD_SEQ);
                return;
            }
        }

        res = ValidationType::kInvalidUpdateSeqNum;

        if (isTooEarlyForAccount(header, *sourceAccount,
                                 lowerBoundCloseTimeOffset))
        {
            txResult.setInnermostError(txBAD_MIN_SEQ_AGE_OR_GAP);
            return;
        }

        if (!checkSignature(signatureChecker, *sourceAccount,
                            sourceAccount->current()
                                .data.account()
                                .thresholds[THRESHOLD_LOW]))
        {
            txResult.setInnermostError(txBAD_AUTH);
            return;
        }

        if (protocolVersionStartsFrom(header.current().ledgerVersion,
                                      ProtocolVersion::V_19) &&
            !checkExtraSigners(signatureChecker))
        {
            txResult.setInnermostError(txBAD_AUTH);
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
            getAvailableBalance(header.current(), sourceAccount->current()) <
                feeToPay)
        {
            txResult.setInnermostError(txINSUFFICIENT_BALANCE);
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

    auto sourceAccount = loadSourceAccount(ltx, header);
    if (!sourceAccount)
    {
        throw std::runtime_error("Unexpected database state");
    }

    auto& acc = sourceAccount.current().data.account();

    int64_t fee = getFee(header.current(), baseFee, true);

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
    return MutableTransactionResult::createSuccess(*this, fee);
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

bool
TransactionFrame::isRestoreFootprintTx() const
{
    return isSoroban() &&
           mOperations.front()->getOperation().body.type() == RESTORE_FOOTPRINT;
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

void
TransactionFrame::checkValidWithOptionallyChargedFee(
    AppConnector& app, LedgerSnapshot const& ls, SequenceNumber current,
    bool chargeFee, uint64_t lowerBoundCloseTimeOffset,
    uint64_t upperBoundCloseTimeOffset, MutableTransactionResultBase& txResult,
    DiagnosticEventManager& diagnosticEvents) const
{
    ZoneScoped;
    mCachedAccountPreProtocol8.reset();

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
            app.getLedgerManager().getLastClosedSorobanNetworkConfig();
        sorobanResourceFee = computePreApplySorobanResourceFee(
            ls.getLedgerHeader().current().ledgerVersion, sorobanConfig.value(),
            app.getConfig());
    }
    if (commonValid(app, sorobanConfig, signatureChecker, ls, current, false,
                    chargeFee, lowerBoundCloseTimeOffset,
                    upperBoundCloseTimeOffset, sorobanResourceFee, txResult,
                    diagnosticEvents) != ValidationType::kMaybeValid)
    {
        return;
    }

    for (size_t i = 0; i < mOperations.size(); ++i)
    {
        auto const& op = mOperations[i];
        auto& opResult = txResult.getOpResultAt(i);

        if (!op->checkValid(app, signatureChecker, sorobanConfig, ls, false,
                            opResult, diagnosticEvents))
        {
            // it's OK to just fast fail here and not try to call
            // checkValid on all operations as the resulting object
            // is only used by applications
            txResult.setInnermostError(txFAILED);
            return;
        }
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        txResult.setInnermostError(txBAD_AUTH_EXTRA);
    }
}

MutableTxResultPtr
TransactionFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                             SequenceNumber current,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset,
                             DiagnosticEventManager& diagnosticEvents) const
{
#ifdef BUILD_TESTS
    if (app.getRunInOverlayOnlyMode())
    {
        return MutableTransactionResult::createSuccess(*this, 0);
    }
#endif

    // Subtle: this check has to happen in `checkValid` and not
    // `checkValidWithOptionallyChargedFee` in order to not validate the
    // envelope XDR twice for the fee bump transactions (they use
    // `checkValidWithOptionallyChargedFee` for the inner tx).
    if (!checkVNext(ls.getLedgerHeader().current().ledgerVersion,
                    app.getConfig(), mEnvelope))
    {
        return MutableTransactionResult::createTxError(txMALFORMED);
    }
    // Perform basic XDR fee validation, as
    // `checkValidWithOptionallyChargedFee` expects proper fee-related XDR.
    if (!XDRProvidesValidFee())
    {
        return MutableTransactionResult::createTxError(txMALFORMED);
    }
    // Setting the fees in this flow is potentially misleading, as these aren't
    // the fees that would end up being applied. However, this is what Core
    // used to return for a while, and some users may rely on this, so we
    // maintain this logic for the time being.
    int64_t minBaseFee = ls.getLedgerHeader().current().baseFee;
    auto feeCharged = getFee(ls.getLedgerHeader().current(), minBaseFee, false);
    auto txResult = MutableTransactionResult::createSuccess(*this, feeCharged);
    checkValidWithOptionallyChargedFee(
        app, ls, current, true, lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset, *txResult, diagnosticEvents);
    return txResult;
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

#ifdef BUILD_TESTS
bool
TransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                        MutableTransactionResultBase& txResult,
                        Hash const& sorobanBasePrngSeed) const
{
    TransactionMetaBuilder tm(true, *this,
                              ltx.loadHeader().current().ledgerVersion, app);
    return apply(app, ltx, tm, txResult, sorobanBasePrngSeed);
}
#endif

#ifdef BUILD_TESTS
void
maybeTriggerTestInternalError(TransactionEnvelope const& env)
{
    auto memo =
        env.type() == ENVELOPE_TYPE_TX_V0 ? env.v0().tx.memo : env.v1().tx.memo;
    if (memo.type() == MEMO_TEXT && memo.text() == "txINTERNAL_ERROR")
    {
        throw std::runtime_error(
            "Intentionally triggered INTERNAL_ERROR in test");
    }
}
#endif

std::unique_ptr<SignatureChecker>
TransactionFrame::commonPreApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                 TransactionMetaBuilder& meta,
                                 MutableTransactionResultBase& txResult,
                                 bool chargeFee) const
{
    mCachedAccountPreProtocol8.reset();
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    std::unique_ptr<SignatureChecker> signatureChecker;
#ifdef BUILD_TESTS
    // If the txResult has a replay result (catchup in skip mode is
    // enabled),
    //  we do not perform signature verification.
    if (txResult.hasReplayTransactionResult())
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
    if (protocolVersionStartsFrom(ledgerVersion, SOROBAN_PROTOCOL_VERSION) &&
        isSoroban())
    {
        sorobanConfig = app.getSorobanNetworkConfigForApply();
        sorobanResourceFee = computePreApplySorobanResourceFee(
            ledgerVersion, *sorobanConfig, app.getConfig());

        meta.setNonRefundableResourceFee(
            sorobanResourceFee->non_refundable_fee);
        int64_t initialFeeRefund = declaredSorobanResourceFee() -
                                   sorobanResourceFee->non_refundable_fee;
        txResult.initializeRefundableFeeTracker(initialFeeRefund);
    }
    LedgerTxn ltxTx(ltx);
    LedgerSnapshot lsTx(ltxTx);
    auto cv = commonValid(app, sorobanConfig, *signatureChecker, lsTx, 0, true,
                          chargeFee, 0, 0, sorobanResourceFee, txResult,
                          meta.getDiagnosticEventManager());
    if (cv >= ValidationType::kInvalidUpdateSeqNum)
    {
        processSeqNum(ltxTx);
    }

    bool signaturesValid =
        processSignatures(cv, *signatureChecker, ltxTx, txResult);

    meta.pushTxChangesBefore(ltxTx);
    ltxTx.commit();

    if (signaturesValid && cv == ValidationType::kMaybeValid)
    {
        return signatureChecker;
    }
    else
    {
        return nullptr;
    }
}

void
TransactionFrame::preParallelApply(
    AppConnector& app, AbstractLedgerTxn& ltx, TransactionMetaBuilder& meta,
    MutableTransactionResultBase& resPayload) const
{
    preParallelApply(app, ltx, meta, resPayload, true);
}

void
TransactionFrame::preParallelApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                   TransactionMetaBuilder& meta,
                                   MutableTransactionResultBase& txResult,
                                   bool chargeFee) const
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  app.threadIsType(Application::ThreadType::APPLY));
    try
    {
        releaseAssertOrThrow(isSoroban());

        auto signatureChecker =
            commonPreApply(app, ltx, meta, txResult, chargeFee);
        bool ok = signatureChecker != nullptr;
        if (ok)
        {
            updateSorobanMetrics(app);

            auto& opResult = txResult.getOpResultAt(0);

            // Pre parallel soroban, OperationFrame::checkValid is called right
            // before OperationFrame::doApply, but we do it here instead to
            // avoid making OperationFrame::checkValid thread safe.
            auto const& cfg = app.getSorobanNetworkConfigForApply();
            ok = mOperations.front()->checkValid(
                app, *signatureChecker, cfg, ltx, true, opResult,
                meta.getDiagnosticEventManager());
            if (!ok)
            {
                txResult.setInnermostError(txFAILED);
            }
        }

        // If validation fails, we check the result code in the parallel step to
        // make sure we don't apply the transaction.
        releaseAssertOrThrow(ok == txResult.isSuccess());
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

ParallelTxReturnVal
TransactionFrame::parallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& config, SorobanNetworkConfig const& sorobanConfig,
    ParallelLedgerInfo const& ledgerInfo,
    MutableTransactionResultBase& txResult, SorobanMetrics& sorobanMetrics,
    Hash const& txPrngSeed, TxEffects& effects) const
{
    ZoneScoped;
    // This tx failed validation earlier, do not apply it
    if (!txResult.isSuccess())
    {
        return {false, {}};
    }

    if (!maybeAdoptFailedReplayResult(txResult))
    {
        return {false, {}};
    }

    auto& internalErrorCounter = app.getMetrics().NewCounter(
        {"ledger", "transaction", "internal-error"});
    bool reportInternalErrOnException = true;
    try
    {
        auto liveSnapshot = app.copySearchableLiveBucketListSnapshot();
        // We do not want to increase the internal-error metric count for
        // older ledger versions. The minimum ledger version for which we
        // start internal-error counting is defined in the app config.
        reportInternalErrOnException =
            ledgerInfo.getLedgerVersion() >=
            config.LEDGER_PROTOCOL_MIN_VERSION_INTERNAL_ERROR_REPORT;

        auto opTimer = app.getMetrics()
                           .NewTimer({"ledger", "operation", "apply"})
                           .TimeScope();

        releaseAssertOrThrow(mOperations.size() == 1);

        auto op = mOperations.front();
        auto& opResult = txResult.getOpResultAt(0);
        auto& opMeta = effects.getMeta().getOperationMetaBuilderAt(0);

        auto res = op->parallelApply(
            app, threadState, config, sorobanConfig, ledgerInfo, sorobanMetrics,
            opResult, txResult.getRefundableFeeTracker(), opMeta, txPrngSeed);

#ifdef BUILD_TESTS
        maybeTriggerTestInternalError(mEnvelope);
#endif

        if (res.getSuccess())
        {
            threadState.setEffectsDeltaFromSuccessfulOp(res, ledgerInfo,
                                                        effects);
            opMeta.setLedgerChangesFromSuccessfulOp(threadState, res,
                                                    ledgerInfo.getLedgerSeq());
        }
        else
        {
            txResult.setInnermostError(txFAILED);
        }

        return res;
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
    if (config.HALT_ON_INTERNAL_TRANSACTION_ERROR)
    {
        printErrorAndAbort("Encountered an exception while applying "
                           "operations, see logs for details.");
    }

    // This is only reachable if an exception is thrown
    txResult.setInnermostError(txINTERNAL_ERROR);

    // We only increase the internal-error metric count if the
    // ledger is a newer version.
    if (reportInternalErrOnException)
    {
        internalErrorCounter.inc();
    }
    return {false, {}};
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  AppConnector& app, AbstractLedgerTxn& ltx,
                                  TransactionMetaBuilder& outerMeta,
                                  MutableTransactionResultBase& txResult,
                                  Hash const& sorobanBasePrngSeed) const
{
    ZoneScoped;
    if (!maybeAdoptFailedReplayResult(txResult))
    {
        return false;
    }

    auto& internalErrorCounter = app.getMetrics().NewCounter(
        {"ledger", "transaction", "internal-error"});
    bool reportInternalErrOnException = true;
    try
    {
        bool success = true;
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
                subSeed = subSha256(sorobanBasePrngSeed, opNum);
            }
            ++opNum;
            auto& opMeta = outerMeta.getOperationMetaBuilderAt(i);
            bool txRes =
                op->apply(app, signatureChecker, ltxOp, subSeed, opResult,
                          txResult.getRefundableFeeTracker(), opMeta);
#ifdef BUILD_TESTS
            maybeTriggerTestInternalError(mEnvelope);
#endif

            if (!txRes)
            {
                success = false;
            }

            // The operation meta will be empty if the transaction
            // doesn't succeed so we may as well not do any work in that
            // case
            if (success)
            {
                auto ledgerSeq = ltxOp.loadHeader().current().ledgerSeq;
                auto delta = ltxOp.getDelta();
                auto& opEventManager = opMeta.getEventManager();
                if (protocolVersionIsBefore(ledgerVersion,
                                            ProtocolVersion::V_8) &&
                    opEventManager.isEnabled() &&
                    opResult.tr().type() != INFLATION)
                {
                    reconcileEvents(getSourceID(), op->getOperation(), delta,
                                    opMeta.getEventManager());
                }

                app.checkOnOperationApply(op->getOperation(), opResult, delta,
                                          opEventManager.getEvents());
                opMeta.setLedgerChanges(ltxOp, ledgerSeq);
            }

            if (txRes ||
                protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_14))
            {
                ltxOp.commit();
            }
        }

        if (success)
        {
            if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_10))
            {
                if (!signatureChecker.checkAllSignaturesUsed())
                {
                    txResult.setInnermostError(txBAD_AUTH_EXTRA);

                    // this should never happen: malformed transaction
                    // should not be accepted by nodes
                    return false;
                }

                // if an error occurred, it is responsibility of account's
                // owner to remove that signer
                LedgerTxn ltxAfter(ltxTx);
                removeOneTimeSignerFromAllSourceAccounts(ltxAfter);
                outerMeta.pushTxChangesAfter(ltxAfter);
                ltxAfter.commit();
            }
            else if (protocolVersionStartsFrom(ledgerVersion,
                                               ProtocolVersion::V_14) &&
                     ltxTx.hasSponsorshipEntry())
            {
                txResult.setInnermostError(txBAD_SPONSORSHIP);
                return false;
            }

            ltxTx.commit();
        }
        else
        {
            txResult.setInnermostError(txFAILED);
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
    txResult.setInnermostError(txINTERNAL_ERROR);

    // We only increase the internal-error metric count if the ledger is a
    // newer version.
    if (reportInternalErrOnException)
    {
        internalErrorCounter.inc();
    }
    return false;
}

bool
TransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                        TransactionMetaBuilder& meta,
                        MutableTransactionResultBase& txResult, bool chargeFee,
                        Hash const& sorobanBasePrngSeed) const
{
    ZoneScoped;
    try
    {
        auto signatureChecker =
            commonPreApply(app, ltx, meta, txResult, chargeFee);
        bool ok = signatureChecker != nullptr;
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
                                     txResult, sorobanBasePrngSeed);
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
                        TransactionMetaBuilder& meta,
                        MutableTransactionResultBase& txResult,
                        Hash const& sorobanBasePrngSeed) const
{
    return apply(app, ltx, meta, txResult, true, sorobanBasePrngSeed);
}

void
TransactionFrame::processPostApply(AppConnector& app,
                                   AbstractLedgerTxn& ltxOuter,
                                   TransactionMetaBuilder& meta,
                                   MutableTransactionResultBase& txResult) const
{
    if (protocolVersionIsBefore(ltxOuter.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_23) &&
        isSoroban())
    {
        LedgerTxn ltx(ltxOuter);
        processRefund(app, ltx, getSourceID(), txResult,
                      meta.getTxEventManager());
        meta.pushTxChangesAfter(ltx);
        ltx.commit();
    }
}

void
TransactionFrame::processPostTxSetApply(AppConnector& app,
                                        AbstractLedgerTxn& ltx,
                                        MutableTransactionResultBase& txResult,
                                        TxEventManager& txEventManager) const
{
    processRefund(app, ltx, getSourceID(), txResult, txEventManager);
}

// This is a TransactionFrame specific function that should only be used by
// FeeBumpTransactionFrame to forward a different account for the refund.
void
TransactionFrame::processRefund(AppConnector& app, AbstractLedgerTxn& ltxOuter,
                                AccountID const& feeSource,
                                MutableTransactionResultBase& txResult,
                                TxEventManager& txEventManager) const
{
    ZoneScoped;
    if (!isSoroban())
    {
        return;
    }
    // Process Soroban resource fee refund (this is independent of the
    // transaction success).
    int64_t refund = refundSorobanFee(ltxOuter, feeSource, txResult);

    // Emit fee refund event. A refund counts as a negative amount of fee
    // charged.
    auto stage = TransactionEventStage::TRANSACTION_EVENT_STAGE_AFTER_TX;
    if (protocolVersionStartsFrom(ltxOuter.loadHeader().current().ledgerVersion,
                                  ProtocolVersion::V_23))
    {
        stage = TransactionEventStage::TRANSACTION_EVENT_STAGE_AFTER_ALL_TXS;
    }
    txEventManager.newFeeEvent(feeSource, -refund, stage);
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

bool
TransactionFrame::maybeAdoptFailedReplayResult(
    MutableTransactionResultBase& txResult) const
{
#ifdef BUILD_TESTS
    if (txResult.adoptFailedReplayResult())
    {
        // Sub-zone for skips
        ZoneScopedN("skipped failed");
        CLOG_DEBUG(Tx, "Skipping replay of failed transaction: tx {}",
                   binToHex(getContentsHash()));
        return false;
    }
#endif
    return true;
}

} // namespace stellar
