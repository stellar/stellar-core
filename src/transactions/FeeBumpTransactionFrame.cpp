// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/FeeBumpTransactionFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "transactions/SignatureChecker.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "xdrpp/marshal.h"

#include <numeric>

namespace stellar
{

static TransactionEnvelope
fromFeeBumpInnerTx(TransactionEnvelope const& envelope)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX_V0);
    e.v0().tx = envelope.feeBump().tx.innerTx.v0().tx;
    e.v0().signatures = envelope.feeBump().tx.innerTx.v0().signatures;
    return e;
}

FeeBumpTransactionFrame::FeeBumpTransactionFrame(
    Hash const& networkID, TransactionEnvelope const& envelope)
    : mEnvelope(envelope)
    , mInnerTx(std::make_shared<TransactionFrame>(
          networkID, fromFeeBumpInnerTx(envelope), false))
    , mNetworkID(networkID)
{
}

bool
FeeBumpTransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                               TransactionMetaV1& meta)
{
    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(),
                                      mEnvelope.feeBump().signatures};

    LedgerTxn ltxTx(ltx);
    // when applying, a failure during tx validation means that
    // we'll skip trying to apply operations but we'll still
    // process the sequence number if needed
    auto cv = commonValid(signatureChecker, ltxTx, 0, true);

    auto signaturesValid = cv >= (ValidationType::kInvalidPostAuth) &&
                           processSignatures(signatureChecker, ltxTx);
    meta.txChanges = ltxTx.getChanges();
    ltxTx.commit();

    return signaturesValid && (cv == ValidationType::kFullyValid);
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

    return signatureChecker.checkSignature(acc.accountID, signers,
                                           neededWeight);
}

bool
FeeBumpTransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                                    SequenceNumber current)
{
    LedgerTxn ltx(ltxOuter);
    auto minBaseFee = ltx.loadHeader().current().baseFee;
    resetResults(ltx.loadHeader().current(), minBaseFee);

    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(),
                                      mEnvelope.feeBump().signatures};
    if (commonValid(signatureChecker, ltx, current, false) !=
        ValidationType::kFullyValid)
    {
        return false;
    }
    if (!signatureChecker.checkAllSignaturesUsed())
    {
        getResult().result.code(txBAD_AUTH_EXTRA);
        return false;
    }

    if (!mInnerTx->checkValid(ltx, current))
    {
        getResult().result.code(txINNER_TX_INVALID);
        auto& inner = getResult().result.innerResult();
        inner.feeCharged = mInnerTx->getResult().feeCharged;
        inner.result.code(mInnerTx->getResult().result.code());
        switch (inner.result.code())
        {
        case txSUCCESS:
        case txFAILED:
            inner.result.results() = mInnerTx->getResult().result.results();
            break;
        case txINNER_TX_INVALID:
            throw std::runtime_error("unexpected result code");
        default:
            break;
        }
        return false;
    }

    return true;
}

