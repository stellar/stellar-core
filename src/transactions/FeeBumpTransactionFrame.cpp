// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionMeta.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "util/numeric128.h"
#include "xdrpp/marshal.h"

#include <numeric>

namespace stellar
{

TransactionEnvelope
FeeBumpTransactionFrame::convertInnerTxToV1(TransactionEnvelope const& envelope)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX);
    e.v1() = envelope.feeBump().tx.innerTx.v1();
    return e;
}

bool
FeeBumpTransactionFrame::hasDexOperations() const
{
    return mInnerTx->hasDexOperations();
}

bool
FeeBumpTransactionFrame::isSoroban() const
{
    return mInnerTx->isSoroban();
}

SorobanResources const&
FeeBumpTransactionFrame::sorobanResources() const
{
    return mInnerTx->sorobanResources();
}

SorobanTransactionData::_ext_t const&
FeeBumpTransactionFrame::getResourcesExt() const
{
    return mInnerTx->getResourcesExt();
}

FeeBumpTransactionFrame::FeeBumpTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope)
    : mEnvelope(envelope)
    , mInnerTx(std::make_shared<TransactionFrame>(networkID,
                                                  convertInnerTxToV1(envelope)))
    , mNetworkID(networkID)
{
}

#ifdef BUILD_TESTS
FeeBumpTransactionFrame::FeeBumpTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope,
    TransactionFramePtr innerTx)
    : mEnvelope(envelope), mInnerTx(innerTx), mNetworkID(networkID)
{
}
#endif

void
FeeBumpTransactionFrame::preParallelApply(
    AppConnector& app, AbstractLedgerTxn& ltx, TransactionMetaBuilder& meta,
    MutableTransactionResultBase& txResult) const
{
    try
    {
        LedgerTxn ltxTx(ltx);
        removeOneTimeSignerKeyFromFeeSource(ltxTx);
        meta.pushTxChangesBefore(ltxTx);
        ltxTx.commit();
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("Exception in preParallelApply ", e.what());
    }
    catch (...)
    {
        printErrorAndAbort("Unknown exception in preParallelApply");
    }

    try
    {
        mInnerTx->preParallelApply(app, ltx, meta, txResult, false);
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("Exception during preParallelApply: ", e.what());
    }
    catch (...)
    {
        printErrorAndAbort("Unknown exception during preParallelApply");
    }
}

