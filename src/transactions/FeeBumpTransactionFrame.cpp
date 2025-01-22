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
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
#include "transactions/TransactionMetaFrame.h"
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

bool
FeeBumpTransactionFrame::apply(AppConnector& app, AbstractLedgerTxn& ltx,
                               TransactionMetaFrame& meta,
                               MutableTxResultPtr txResult,
                               Hash const& sorobanBasePrngSeed) const
{
    try
    {
        LedgerTxn ltxTx(ltx);
        removeOneTimeSignerKeyFromFeeSource(ltxTx);
        meta.pushTxChangesBefore(ltxTx.getChanges());
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
        // Note that even after updateResult is called here, feeCharged will not
        // be accurate for Soroban transactions until
        // FeeBumpTransactionFrame::processPostApply is called.
        bool res = mInnerTx->apply(app, ltx, meta, txResult, false,
                                   sorobanBasePrngSeed);
        FeeBumpMutableTransactionResult::updateResult(mInnerTx, *txResult);
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

void
FeeBumpTransactionFrame::processPostApply(AppConnector& app,
                                          AbstractLedgerTxn& ltx,
                                          TransactionMetaFrame& meta,
                                          MutableTxResultPtr txResult) const
{
    // We must forward the Fee-bump source so the refund is applied to the
    // correct account
    // Note that we are not calling TransactionFrame::processPostApply, so if
    // any logic is added there, we would have to reason through if that logic
    // should also be reflected here.
    mInnerTx->processRefund(app, ltx, meta, getFeeSourceID(), *txResult);
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
FeeBumpTransactionFrame::checkValid(AppConnector& app, LedgerSnapshot const& ls,
                                    SequenceNumber current,
                                    uint64_t lowerBoundCloseTimeOffset,
                                    uint64_t upperBoundCloseTimeOffset) const
{
    if (!isTransactionXDRValidForProtocol(
            ls.getLedgerHeader().current().ledgerVersion, app.getConfig(),
            mEnvelope) ||
        !XDRProvidesValidFee())
    {
        auto txResult = createSuccessResult();
        txResult->setResultCode(txMALFORMED);
        return txResult;
    }

    int64_t minBaseFee = ls.getLedgerHeader().current().baseFee;
    auto txResult = createSuccessResultWithFeeCharged(
        ls.getLedgerHeader().current(), minBaseFee, false);

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
        txResult->setResultCode(txBAD_AUTH_EXTRA);
        return txResult;
    }

    auto innerTxResult = mInnerTx->checkValidWithOptionallyChargedFee(
        app, ls, current, false, lowerBoundCloseTimeOffset,
        upperBoundCloseTimeOffset);
    auto finalTxResult = createSuccessResultWithNewInnerTx(
        std::move(txResult), std::move(innerTxResult), mInnerTx);

    return finalTxResult;
}

bool
FeeBumpTransactionFrame::checkSorobanResourceAndSetError(
    AppConnector& app, SorobanNetworkConfig const& cfg, uint32_t ledgerVersion,
    MutableTxResultPtr txResult) const
{
    return mInnerTx->checkSorobanResourceAndSetError(app, cfg, ledgerVersion,
                                                     txResult);
}

bool
FeeBumpTransactionFrame::commonValidPreSeqNum(
    LedgerSnapshot const& ls, MutableTransactionResultBase& txResult) const
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    auto header = ls.getLedgerHeader();
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_13))
    {
        txResult.setResultCode(txNOT_SUPPORTED);
        return false;
    }
    auto inclusionFee = getInclusionFee();
    auto minInclusionFee = getMinInclusionFee(*this, header.current());
    if (inclusionFee < minInclusionFee)
    {
        txResult.setResultCode(txINSUFFICIENT_FEE);
        return false;
    }
    // While in theory it should be possible to bump a Soroban
    // transaction with negative inclusion fee (this is unavoidable
    // when Soroban resource fee exceeds uint32), we still won't
    // consider the inner transaction valid. So we return early here
    // in order to have `bigMultiply` below not crash.
    if (mInnerTx->getInclusionFee() < 0)
    {
        txResult.setResultCode(txFEE_BUMP_INNER_FAILED);
        return false;
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
        if (!bigDivide128(txResult.getResult().feeCharged, v2,
                          getMinInclusionFee(*mInnerTx, lh),
                          Rounding::ROUND_UP))
        {
            txResult.getResult().feeCharged = INT64_MAX;
        }
        txResult.setResultCode(txINSUFFICIENT_FEE);
        return false;
    }

    if (!ls.getAccount(getFeeSourceID()))
    {
        txResult.setResultCode(txNO_ACCOUNT);
        return false;
    }

    return true;
}

FeeBumpTransactionFrame::ValidationType
FeeBumpTransactionFrame::commonValid(
    SignatureChecker& signatureChecker, LedgerSnapshot const& ls, bool applying,
    MutableTransactionResultBase& txResult) const
{
    ValidationType res = ValidationType::kInvalid;

    if (!commonValidPreSeqNum(ls, txResult))
    {
        return res;
    }

    auto const feeSource = ls.getAccount(getFeeSourceID());
    if (!checkSignature(
            signatureChecker, feeSource,
            feeSource.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        txResult.setResultCode(txBAD_AUTH);
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
    if (getAvailableBalance(header.current(), feeSource.current()) < feeToPay)
    {
        txResult.setResultCode(txINSUFFICIENT_BALANCE);
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

Resource
FeeBumpTransactionFrame::getResources(bool useByteLimitInClassic) const
{
    auto res = mInnerTx->getResources(useByteLimitInClassic);
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
FeeBumpTransactionFrame::insertKeysForTxApply(UnorderedSet<LedgerKey>& keys,
                                              LedgerKeyMeter* lkMeter) const
{
    mInnerTx->insertKeysForTxApply(keys, lkMeter);
}

MutableTxResultPtr
FeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                          std::optional<int64_t> baseFee) const
{
    auto txResult = createSuccessResultWithFeeCharged(
        ltx.loadHeader().current(), baseFee, true);
    releaseAssert(txResult);

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!feeSource)
    {
        throw std::runtime_error("Unexpected database state");
    }
    auto& acc = feeSource.current().data.account();

    auto header = ltx.loadHeader();

    int64_t& fee = txResult->getResult().feeCharged;
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.current().feePool += fee;
    }

    return txResult;
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
FeeBumpTransactionFrame::createSuccessResultWithFeeCharged(
    LedgerHeader const& header, std::optional<int64_t> baseFee,
    bool applying) const
{
    auto innerTxResult =
        mInnerTx->createSuccessResultWithFeeCharged(header, baseFee, applying);

    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    auto feeCharged = getFee(header, baseFee, applying);
    std::shared_ptr<FeeBumpMutableTransactionResult> txResult(
        new FeeBumpMutableTransactionResult(innerTxResult));
    txResult->setResultCode(txFEE_BUMP_INNER_SUCCESS);
    txResult->getResult().feeCharged = feeCharged;

    return txResult;
}

MutableTxResultPtr
FeeBumpTransactionFrame::createSuccessResult() const
{
    return MutableTxResultPtr(
        new FeeBumpMutableTransactionResult(mInnerTx->createSuccessResult()));
}

MutableTxResultPtr
FeeBumpTransactionFrame::createSuccessResultWithNewInnerTx(
    MutableTxResultPtr&& outerResult, MutableTxResultPtr&& innerResult,
    TransactionFrameBasePtr innerTx) const
{
    return MutableTxResultPtr(new FeeBumpMutableTransactionResult(
        std::move(outerResult), std::move(innerResult), innerTx));
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
