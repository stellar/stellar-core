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
#include "transactions/TransactionUtils.h"
#include "util/Algoritm.h"
#include "util/Decoder.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"
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
    assert(isZero(oldHash) || (oldHash == mContentsHash));
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

int64_t
TransactionFrame::getFeeBid() const
{
    return mEnvelope.type() == ENVELOPE_TYPE_TX_V0 ? mEnvelope.v0().tx.fee
                                                   : mEnvelope.v1().tx.fee;
}

int64_t
TransactionFrame::getMinFee(LedgerHeader const& header) const
{
    return ((int64_t)header.baseFee) * std::max<int64_t>(1, getNumOperations());
}

int64_t
TransactionFrame::getFee(LedgerHeader const& header, int64_t baseFee,
                         bool applying) const
{
    if (header.ledgerVersion >= 11 || !applying)
    {
        int64_t adjustedFee =
            baseFee * std::max<int64_t>(1, getNumOperations());

        if (applying)
        {
            return std::min<int64_t>(getFeeBid(), adjustedFee);
        }
        else
        {
            return adjustedFee;
        }
    }
    else
    {
        return getFeeBid();
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

LedgerTxnEntry
TransactionFrame::loadSourceAccount(AbstractLedgerTxn& ltx,
                                    LedgerTxnHeader const& header)
{
    ZoneScoped;
    auto res = loadAccount(ltx, header, getSourceID());
    if (header.current().ledgerVersion < 8)
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
    if (header.current().ledgerVersion < 8 && mCachedAccount &&
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

std::shared_ptr<OperationFrame>
TransactionFrame::makeOperation(Operation const& op, OperationResult& res,
                                size_t index)
{
    return OperationFrame::makeHelper(op, res, *this,
                                      static_cast<uint32_t>(index));
}

void
TransactionFrame::resetResults(LedgerHeader const& header, int64_t baseFee,
                               bool applying)
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

bool
TransactionFrame::isTooEarly(LedgerTxnHeader const& header,
                             uint64_t lowerBoundCloseTimeOffset) const
{
    auto const& tb = mEnvelope.type() == ENVELOPE_TYPE_TX_V0
                         ? mEnvelope.v0().tx.timeBounds
                         : mEnvelope.v1().tx.timeBounds;
    if (tb)
    {
        uint64 closeTime = header.current().scpValue.closeTime;
        return tb->minTime &&
               (tb->minTime > (closeTime + lowerBoundCloseTimeOffset));
    }
    return false;
}

bool
TransactionFrame::isTooLate(LedgerTxnHeader const& header,
                            uint64_t upperBoundCloseTimeOffset) const
{
    auto const& tb = mEnvelope.type() == ENVELOPE_TYPE_TX_V0
                         ? mEnvelope.v0().tx.timeBounds
                         : mEnvelope.v1().tx.timeBounds;
    if (tb)
    {
        // Prior to consensus, we can pass in an upper bound estimate on when we
        // expect the ledger to close so we don't accept transactions that will
        // expire by the time they are applied
        uint64 closeTime = header.current().scpValue.closeTime;
        return tb->maxTime &&
               (tb->maxTime < (closeTime + upperBoundCloseTimeOffset));
    }
    return false;
}

bool
TransactionFrame::commonValidPreSeqNum(AbstractLedgerTxn& ltx, bool chargeFee,
                                       uint64_t lowerBoundCloseTimeOffset,
                                       uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    // this function does validations that are independent of the account state
    //    (stay true regardless of other side effects)
    auto header = ltx.loadHeader();
    uint32_t ledgerVersion = header.current().ledgerVersion;
    if ((ledgerVersion < 13 && (mEnvelope.type() == ENVELOPE_TYPE_TX ||
                                hasMuxedAccount(mEnvelope))) ||
        (ledgerVersion >= 13 && mEnvelope.type() == ENVELOPE_TYPE_TX_V0))
    {
        getResult().result.code(txNOT_SUPPORTED);
        return false;
    }

    if (getNumOperations() == 0)
    {
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

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

    if (chargeFee && getFeeBid() < getMinFee(header.current()))
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
    if (header.current().ledgerVersion >= 10)
    {
        auto sourceAccount = loadSourceAccount(ltx, header);
        if (sourceAccount.current().data.account().seqNum > getSeqNum())
        {
            throw std::runtime_error("unexpected sequence number");
        }
        sourceAccount.current().data.account().seqNum = getSeqNum();
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
    if (ledgerVersion < 10)
    {
        return maybeValid;
    }

    // check if we need to fast fail and use the original error code
    if (ledgerVersion >= 13 && !maybeValid)
    {
        removeOneTimeSignerFromAllSourceAccounts(ltxOuter);
        return false;
    }
    // older versions of the protocol only fast fail in a subset of cases
    if (ledgerVersion < 13 && cv < ValidationType::kInvalidPostAuth)
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
TransactionFrame::isBadSeq(int64_t seqNum) const
{
    return seqNum == INT64_MAX || seqNum + 1 != getSeqNum();
}

TransactionFrame::ValidationType
TransactionFrame::commonValid(SignatureChecker& signatureChecker,
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

    if (!commonValidPreSeqNum(ltx, chargeFee, lowerBoundCloseTimeOffset,
                              upperBoundCloseTimeOffset))
    {
        return res;
    }

    auto header = ltx.loadHeader();
    auto sourceAccount = loadSourceAccount(ltx, header);

    // in older versions, the account's sequence number is updated when taking
    // fees
    if (header.current().ledgerVersion >= 10 || !applying)
    {
        if (current == 0)
        {
            current = sourceAccount.current().data.account().seqNum;
        }
        if (isBadSeq(current))
        {
            getResult().result.code(txBAD_SEQ);
            return res;
        }
    }

    res = ValidationType::kInvalidUpdateSeqNum;

    if (!checkSignature(
            signatureChecker, sourceAccount,
            sourceAccount.current().data.account().thresholds[THRESHOLD_LOW]))
    {
        getResult().result.code(txBAD_AUTH);
        return res;
    }

    res = ValidationType::kInvalidPostAuth;

    // if we are in applying mode fee was already deduced from signing account
    // balance, if not, we need to check if after that deduction this account
    // will still have minimum balance
    uint32_t feeToPay = (applying && (header.current().ledgerVersion > 8))
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
TransactionFrame::processFeeSeqNum(AbstractLedgerTxn& ltx, int64_t baseFee)
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
    if (header.current().ledgerVersion <= 9)
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
TransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                             SequenceNumber current, bool chargeFee,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset)
{
    ZoneScoped;
    mCachedAccount.reset();

    LedgerTxn ltx(ltxOuter);
    int64_t minBaseFee = chargeFee ? ltx.loadHeader().current().baseFee : 0;
    resetResults(ltx.loadHeader().current(), minBaseFee, false);

    SignatureChecker signatureChecker{ltx.loadHeader().current().ledgerVersion,
                                      getContentsHash(),
                                      getSignatures(mEnvelope)};
    bool res =
        commonValid(signatureChecker, ltx, current, false, chargeFee,
                    lowerBoundCloseTimeOffset,
                    upperBoundCloseTimeOffset) == ValidationType::kMaybeValid;
    if (res)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(signatureChecker, ltx, false))
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
TransactionFrame::checkValid(AbstractLedgerTxn& ltxOuter,
                             SequenceNumber current,
                             uint64_t lowerBoundCloseTimeOffset,
                             uint64_t upperBoundCloseTimeOffset)
{
    return checkValid(ltxOuter, current, true, lowerBoundCloseTimeOffset,
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
    // Changing "code" normally causes the XDR structure to be destructed, then
    // a different XDR structure is constructed. However, txFAILED and txSUCCESS
    // have the same underlying field number so this does not occur.
    getResult().result.code(txFAILED);
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx)
{
    TransactionMeta tm(2);
    return apply(app, ltx, tm);
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  Application& app, AbstractLedgerTxn& ltx,
                                  TransactionMeta& outerMeta)
{
    ZoneScoped;
    try
    {
        bool success = true;

        TransactionMeta newMeta(2);
        newMeta.v2().operations.reserve(getNumOperations());

        // shield outer scope of any side effects with LedgerTxn
        LedgerTxn ltxTx(ltx);
        uint32_t ledgerVersion = ltxTx.loadHeader().current().ledgerVersion;

        auto& opTimer =
            app.getMetrics().NewTimer({"ledger", "operation", "apply"});
        for (auto& op : mOperations)
        {
            auto time = opTimer.TimeScope();
            LedgerTxn ltxOp(ltxTx);
            bool txRes = op->apply(signatureChecker, ltxOp);

            if (!txRes)
            {
                success = false;
            }
            if (success)
            {
                app.getInvariantManager().checkOnOperationApply(
                    op->getOperation(), op->getResult(), ltxOp.getDelta());

                // The operation meta will be empty if the transaction doesn't
                // succeed so we may as well not do any work in that case
                newMeta.v2().operations.emplace_back(ltxOp.getChanges());
            }

            if (txRes || ledgerVersion < 14)
            {
                ltxOp.commit();
            }
        }

        if (success)
        {
            if (ledgerVersion < 10)
            {
                if (!signatureChecker.checkAllSignaturesUsed())
                {
                    getResult().result.code(txBAD_AUTH_EXTRA);
                    // this should never happen: malformed transaction should
                    // not be accepted by nodes
                    return false;
                }

                // if an error occurred, it is responsibility of account's owner
                // to remove that signer
                LedgerTxn ltxAfter(ltxTx);
                removeOneTimeSignerFromAllSourceAccounts(ltxAfter);
                newMeta.v2().txChangesAfter = ltxAfter.getChanges();
                ltxAfter.commit();
            }
            else if (ledgerVersion >= 14 && ltxTx.hasSponsorshipEntry())
            {
                getResult().result.code(txBAD_SPONSORSHIP);
                return false;
            }

            ltxTx.commit();
            // commit -> propagate the meta to the outer scope
            std::swap(outerMeta.v2().operations, newMeta.v2().operations);
            std::swap(outerMeta.v2().txChangesAfter,
                      newMeta.v2().txChangesAfter);
        }
        else
        {
            markResultFailed();
        }
        return success;
    }
    catch (InvariantDoesNotHold&)
    {
        printErrorAndAbort("Invariant failure while applying operations");
    }
    catch (std::bad_alloc& e)
    {
        printErrorAndAbort("Exception while applying operations: ", e.what());
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Tx, "Exception while applying operations ({}, {}): {}",
                   xdr_to_string(getFullHash(), "fullHash"),
                   xdr_to_string(getContentsHash(), "contentsHash"), e.what());
    }
    catch (...)
    {
        CLOG_ERROR(Tx, "Unknown exception while applying operations ({}, {})",
                   xdr_to_string(getFullHash(), "fullHash"),
                   xdr_to_string(getContentsHash(), "contentsHash"));
    }
    // This is only reachable if an exception is thrown
    getResult().result.code(txINTERNAL_ERROR);

    auto& internalErrorCounter = app.getMetrics().NewCounter(
        {"ledger", "transaction", "internal-error"});
    internalErrorCounter.inc();

    // operations and txChangesAfter should already be empty at this point
    outerMeta.v2().operations.clear();
    outerMeta.v2().txChangesAfter.clear();
    return false;
}

bool
TransactionFrame::apply(Application& app, AbstractLedgerTxn& ltx,
                        TransactionMeta& meta, bool chargeFee)
{
    ZoneScoped;
    try
    {
        mCachedAccount.reset();
        SignatureChecker signatureChecker{
            ltx.loadHeader().current().ledgerVersion, getContentsHash(),
            getSignatures(mEnvelope)};

        LedgerTxn ltxTx(ltx);
        // when applying, a failure during tx validation means that
        // we'll skip trying to apply operations but we'll still
        // process the sequence number if needed
        auto cv =
            commonValid(signatureChecker, ltxTx, 0, true, chargeFee, 0, 0);
        if (cv >= ValidationType::kInvalidUpdateSeqNum)
        {
            processSeqNum(ltxTx);
        }

        bool signaturesValid = processSignatures(cv, signatureChecker, ltxTx);

        auto changes = ltxTx.getChanges();
        std::move(changes.begin(), changes.end(),
                  std::back_inserter(meta.v2().txChangesBefore));
        ltxTx.commit();

        bool valid = signaturesValid && cv == ValidationType::kMaybeValid;
        try
        {
            // This should only throw if the logging during exception handling
            // for applyOperations throws. In that case, we may not have the
            // correct TransactionResult so we must crash.
            return valid && applyOperations(signatureChecker, app, ltx, meta);
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
                        TransactionMeta& meta)
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