ParallelTxReturnVal
FeeBumpTransactionFrame::parallelApply(
    AppConnector& app, ThreadParallelApplyLedgerState const& threadState,
    Config const& config, SorobanNetworkConfig const& sorobanConfig,
    ParallelLedgerInfo const& ledgerInfo,
    MutableTransactionResultBase& txResult, SorobanMetrics& sorobanMetrics,
    Hash const& txPrngSeed, TxEffects& effects) const
{
    try
    {
        // If this throws, then we may not have the correct TransactionResult so
        // we must crash.
        // Note that even after updateResult is called here, feeCharged will not
        // be accurate for Soroban transactions until
        // FeeBumpTransactionFrame::processPostApply is called.
        auto res = mInnerTx->parallelApply(app, threadState, config,
                                           sorobanConfig, ledgerInfo, txResult,
                                           sorobanMetrics, txPrngSeed, effects);
        return res;
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("Exception while applying inner transaction: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "Unknown exception while applying inner transaction");
    }
}

bool
FeeBumpTransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                               TransactionMetaBuilder& meta,
                               MutableTransactionResultBase& txResult,
                               Hash const& sorobanBasePrngSeed) const
{
    try
    {
        LedgerTxn ltxTx(ltx);
        removeOneTimeSignerKeyFromFeeSource(ltxTx);
        meta.pushTxChangesBefore(ltxTx);
        ltxTx.commit();
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

    try
    {
        // If this throws, then we may not have the correct TransactionResult so
        // we must crash.
        return mInnerTx->apply(app, ltx, meta, txResult, false,
                               sorobanBasePrngSeed);
    }
    catch (std::exception& e)
    {
        printErrorAndAbort("Exception while applying inner transaction: ",
                           e.what());
    }
    catch (...)
    {
        printErrorAndAbort(
            "Unknown exception while applying inner transaction");
    }
}

void
FeeBumpTransactionFrame::processPostApply(
    AppConnector& app, AbstractLedgerTxn& ltx, TransactionMetaBuilder& meta,
    MutableTransactionResultBase& txResult) const
{
    // We must forward the Fee-bump source so the refund is applied to the
    // correct account
    // Note that we are not calling TransactionFrame::processPostApply, so if
    // any logic is added there, we would have to reason through if that logic
    // should also be reflected here.
    if (protocolVersionIsBefore(ltx.loadHeader().current().ledgerVersion,
                                ProtocolVersion::V_23) &&
        isSoroban())
    {
        LedgerTxn ltxInner(ltx);
        mInnerTx->processRefund(app, ltxInner, getFeeSourceID(), txResult,
                                meta.getTxEventManager());
        meta.pushTxChangesAfter(ltxInner);
        ltxInner.commit();
    }
}

void
FeeBumpTransactionFrame::processPostTxSetApply(
    AppConnector& app, AbstractLedgerTxn& ltx,
    MutableTransactionResultBase& txResult,
    TxEventManager& txEventManager) const
{
    mInnerTx->processRefund(app, ltx, getFeeSourceID(), txResult,
                            txEventManager);
}

bool
FeeBumpTransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                        LedgerEntryWrapper const& account,
                                        int32_t neededWeight) const
{
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

MutableTxResultPtr
FeeBumpTransactionFrame::checkValid(
    AppConnector& app, LedgerSnapshot const& ls, SequenceNumber current,
    uint64_t lowerBoundCloseTimeOffset, uint64_t upperBoundCloseTimeOffset,
    DiagnosticEventManager& diagnosticEvents) const
{
    if (!checkVNext(ls.getLedgerHeader().current().ledgerVersion,
                    app.getConfig(), mEnvelope) ||
        !XDRProvidesValidFee())
    {
        return FeeBumpMutableTransactionResult::createTxError(txMALFORMED);
    }

    // Setting the fees in this flow is potentially misleading, as these aren't
    // the fees that would end up being applied. However, this is what Core
    // used to return for a while, and some users may rely on this, so we
    // maintain this logic for the time being.
    int64_t minBaseFee = ls.getLedgerHeader().current().baseFee;
    auto feeCharged = getFee(ls.getLedgerHeader().current(), minBaseFee, false);
    auto txResult = FeeBumpMutableTransactionResult::createSuccess(
        *mInnerTx, feeCharged, 0);

    SignatureChecker signatureChecker{
        ls.getLedgerHeader().current().ledgerVersion, getContentsHash(),
        mEnvelope.feeBump().signatures};
    if (commonValid(signatureChecker, ls, false, *txResult) !=
        ValidationType::kFullyValid)
    {
        return txResult;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        txResult->setError(txBAD_AUTH_EXTRA);
        return txResult;
    }

    mInnerTx->checkValidWithOptionallyChargedFee(
        app, ls, current, false, lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset, *txResult, diagnosticEvents);

    return txResult;
}

bool
FeeBumpTransactionFrame::checkSorobanResources(
    SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
    DiagnosticEventManager& diagnosticEvents) const
{
    return mInnerTx->checkSorobanResources(cfg, ledgerVersion,
                                           diagnosticEvents);
}

std::optional<LedgerEntryWrapper>
FeeBumpTransactionFrame::commonValidPreSeqNum(
    LedgerSnapshot const& ls, MutableTransactionResultBase& txResult) const
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    auto header = ls.getLedgerHeader();
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_13))
    {
        txResult.setError(txNOT_SUPPORTED);
        return std::nullopt;
    }
    auto inclusionFee = getInclusionFee();
    auto minInclusionFee = getMinInclusionFee(*this, header.current());
    if (inclusionFee < minInclusionFee)
    {
        txResult.setError(txINSUFFICIENT_FEE);
        return std::nullopt;
    }
    // While in theory it should be possible to bump a Soroban
    // transaction with negative inclusion fee (this is unavoidable
    // when Soroban resource fee exceeds uint32), we still won't
    // consider the inner transaction valid. So we return early here
    // in order to have `bigMultiply` below not crash.
    if (mInnerTx->getInclusionFee() < 0)
    {
        txResult.setError(txFEE_BUMP_INNER_FAILED);
        return std::nullopt;
    }
    auto const& lh = header.current();
    // Make sure that fee bump is actually happening, i.e. that the
    // inclusion fee per operation in this envelope is higher than
    // the one in the inner envelope.
    uint128_t v1 =
        bigMultiply(getInclusionFee(), getMinInclusionFee(*mInnerTx, lh));
    uint128_t v2 =
        bigMultiply(mInnerTx->getInclusionFee(), getMinInclusionFee(*this, lh));
    if (v1 < v2)
    {
        int64_t feeNeeded = 0;
        if (!bigDivide128(feeNeeded, v2, getMinInclusionFee(*mInnerTx, lh),
                          Rounding::ROUND_UP))
        {
            txResult.setInsufficientFeeErrorWithFeeCharged(
                std::numeric_limits<int64_t>::max());
        }
        else
        {
            txResult.setInsufficientFeeErrorWithFeeCharged(feeNeeded);
        }
        return std::nullopt;
    }

    auto feeSource = ls.getAccount(getFeeSourceID());
    if (!feeSource)
    {
        txResult.setError(txNO_ACCOUNT);
        return std::nullopt;
    }

    return feeSource;
}

