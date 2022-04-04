// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "crypto/SignerKeyUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/SponsorshipUtils.h"
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

static void
updateResult(TransactionResult& outerRes, TransactionResult& innerRes,
             TransactionFrameBasePtr innerTx)
{
    if (innerRes.result.code() == txSUCCESS)
    {
        outerRes.result.code(txFEE_BUMP_INNER_SUCCESS);
    }
    else
    {
        outerRes.result.code(txFEE_BUMP_INNER_FAILED);
    }

    auto& irp = outerRes.result.innerResultPair();
    irp.transactionHash = innerTx->getContentsHash();

    auto& res = irp.result;
    res.feeCharged = innerRes.feeCharged;
    res.result.code(innerRes.result.code());
    switch (res.result.code())
    {
    case txSUCCESS:
    case txFAILED:
    {
        res.result.results() = innerRes.result.results();
        break;
    }
    default:
        break;
    }
}

bool
FeeBumpTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                               TransactionMeta& meta,
                               TransactionResult& txResult)
{
    try
    {
        LedgerTxn ltxTx(ltx);
        removeOneTimeSignerKeyFromFeeSource(ltxTx);

        auto& txChanges = meta.v2().txChangesBefore;
        txChanges = ltxTx.getChanges();
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
        TransactionResult innerRes;
        bool res = mInnerTx->apply(app, ltx, meta, false, innerRes);
        // If this throws, then we may not have the correct TransactionResult so
        // we must crash.
        updateResult(txResult, innerRes, mInnerTx);
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
FeeBumpTransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                        LedgerTxnEntry const& account,
                                        int32_t neededWeight)
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

bool
FeeBumpTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                    SequenceNumber current,
                                    uint64_t lowerBoundCloseTimeOffset,
                                    uint64_t upperBoundCloseTimeOffset,
                                    TransactionResult& txResult)
{
    LedgerTxn ltx(ltxOuter);
    auto minBaseFee = ltx.loadHeader().current().baseFee;
    TransactionResult innerRes;
    resetResults(ltx.loadHeader().current(), minBaseFee, false, innerRes,
                 txResult);

    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(),
                                      mEnvelope.feeBump().signatures};
    if (commonValid(signatureChecker, ltx, false, txResult) !=
        ValidationType::kFullyValid)
    {
        return false;
    }
    if (!signatureChecker.checkAllSignaturesUsed())
    {
        txResult.result.code(txBAD_AUTH_EXTRA);
        return false;
    }

    bool res =
        mInnerTx->checkValid(ltx, current, false, lowerBoundCloseTimeOffset,
                             upperBoundCloseTimeOffset, innerRes);
    updateResult(txResult, innerRes, mInnerTx);
    return res;
}

bool
FeeBumpTransactionFrame::commonValidPreSeqNum(AbstractLedgerTxn& ltx,
                                              TransactionResult& txResult)
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    auto header = ltx.loadHeader();
    if (protocolVersionIsBefore(header.current().ledgerVersion,
                                ProtocolVersion::V_13))
    {
        txResult.result.code(txNOT_SUPPORTED);
        return false;
    }

    if (getFeeBid() < getMinFee(header.current()))
    {
        txResult.result.code(txINSUFFICIENT_FEE);
        return false;
    }

    auto const& lh = header.current();
    uint128_t v1 = bigMultiply(getFeeBid(), mInnerTx->getMinFee(lh));
    uint128_t v2 = bigMultiply(mInnerTx->getFeeBid(), getMinFee(lh));
    if (v1 < v2)
    {
        if (!bigDivide128(txResult.feeCharged, v2, mInnerTx->getMinFee(lh),
                          Rounding::ROUND_UP))
        {
            txResult.feeCharged = INT64_MAX;
        }
        txResult.result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!stellar::loadAccount(ltx, getFeeSourceID()))
    {
        txResult.result.code(txNO_ACCOUNT);
        return false;
    }

    return true;
}

FeeBumpTransactionFrame::ValidationType
FeeBumpTransactionFrame::commonValid(SignatureChecker& signatureChecker,
                                     AbstractLedgerTxn& ltxOuter, bool applying,
                                     TransactionResult& txResult)
{
    LedgerTxn ltx(ltxOuter);
    ValidationType res = ValidationType::kInvalid;

    if (!commonValidPreSeqNum(ltx, txResult))
    {
        return res;
    }

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!checkSignature(
            signatureChecker, feeSource,
            feeSource.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        txResult.result.code(txBAD_AUTH);
        return res;
    }

    res = ValidationType::kInvalidPostAuth;

    auto header = ltx.loadHeader();
    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    int64_t feeToPay = applying ? 0 : getFeeBid();
    // don't let the account go below the reserve after accounting for
    // liabilities
    if (getAvailableBalance(header, feeSource) < feeToPay)
    {
        txResult.result.code(txINSUFFICIENT_BALANCE);
        return res;
    }

    return ValidationType::kFullyValid;
}

TransactionEnvelope const&
FeeBumpTransactionFrame::getEnvelope() const
{
    return mEnvelope;
}

int64_t
FeeBumpTransactionFrame::getFeeBid() const
{
    return mEnvelope.feeBump().tx.fee;
}

int64_t
FeeBumpTransactionFrame::getMinFee(LedgerHeader const& header) const
{
    return ((int64_t)header.baseFee) * std::max<int64_t>(1, getNumOperations());
}

int64_t
FeeBumpTransactionFrame::getFee(LedgerHeader const& header, int64_t baseFee,
                                bool applying) const
{
    int64_t adjustedFee = baseFee * std::max<int64_t>(1, getNumOperations());
    if (applying)
    {
        return std::min<int64_t>(getFeeBid(), adjustedFee);
    }
    else
    {
        return adjustedFee;
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

void
FeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                          int64_t baseFee,
                                          TransactionResult& txResult)
{
    TransactionResult innerRes;
    resetResults(ltx.loadHeader().current(), baseFee, true, innerRes, txResult);

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!feeSource)
    {
        throw std::runtime_error("Unexpected database state");
    }
    auto& acc = feeSource.current().data.account();

    auto header = ltx.loadHeader();
    int64_t& fee = txResult.feeCharged;
    if (fee > 0)
    {
        fee = std::min(acc.balance, fee);
        // Note: TransactionUtil addBalance checks that reserve plus liabilities
        // are respected. In this case, we allow it to fall below that since it
        // will be caught later in commonValid.
        stellar::addBalance(acc.balance, -fee);
        header.current().feePool += fee;
    }
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

void
FeeBumpTransactionFrame::resetResults(LedgerHeader const& header,
                                      int64_t baseFee, bool applying,
                                      TransactionResult& innerRes,
                                      TransactionResult& outerRes)
{
    mInnerTx->resetResults(header, baseFee, applying, innerRes);
    outerRes.result.code(txFEE_BUMP_INNER_SUCCESS);

    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    outerRes.feeCharged = getFee(header, baseFee, applying);
}

StellarMessage
FeeBumpTransactionFrame::toStellarMessage() const
{
    StellarMessage msg(TRANSACTION);
    msg.transaction() = mEnvelope;
    return msg;
}
}