bool
FeeBumpTransactionFrame::commonValidPreSeqNum(AbstractLedgerTxn& ltx,
                                              bool forApply)
{
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)

    auto header = ltx.loadHeader();
    if (header.current().ledgerVersion < 12)
    {
        getResult().result.code(txNOT_SUPPORTED);
        return false;
    }

    if (!std::is_sorted(mEnvelope.feeBump().signatures.begin(),
                        mEnvelope.feeBump().signatures.end(), signatureCompare))
    {
        getResult().result.code(txNOT_NORMALIZED);
        return false;
    }

    if (isTooEarly(header))
    {
        getResult().result.code(txTOO_EARLY);
        return false;
    }
    if (isTooLate(header))
    {
        getResult().result.code(txTOO_LATE);
        return false;
    }

    // If fee exceeds INT64_MAX, it is impossible to have sufficient balance.
    // Checking this here makes it possible to use getFeeBid safely in other
    // places.
    if (mEnvelope.feeBump().tx.fee > INT64_MAX)
    {
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return false;
    }

    if (getFeeBid() < getMinFee(header.current()))
    {
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    if (!stellar::loadAccount(ltx, getFeeSourceID()))
    {
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    return true;
}

FeeBumpTransactionFrame::ValidationType
FeeBumpTransactionFrame::commonValid(SignatureChecker& signatureChecker,
                                     AbstractLedgerTxn& ltxOuter,
                                     SequenceNumber current, bool applying)
{
    LedgerTxn ltx(ltxOuter);
    ValidationType res = ValidationType::kInvalid;

    if (!commonValidPreSeqNum(ltx, applying))
    {
        return res;
    }

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!checkSignature(
            signatureChecker, feeSource,
            feeSource.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        getResult().result.code(txBAD_AUTH);
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
        getResult().result.code(txINSUFFICIENT_BALANCE);
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
    assert(mEnvelope.feeBump().tx.fee <= INT64_MAX);
    return mEnvelope.feeBump().tx.fee;
}

int64_t
FeeBumpTransactionFrame::getMinFee(LedgerHeader const& header) const
{
    return ((int64_t)header.baseFee) *
           std::max<int64_t>(1, getOperationCountForValidation());
}

int64_t
FeeBumpTransactionFrame::getFee(LedgerHeader const& header,
                                int64_t baseFee) const
{
    int64_t adjustedFee =
        baseFee * std::max<int64_t>(1, getOperationCountForValidation());
    return std::min<int64_t>(getFeeBid(), adjustedFee);
}

Hash const&
FeeBumpTransactionFrame::getContentsHash() const
{
    if (isZero(mContentsHash))
    {
        mContentsHash = sha256(xdr::xdr_to_opaque(
            mNetworkID, ENVELOPE_TYPE_FEE_BUMP, mEnvelope.feeBump().tx));
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
FeeBumpTransactionFrame::getInnerHash() const
{
    return mInnerTx->getFullHash();
}

size_t
FeeBumpTransactionFrame::getOperationCountForApply() const
{
    return 0;
}

size_t
FeeBumpTransactionFrame::getOperationCountForValidation() const
{
    return mInnerTx->getOperationCountForValidation() + 1;
}

TransactionResult&
FeeBumpTransactionFrame::getResult()
{
    return mResult;
}

TransactionResultCode
FeeBumpTransactionFrame::getResultCode() const
{
    return mResult.result.code();
}

TransactionResultPair
FeeBumpTransactionFrame::getResultPair() const
{
    TransactionResultPair trp;
    trp.transactionHash = getContentsHash();
    trp.result = mResult;
    return trp;
}

SequenceNumber
FeeBumpTransactionFrame::getSeqNum() const
{
    return mInnerTx->getSeqNum();
}

AccountID
FeeBumpTransactionFrame::getFeeSourceID() const
{
    return mEnvelope.feeBump().tx.feeSource;
}

AccountID
FeeBumpTransactionFrame::getSourceID() const
{
    return mInnerTx->getSourceID();
}

void
FeeBumpTransactionFrame::insertLedgerKeysToPrefetch(
    std::unordered_set<LedgerKey>& keys) const
{
    mInnerTx->insertLedgerKeysToPrefetch(keys);
}

bool
FeeBumpTransactionFrame::isTooEarly(LedgerTxnHeader const& header) const
{
    auto const& tb = mEnvelope.feeBump().tx.innerTx.v0().tx.timeBounds;
    if (tb)
    {
        uint64 closeTime = header.current().scpValue.closeTime;
        return tb->minTime > closeTime;
    }
    return false;
}

bool
FeeBumpTransactionFrame::isTooLate(LedgerTxnHeader const& header) const
{
    auto const& tb = mEnvelope.feeBump().tx.innerTx.v0().tx.timeBounds;
    if (tb)
    {
        uint64 closeTime = header.current().scpValue.closeTime;
        return tb->maxTime && (tb->maxTime < closeTime);
    }
    return false;
}

void
FeeBumpTransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx,
                                          int64_t baseFee)
{
    resetResults(ltx.loadHeader().current(), baseFee);

    auto feeSource = stellar::loadAccount(ltx, getFeeSourceID());
    if (!feeSource)
    {
        throw std::runtime_error("Unexpected database state");
    }
    auto& acc = feeSource.current().data.account();

    auto header = ltx.loadHeader();
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
}

bool
FeeBumpTransactionFrame::processSignatures(SignatureChecker& signatureChecker,
                                           AbstractLedgerTxn& ltxOuter)
{
    removeUsedOneTimeSignerKeys(signatureChecker, ltxOuter);
    if (!signatureChecker.checkAllSignaturesUsed())
    {
        getResult().result.code(txBAD_AUTH_EXTRA);
        return false;
    }
    return true;
}

bool
FeeBumpTransactionFrame::removeAccountSigner(LedgerTxnHeader const& header,
                                             LedgerTxnEntry& account,
                                             SignerKey const& signerKey) const
{
    auto& acc = account.current().data.account();
    auto it = std::find_if(
        std::begin(acc.signers), std::end(acc.signers),
        [&signerKey](Signer const& signer) { return signer.key == signerKey; });
    if (it != std::end(acc.signers))
    {
        auto removed = stellar::addNumEntries(header, account, -1);
        assert(removed == AddSubentryResult::SUCCESS);
        acc.signers.erase(it);
        return true;
    }

    return false;
}

void
FeeBumpTransactionFrame::removeUsedOneTimeSignerKeys(
    SignatureChecker& signatureChecker, AbstractLedgerTxn& ltx) const
{
    for (auto const& usedAccount : signatureChecker.usedOneTimeSignerKeys())
    {
        removeUsedOneTimeSignerKeys(ltx, usedAccount.first, usedAccount.second);
    }
}

void
FeeBumpTransactionFrame::removeUsedOneTimeSignerKeys(
    AbstractLedgerTxn& ltx, AccountID const& accountID,
    std::set<SignerKey> const& keys) const
{
    auto account = stellar::loadAccount(ltx, accountID);
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto header = ltx.loadHeader();
    auto changed = std::accumulate(
        std::begin(keys), std::end(keys), false,
        [&](bool r, const SignerKey& signerKey) {
            return r || removeAccountSigner(header, account, signerKey);
        });

    if (changed)
    {
        normalizeSigners(account);
    }
}

void
FeeBumpTransactionFrame::resetResults(LedgerHeader const& header,
                                      int64_t baseFee)
{
    getResult().result.code(txFEE_BUMPED);

    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    getResult().feeCharged = getFee(header, baseFee);
}

StellarMessage
FeeBumpTransactionFrame::toStellarMessage() const
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = mEnvelope;
    return msg;
}

std::vector<TransactionFrameBasePtr>
FeeBumpTransactionFrame::transactionsToApply()
{
    return {shared_from_this(), mInnerTx};
}
}