FeeBumpTransactionFrame::ValidationType
FeeBumpTransactionFrame::commonValid(
    SignatureChecker& signatureChecker, LedgerSnapshot const& ls, bool applying,
    MutableTransactionResultBase& txResult) const
{
    ValidationType res = ValidationType::kInvalid;

    // Get the fee source account during commonValidPreSeqNum to avoid redundant
    // account loading
    auto feeSource = commonValidPreSeqNum(ls, txResult);
    if (!feeSource)
    {
        return res;
    }

    if (!checkSignature(
            signatureChecker, *feeSource,
            feeSource->current().data.account().thresholds[THRESHOLD_LOW]))
    {
        txResult.setError(txBAD_AUTH);
        return res;
    }

    res = ValidationType::kInvalidPostAuth;

    auto header = ls.getLedgerHeader();
    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    int64_t feeToPay = applying ? 0 : getFullFee();
    // don't let the account go below the reserve after accounting for
    // liabilities
    if (getAvailableBalance(header.current(), feeSource->current()) < feeToPay)
    {
        txResult.setError(txINSUFFICIENT_BALANCE);
        return res;
    }

    return ValidationType::kFullyValid;
}

TransactionEnvelope const&
FeeBumpTransactionFrame::getEnvelope() const
{
    return mEnvelope;
}

#ifdef BUILD_TESTS
TransactionEnvelope&
FeeBumpTransactionFrame::getMutableEnvelope() const
{
    return mEnvelope;
}

void
FeeBumpTransactionFrame::clearCached() const
{
    Hash zero;
    mContentsHash = zero;
    mFullHash = zero;
    mInnerTx->clearCached();
}
#endif

int64_t
FeeBumpTransactionFrame::getFullFee() const
{
    return mEnvelope.feeBump().tx.fee;
}

int64
FeeBumpTransactionFrame::declaredSorobanResourceFee() const
{
    return mInnerTx->declaredSorobanResourceFee();
}

int64_t
FeeBumpTransactionFrame::getInclusionFee() const
{
    if (isSoroban())
    {
        return getFullFee() - declaredSorobanResourceFee();
    }
    return getFullFee();
}

bool
FeeBumpTransactionFrame::XDRProvidesValidFee() const
{
    if (getFullFee() < 0)
    {
        return false;
    }
    return mInnerTx->XDRProvidesValidFee();
}

bool
FeeBumpTransactionFrame::isRestoreFootprintTx() const
{
    return mInnerTx->isRestoreFootprintTx();
}

int64_t
FeeBumpTransactionFrame::getFee(LedgerHeader const& header,
                                std::optional<int64_t> baseFee,
                                bool applying) const
{
    if (!baseFee)
    {
        return getFullFee();
    }
    int64_t flatFee = 0;
    if (mInnerTx->isSoroban())
    {
        flatFee = mInnerTx->declaredSorobanResourceFee();
    }
    int64_t adjustedFee = *baseFee * std::max<int64_t>(1, getNumOperations());
    if (applying)
    {
        return flatFee + std::min<int64_t>(getInclusionFee(), adjustedFee);
    }
    else
    {
        return flatFee + adjustedFee;
    }
}

Hash const&
FeeBumpTransactionFrame::getContentsHash() const
{
    if (isZero(mContentsHash))
    {
        mContentsHash = sha256(xdr::xdr_to_opaque(
            mNetworkID, ENVELOPE_TYPE_TX_FEE_BUMP, mEnvelope.feeBump().tx));
    }
    return mContentsHash;
}

Hash const&
FeeBumpTransactionFrame::getFullHash() const
{
    if (isZero(mFullHash))
    {
        mFullHash = sha256(xdr::xdr_to_opaque(mEnvelope));
    }
    return mFullHash;
}

Hash const&
FeeBumpTransactionFrame::getInnerFullHash() const
{
    return mInnerTx->getFullHash();
}

uint32_t
FeeBumpTransactionFrame::getNumOperations() const
{
    return mInnerTx->getNumOperations() + 1;
}

std::vector<std::shared_ptr<OperationFrame const>> const&
FeeBumpTransactionFrame::getOperationFrames() const
{
    return mInnerTx->getOperationFrames();
}

Resource
FeeBumpTransactionFrame::getResources(bool useByteLimitInClassic,
                                      uint32_t ledgerVersion) const
{
    auto res = mInnerTx->getResources(useByteLimitInClassic, ledgerVersion);
    res.setVal(Resource::Type::OPERATIONS, getNumOperations());
    return res;
}

std::vector<Operation> const&
FeeBumpTransactionFrame::getRawOperations() const
{
    return mInnerTx->getRawOperations();
}

SequenceNumber
FeeBumpTransactionFrame::getSeqNum() const
{
    return mInnerTx->getSeqNum();
}

AccountID
FeeBumpTransactionFrame::getFeeSourceID() const
{
    return toAccountID(mEnvelope.feeBump().tx.feeSource);
}

AccountID
FeeBumpTransactionFrame::getSourceID() const
{
    return mInnerTx->getSourceID();
}

std::optional<SequenceNumber const> const
FeeBumpTransactionFrame::getMinSeqNum() const
{
    return mInnerTx->getMinSeqNum();
}

Duration
FeeBumpTransactionFrame::getMinSeqAge() const
{
    return mInnerTx->getMinSeqAge();
}

uint32
FeeBumpTransactionFrame::getMinSeqLedgerGap() const
{
    return mInnerTx->getMinSeqLedgerGap();
}

void
FeeBumpTransactionFrame::insertKeysForFeeProcessing(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(accountKey(getFeeSourceID()));
    mInnerTx->insertKeysForFeeProcessing(keys);
}

void
FeeBumpTransactionFrame::insertKeysForTxApply(
    UnorderedSet<LedgerKey>& keys) const
{
    mInnerTx->insertKeysForTxApply(keys);
}

MutableTxResultPtr
FeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                          std::optional<int64_t> baseFee) const
{
    auto& header = ltx.loadHeader().current();

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!feeSource)
    {
        throw std::runtime_error("Unexpected database state");
    }
    auto& acc = feeSource.current().data.account();

    auto fee = getFee(header, baseFee, true);
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.feePool += fee;
    }

    int64_t innerFeeCharged = mInnerTx->getFee(header, baseFee, true);
    return FeeBumpMutableTransactionResult::createSuccess(*mInnerTx, fee,
                                                          innerFeeCharged);
}

void
FeeBumpTransactionFrame::removeOneTimeSignerKeyFromFeeSource(
    AbstractLedgerTxn& ltx) const
{
    auto account = stellar::loadAccount(ltx, getFeeSourceID());
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto header = ltx.loadHeader();
    auto signerKey = SignerKeyUtils::preAuthTxKey(*this);
    auto& signers = account.current().data.account().signers;
    auto findRes = findSignerByKey(signers.begin(), signers.end(), signerKey);
    if (findRes.second)
    {
        removeSignerWithPossibleSponsorship(ltx, header, findRes.first,
                                            account);
    }
}

MutableTxResultPtr
FeeBumpTransactionFrame::createTxErrorResult(
    TransactionResultCode txErrorCode) const
{
    return FeeBumpMutableTransactionResult::createTxError(txErrorCode);
}

MutableTxResultPtr
FeeBumpTransactionFrame::createValidationSuccessResult() const
{
    return FeeBumpMutableTransactionResult::createSuccess(*mInnerTx, 0, 0);
}

std::shared_ptr<StellarMessage const>
FeeBumpTransactionFrame::toStellarMessage() const
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(TRANSACTION);
    msg->transaction() = mEnvelope;
    return msg;
}
}
