// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "TransactionFrame.h"
#include "OperationFrame.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "herder/TxSetFrame.h"
#include "ledgerdelta/LedgerDeltaScope.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledgerdelta/LedgerDeltaLayer.h"
#include "ledger/LedgerEntries.h"
#include "main/Application.h"
#include "signature/SignatureChecker.h"
#include "signature/SignatureUtils.h"
#include "signature/SigningAccount.h"
#include "util/Algoritm.h"
#include "util/Logging.h"
#include "util/XDRStream.h"
#include "util/basen.h"
#include "xdrpp/marshal.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include <numeric>
#include <string>

namespace stellar
{

namespace
{

bool triggerManageDataBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() == 3;
}

bool allowMultipleMerges(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool multipleMergeReturnsMergeNoAccount(Application& app)
{
    return
        app.getLedgerManager().getCurrentLedgerVersion() > 4 &&
        app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool allowMergeCreateMergeBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 6;
}

bool allowMergeBackBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 6;
}

bool allowCreateMergePayBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool allowPayMergeCreatePayBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool allowMergePayBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool allowSelfXLMtoXLMPaymentBug(Application& app)
{
    return app.getLedgerManager().getCurrentLedgerVersion() < 8;
}

bool isMerge(OperationFrame const& op)
{
    return op.getOperation().body.type() == ACCOUNT_MERGE;
}

bool isMergeFrom(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isMerge(op)
        && accountFrame->getAccountID() == op.getSourceID();
}

bool isMergeTo(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isMerge(op)
        && accountFrame->getAccountID() == op.getOperation().body.destination();
}

bool isCreate(OperationFrame const& op)
{
    return op.getOperation().body.type() == CREATE_ACCOUNT;
}

bool isCreateFrom(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isCreate(op)
        && accountFrame->getAccountID() == op.getSourceID();
}

bool isCreateDestination(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isCreate(op)
        && accountFrame->getAccountID() == AccountFrame{op.getOperation().body.createAccountOp().destination}.getAccountID();
}

bool isPayment(OperationFrame const& op)
{
    return op.getOperation().body.type() == PAYMENT;
}

bool isPaymentSource(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isPayment(op)
        && accountFrame->getAccountID() == op.getSourceID();
}

bool isPaymentDestination(OperationFrame const& op, optional<AccountFrame> accountFrame)
{
    return accountFrame
        && isPayment(op)
        && accountFrame->getAccountID() == AccountFrame{op.getOperation().body.paymentOp().destination}.getAccountID();
}

bool isPathPayment(OperationFrame const& op)
{
    return op.getOperation().body.type() == PATH_PAYMENT;
}

bool isPathPaymentSelfXLMtoXLM(OperationFrame const& op)
{
    return isPathPayment(op)
        && op.getOperation().body.pathPaymentOp().sendAsset.type() == ASSET_TYPE_NATIVE
        && op.getOperation().body.pathPaymentOp().destAsset.type() == ASSET_TYPE_NATIVE
        && op.getOperation().body.pathPaymentOp().destination == op.getSourceID();
}

bool isManageData(OperationFrame const& op)
{
    return op.getOperation().body.type() == MANAGE_DATA;
}

}

using namespace std;
using xdr::operator==;

TransactionFramePtr
TransactionFrame::makeTransactionFromWire(Hash const& networkID,
                                          TransactionEnvelope const& msg)
{
    TransactionFramePtr res = make_shared<TransactionFrame>(networkID, msg);
    return res;
}

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
        mFullHash = sha256(xdr::xdr_to_opaque(mEnvelope));
    }
    return (mFullHash);
}

Hash const&
TransactionFrame::getContentsHash() const
{
    if (isZero(mContentsHash))
    {
        mContentsHash = sha256(
            xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_TX, mEnvelope.tx));
    }
    return (mContentsHash);
}

void
TransactionFrame::clearCached()
{
    Hash zero;
    mContentsHash = zero;
    mFullHash = zero;
}

TransactionResultPair
TransactionFrame::getResultPair() const
{
    TransactionResultPair trp;
    trp.transactionHash = getContentsHash();
    trp.result = mResult;
    return trp;
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

double
TransactionFrame::getFeeRatio(LedgerManager const& lm) const
{
    return ((double)getFee() / (double)getMinFee(lm));
}

int64_t
TransactionFrame::getFee() const
{
    return mEnvelope.tx.fee;
}

int64_t
TransactionFrame::getMinFee(LedgerManager const& lm) const
{
    size_t count = mOperations.size();

    if (count == 0)
    {
        count = 1;
    }

    return lm.getTxFee() * count;
}

void
TransactionFrame::addSignature(SecretKey const& secretKey)
{
    clearCached();
    auto sig = SignatureUtils::sign(secretKey, getContentsHash());
    addSignature(sig);
}

void
TransactionFrame::addSignature(DecoratedSignature const& signature)
{
    mEnvelope.signatures.push_back(signature);
}

bool
TransactionFrame::checkSignature(SignatureChecker& signatureChecker,
                                 SigningAccount const& signingAccount, ThresholdLevel threshold)
{
    std::vector<Signer> signers;
    if (signingAccount.weight)
        signers.push_back(
            Signer(KeyUtils::convertKey<SignerKey>(signingAccount.accountID),
                   signingAccount.weight));
    signers.insert(signers.end(), std::begin(signingAccount.signers),
                   std::end(signingAccount.signers));

    auto neededWeight = [&](){;
        switch (threshold)
        {
        case ThresholdLevel::LOW:
            return signingAccount.lowThreshold;
        case ThresholdLevel::MEDIUM:
            return signingAccount.mediumThreshold;
        case ThresholdLevel::HIGH:
            return signingAccount.highThreshold;
        };
    }();

    return signatureChecker.checkSignature(signingAccount.accountID, signers,
                                           neededWeight);
}

void
TransactionFrame::resetResults()
{
    // pre-allocates the results for all operations
    getResult().result.code(txSUCCESS);
    getResult().result.results().resize(
        (uint32_t)mEnvelope.tx.operations.size());

    mOperations.clear();

    // bind operations to the results
    for (size_t i = 0; i < mEnvelope.tx.operations.size(); i++)
    {
        mOperations.push_back(
            OperationFrame::makeHelper(mEnvelope.tx.operations[i],
                                       getResult().result.results()[i], *this));
    }

    // feeCharged is updated accordingly to represent the cost of the
    // transaction regardless of the failure modes.
    getResult().feeCharged = getFee();
}

bool
TransactionFrame::commonValid(SignatureChecker& signatureChecker,
                              Application& app, LedgerDelta* ledgerDelta,
                              SequenceNumber current)
{
    bool applying = (ledgerDelta != nullptr);

    if (mOperations.size() == 0)
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "missing-operation"},
                      "transaction")
            .Mark();
        getResult().result.code(txMISSING_OPERATION);
        return false;
    }

    auto& lm = app.getLedgerManager();
    if (mEnvelope.tx.timeBounds)
    {
        uint64 closeTime = lm.getCurrentLedgerHeader().scpValue.closeTime;
        if (mEnvelope.tx.timeBounds->minTime > closeTime)
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "too-early"},
                          "transaction")
                .Mark();
            getResult().result.code(txTOO_EARLY);
            return false;
        }
        if (mEnvelope.tx.timeBounds->maxTime &&
            (mEnvelope.tx.timeBounds->maxTime < closeTime))
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "too-late"}, "transaction")
                .Mark();
            getResult().result.code(txTOO_LATE);
            return false;
        }
    }

    if (mEnvelope.tx.fee < getMinFee(lm))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-fee"},
                      "transaction")
            .Mark();
        getResult().result.code(txINSUFFICIENT_FEE);
        return false;
    }

    auto signingEntry = ledgerDelta
        ? ledgerDelta->loadAccount(getSourceID())
        : app.getLedgerEntries().load(accountKey(getSourceID()));
    if (!signingEntry)
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "no-account"}, "transaction")
            .Mark();
        getResult().result.code(txNO_ACCOUNT);
        return false;
    }

    auto signingFrame = AccountFrame{*signingEntry};
    auto signingAccount = SigningAccount{signingFrame};
    // when applying, the account's sequence number is updated when taking fees
    if (!applying)
    {
        if (current == 0)
        {
            current = signingFrame.getSeqNum();
        }

        if (current + 1 != mEnvelope.tx.seqNum)
        {
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "bad-seq"}, "transaction")
                .Mark();
            getResult().result.code(txBAD_SEQ);
            return false;
        }
    }

    if (!checkSignature(signatureChecker, signingAccount,
                        ThresholdLevel::LOW))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "bad-auth"}, "transaction")
            .Mark();
        getResult().result.code(txBAD_AUTH);
        return false;
    }

    // don't let the account go below the reserve
    if (signingFrame.getBalance() - mEnvelope.tx.fee <
        signingFrame.getMinimumBalance(app.getLedgerManager()))
    {
        app.getMetrics()
            .NewMeter({"transaction", "invalid", "insufficient-balance"},
                      "transaction")
            .Mark();
        getResult().result.code(txINSUFFICIENT_BALANCE);
        return false;
    }

    return true;
}

void
TransactionFrame::processFeeSeqNum(int ledgerVersion,
                                   LedgerDelta& ledgerDelta,
                                   LedgerEntries &entries)
{
    resetResults();

    auto signingEntry = ledgerDelta.loadAccount(getSourceID());
    if (!signingEntry)
    {
        throw std::runtime_error("Unexpected database state");
    }

    int64_t& fee = getResult().feeCharged;

    auto signingFrame = AccountFrame{*signingEntry};
    if (fee > 0)
    {
        fee = std::min(signingFrame.getBalance(), fee);
        signingFrame.addBalance(-fee);
        ledgerDelta.getHeader().feePool += fee;
    }
    if (signingFrame.getSeqNum() + 1 != mEnvelope.tx.seqNum)
    {
        // this should not happen as the transaction set is sanitized for
        // sequence numbers
        throw std::runtime_error("Unexpected account state");
    }
    signingFrame.setSeqNum(mEnvelope.tx.seqNum);
    ledgerDelta.updateEntry(signingFrame);
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    SignatureChecker& signatureChecker, LedgerDelta& ledgerDelta,
    Application& app)
{
    for (auto const& usedAccount : signatureChecker.usedOneTimeSignerKeys())
    {
        removeUsedOneTimeSignerKeys(usedAccount.first, usedAccount.second,
                                    ledgerDelta, app);
    }
}

void
TransactionFrame::removeUsedOneTimeSignerKeys(
    const AccountID& accountId, const std::set<SignerKey>& keys,
    LedgerDelta& ledgerDelta, Application& app) const
{
    auto account = ledgerDelta.loadAccount(accountId);
    if (!account)
    {
        return; // probably account was removed due to merge operation
    }

    auto frame = AccountFrame{*account};
    auto changed = std::accumulate(
        std::begin(keys), std::end(keys), false,
        [&](bool r, const SignerKey& signerKey) {
            return r || removeAccountSigner(frame, signerKey, app.getLedgerManager());
        });

    if (changed)
    {
        ledgerDelta.updateEntry(frame);
    }
}

bool
TransactionFrame::removeAccountSigner(AccountFrame& account,
                                      SignerKey const& signerKey,
                                      LedgerManager& ledgerManager) const
{
    auto signers = account.getSigners();
    auto it = std::find_if(
        std::begin(signers), std::end(signers),
        [&signerKey](Signer const& signer) { return signer.key == signerKey; });
    if (it != std::end(signers))
    {
        auto removed = account.addNumEntries(-1, ledgerManager);
        assert(removed);
        signers.erase(it);
        account.setSigners(signers);
        return true;
    }

    return false;
}

bool
TransactionFrame::checkValid(Application& app, SequenceNumber current)
{
    resetResults();
    SignatureChecker signatureChecker{
        app.getLedgerManager().getCurrentLedgerVersion() == 7,
        getContentsHash(), mEnvelope.signatures};
    bool res = commonValid(signatureChecker, app, nullptr, current);
    if (res)
    {
        for (auto& op : mOperations)
        {
            if (!op->checkValid(signatureChecker, app))
            {
                // it's OK to just fast fail here and not try to call
                // checkValid on all operations as the resulting object
                // is only used by applications
                app.getMetrics()
                    .NewMeter({"transaction", "invalid", "invalid-op"},
                              "transaction")
                    .Mark();
                markResultFailed();
                return false;
            }
        }

        if (!signatureChecker.checkAllSignaturesUsed())
        {
            res = false;
            getResult().result.code(txBAD_AUTH_EXTRA);
            app.getMetrics()
                .NewMeter({"transaction", "invalid", "bad-auth-extra"},
                          "transaction")
                .Mark();
        }
    }
    return res;
}

void
TransactionFrame::markResultFailed()
{
    // changing "code" causes the xdr struct to be deleted/re-created
    // As we want to preserve the results, we save them inside a temp object
    // Also, note that because we're using move operators
    // mOperations are still valid (they have pointers to the individual
    // results elements)
    xdr::xvector<OperationResult> t(std::move(getResult().result.results()));
    getResult().result.code(txFAILED);
    getResult().result.results() = std::move(t);

    // sanity check in case some implementations decide
    // to not implement std::move properly
    auto const& allResults = getResult().result.results();
    assert(allResults.size() == mOperations.size());
    for (size_t i = 0; i < mOperations.size(); i++)
    {
        assert(&mOperations[i]->getResult() == &allResults[i]);
    }
}

bool
TransactionFrame::apply(LedgerDelta& ledgerDelta, Application& app)
{
    TransactionMeta tm;
    return apply(ledgerDelta, tm, app);
}

bool
TransactionFrame::apply(LedgerDelta& ledgerDelta, TransactionMeta& meta,
                        Application& app)
{
    SignatureChecker signatureChecker{
        app.getLedgerManager().getCurrentLedgerVersion() == 7,
        getContentsHash(), mEnvelope.signatures};
    if (!commonValid(signatureChecker, app, &ledgerDelta, 0))
    {
        return false;
    }

    soci::transaction sqlTx(app.getDatabase().getSession());
    LedgerDeltaScope txDeltaScope{ledgerDelta};

    if (!applyOperations(signatureChecker, ledgerDelta, meta, app))
    {
        meta.operations().clear();
        markResultFailed();
        return false;
    }

    if (!signatureChecker.checkAllSignaturesUsed())
    {
        getResult().result.code(txBAD_AUTH_EXTRA);
        // this should never happen: malformed transaction should not be
        // accepted by nodes
        return false;
    }

    // if an error occurred, it is responsibility of account's owner to
    // remove that signer
    removeUsedOneTimeSignerKeys(signatureChecker, ledgerDelta,
                                app);
    sqlTx.commit();
    txDeltaScope.commit();

    return true;
}

bool
TransactionFrame::applyOperations(SignatureChecker& signatureChecker,
                                  LedgerDelta& ledgerDelta,
                                  TransactionMeta& meta,
                                  Application& app)
{
    auto& opTimer =
        app.getMetrics().NewTimer({"transaction", "op", "apply"});
    auto error = false;

    std::set<AccountID> paidFromAccounts;
    optional<AccountFrame> mergedAccountFrame;
    optional<AccountFrame> mergedToAccountFrame;
    auto anyPayment = false;
    auto wasRecreated = false;

    for (auto& op : mOperations)
    {
        auto time = opTimer.TimeScope();
        auto setSeqNum = false;
        LedgerDeltaScope opDeltaScope{ledgerDelta};

        if (triggerManageDataBug(app) && isManageData(*op))
        {
            meta.operations().clear();
            getResult().result.code(txINTERNAL_ERROR);
            return false;
        }

        if (allowMultipleMerges(app) && isMergeFrom(*op, mergedAccountFrame) && !wasRecreated)
        {
            if (multipleMergeReturnsMergeNoAccount(app))
            {
                op->getResult().code(opINNER);
                op->getResult().tr().type(ACCOUNT_MERGE);
                op->getResult().tr().accountMergeResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
                meta.operations().clear();
                markResultFailed();
                return false;
            }
            else
            {
                if (!ledgerDelta.entryExists(mergedAccountFrame->getKey()))
                {
                    ledgerDelta.addEntry(*mergedAccountFrame);
                }
            }
        }

        if (allowMultipleMerges(app) && isCreateFrom(*op, mergedAccountFrame))
        {
            if (isCreateDestination(*op, mergedToAccountFrame))
            {
                op->getResult().code(opINNER);
                op->getResult().tr().type(CREATE_ACCOUNT);
                op->getResult().tr().createAccountResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
                markResultFailed();

                meta.operations().emplace_back(ledgerDelta.top().getChanges());
                opDeltaScope.commit();
                error = true;
                continue;
            }
            else
            {
                meta.operations().clear();
                getResult().result.code(txINTERNAL_ERROR);
                return false;
            }
        }

        if (allowMergeBackBug(app) && isMergeFrom(*op, mergedToAccountFrame) && isMergeTo(*op, mergedAccountFrame))
        {
            auto sourceAccount = AccountFrame{*ledgerDelta.loadAccount(mergedAccountFrame->getAccountID())};
            setSeqNum = true;

            auto currentToAccount = make_optional<AccountFrame>(*ledgerDelta.loadAccount(mergedToAccountFrame->getAccountID()));
            mergedAccountFrame->setBalance(mergedAccountFrame->getBalance() + mergedToAccountFrame->getBalance() - currentToAccount->getBalance());
            ledgerDelta.updateEntry(*mergedAccountFrame);
        }

        if (allowMergePayBug(app) && isPaymentSource(*op, mergedAccountFrame) && isPaymentDestination(*op, mergedToAccountFrame) && !wasRecreated)
        {
            meta.operations().clear();
            getResult().result.code(txINTERNAL_ERROR);
            return false;
        }

        auto setMerge = false;
        if (isMerge(*op) && !mergedAccountFrame && (op->getSourceID() == getSourceID()))
        {
            mergedAccountFrame = make_optional<AccountFrame>(*ledgerDelta.loadAccount(op->getSourceID()));
            auto toAccountEntry = ledgerDelta.loadAccount(op->getOperation().body.destination());
            if (toAccountEntry)
            {
                mergedToAccountFrame = make_optional<AccountFrame>(*toAccountEntry);
            }
            setMerge = true;
        }

        if (allowCreateMergePayBug(app) && isPaymentSource(*op, mergedAccountFrame) && !wasRecreated)
        {
            if (isPaymentDestination(*op, mergedAccountFrame))
            {
                if (!ledgerDelta.entryExists(mergedAccountFrame->getKey()))
                {
                    ledgerDelta.addEntry(*mergedAccountFrame);
                }
            }
            else
            {
                meta.operations().clear();
                getResult().result.code(txINTERNAL_ERROR);
                return false;
            }
        }

        if (op->apply(signatureChecker, ledgerDelta, app))
        {
            if (isPayment(*op))
            {
                paidFromAccounts.insert(op->getSourceID());
                anyPayment = true;
            }

            if (allowMergeCreateMergeBug(app) && isCreateDestination(*op, mergedAccountFrame) && (!anyPayment || (paidFromAccounts.find(mergedAccountFrame->getAccountID()) != std::end(paidFromAccounts))))
            {
                auto sourceAccount = AccountFrame{*ledgerDelta.loadAccount(mergedAccountFrame->getAccountID())};
                sourceAccount.setBalance(mergedAccountFrame->getBalance());
                sourceAccount.setSeqNum(mergedAccountFrame->getSeqNum());
                ledgerDelta.updateEntry(sourceAccount);
            }

            if (isCreateDestination(*op, mergedAccountFrame))
            {
                wasRecreated = true;
            }

            if (allowPayMergeCreatePayBug(app) && isPaymentSource(*op, mergedAccountFrame) && isPaymentDestination(*op, mergedToAccountFrame))
            {
                auto sourceAccount = AccountFrame{*ledgerDelta.loadAccount(mergedAccountFrame->getAccountID())};
                sourceAccount.setBalance(mergedAccountFrame->getBalance() - op->getOperation().body.paymentOp().amount);
                sourceAccount.setSeqNum(mergedAccountFrame->getSeqNum());
                ledgerDelta.updateEntry(sourceAccount);
            }

            if (allowSelfXLMtoXLMPaymentBug(app) && isPathPaymentSelfXLMtoXLM(*op))
            {
                auto sourceAccount = AccountFrame{*ledgerDelta.loadAccount(op->getSourceID())};
                if (sourceAccount.getBalance() > op->getOperation().body.pathPaymentOp().destAmount)
                {
                    sourceAccount.setBalance(sourceAccount.getBalance() - op->getOperation().body.pathPaymentOp().destAmount);
                    ledgerDelta.updateEntry(sourceAccount);
                }
                else
                {
                    op->getResult().code(opINNER);
                    op->getResult().tr().type(PATH_PAYMENT);
                    op->getResult().tr().pathPaymentResult().code(PATH_PAYMENT_UNDERFUNDED);
                    markResultFailed();

                    meta.operations().emplace_back(ledgerDelta.top().getChanges());
                    opDeltaScope.commit();
                    error = true;
                    continue;
                }
            }

            if (setSeqNum)
            {
                auto sourceAccount = AccountFrame{*ledgerDelta.loadAccount(mergedAccountFrame->getAccountID())};
                sourceAccount.setSeqNum(ledgerDelta.getHeaderFrame().getStartingSequenceNumber());
                ledgerDelta.updateEntry(sourceAccount);
            }
        }
        else
        {
            error = true;

            if (setMerge)
            {
                mergedAccountFrame = nullopt<AccountFrame>();
                mergedToAccountFrame = nullopt<AccountFrame>();
            }
        }

        meta.operations().emplace_back(ledgerDelta.top().getChanges());
        opDeltaScope.commit();
    }

    if (!wasRecreated && mergedAccountFrame && ledgerDelta.entryExists(mergedAccountFrame->getKey()))
    {
        ledgerDelta.deleteEntry(mergedAccountFrame->getKey());
    }

    return !error;
}

StellarMessage
TransactionFrame::toStellarMessage() const
{
    StellarMessage msg;
    msg.type(TRANSACTION);
    msg.transaction() = mEnvelope;
    return msg;
}

void
TransactionFrame::storeTransaction(LedgerManager& ledgerManager,
                                   TransactionMeta& tm, int txindex,
                                   TransactionResultSet& resultSet) const
{
    auto txBytes(xdr::xdr_to_opaque(mEnvelope));

    resultSet.results.emplace_back(getResultPair());
    auto txResultBytes(xdr::xdr_to_opaque(resultSet.results.back()));

    std::string txBody;
    txBody = bn::encode_b64(txBytes);

    std::string txResult;
    txResult = bn::encode_b64(txResultBytes);

    xdr::opaque_vec<> txMeta(xdr::xdr_to_opaque(tm));

    std::string meta;
    meta = bn::encode_b64(txMeta);

    string txIDString(binToHex(getContentsHash()));

    auto& db = ledgerManager.getDatabase();
    auto prep = db.getPreparedStatement(
        "INSERT INTO txhistory "
        "( txid, ledgerseq, txindex,  txbody, txresult, txmeta) VALUES "
        "(:id,  :seq,      :txindex, :txb,   :txres,   :meta)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerManager.getCurrentLedgerHeader().ledgerSeq));
    st.exchange(soci::use(txindex));
    st.exchange(soci::use(txBody));
    st.exchange(soci::use(txResult));
    st.exchange(soci::use(meta));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("txhistory");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
TransactionFrame::storeTransactionFee(LedgerManager& ledgerManager,
                                      LedgerEntryChanges const& changes,
                                      int txindex) const
{
    xdr::opaque_vec<> txChanges(xdr::xdr_to_opaque(changes));

    std::string txChanges64;
    txChanges64 = bn::encode_b64(txChanges);

    string txIDString(binToHex(getContentsHash()));

    auto& db = ledgerManager.getDatabase();
    auto prep = db.getPreparedStatement(
        "INSERT INTO txfeehistory "
        "( txid, ledgerseq, txindex,  txchanges) VALUES "
        "(:id,  :seq,      :txindex, :txchanges)");

    auto& st = prep.statement();
    st.exchange(soci::use(txIDString));
    st.exchange(soci::use(ledgerManager.getCurrentLedgerHeader().ledgerSeq));
    st.exchange(soci::use(txindex));
    st.exchange(soci::use(txChanges64));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("txfeehistory");
        st.execute(true);
    }

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

static void
saveTransactionHelper(Database& db, soci::session& sess, uint32 ledgerSeq,
                      TxSetFrame& txSet, TransactionHistoryResultEntry& results,
                      XDROutputFileStream& txOut,
                      XDROutputFileStream& txResultOut)
{
    // prepare the txset for saving
    LedgerHeaderFrame::pointer lh =
        LedgerHeaderFrame::loadBySequence(ledgerSeq, db, sess);
    if (!lh)
    {
        throw std::runtime_error("Could not find ledger");
    }
    txSet.previousLedgerHash() = lh->mHeader.previousLedgerHash;
    txSet.sortForHash();
    TransactionHistoryEntry hist;
    hist.ledgerSeq = ledgerSeq;
    txSet.toXDR(hist.txSet);
    txOut.writeOne(hist);

    txResultOut.writeOne(results);
}

TransactionResultSet
TransactionFrame::getTransactionHistoryResults(Database& db, uint32 ledgerSeq)
{
    TransactionResultSet res;
    std::string txresult64;
    auto prep =
        db.getPreparedStatement("SELECT txresult FROM txhistory "
                                "WHERE ledgerseq = :lseq ORDER BY txindex ASC");
    auto& st = prep.statement();

    st.exchange(soci::use(ledgerSeq));
    st.exchange(soci::into(txresult64));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        std::vector<uint8_t> result;
        bn::decode_b64(txresult64, result);

        res.results.emplace_back();
        TransactionResultPair& p = res.results.back();

        xdr::xdr_get g(&result.front(), &result.back() + 1);
        xdr_argpack_archive(g, p);

        st.fetch();
    }
    return res;
}

std::vector<LedgerEntryChanges>
TransactionFrame::getTransactionFeeMeta(Database& db, uint32 ledgerSeq)
{
    std::vector<LedgerEntryChanges> res;
    std::string changes64;
    auto prep =
        db.getPreparedStatement("SELECT txchanges FROM txfeehistory "
                                "WHERE ledgerseq = :lseq ORDER BY txindex ASC");
    auto& st = prep.statement();

    st.exchange(soci::into(changes64));
    st.exchange(soci::use(ledgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        std::vector<uint8_t> changesRaw;
        bn::decode_b64(changes64, changesRaw);

        xdr::xdr_get g1(&changesRaw.front(), &changesRaw.back() + 1);
        res.emplace_back();
        xdr_argpack_archive(g1, res.back());

        st.fetch();
    }
    return res;
}

size_t
TransactionFrame::copyTransactionsToStream(Hash const& networkID, Database& db,
                                           soci::session& sess,
                                           uint32_t ledgerSeq,
                                           uint32_t ledgerCount,
                                           XDROutputFileStream& txOut,
                                           XDROutputFileStream& txResultOut)
{
    auto timer = db.getSelectTimer("txhistory");
    std::string txBody, txResult, txMeta;
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    TransactionEnvelope tx;
    uint32_t curLedgerSeq;

    assert(begin <= end);
    soci::statement st =
        (sess.prepare << "SELECT ledgerseq, txbody, txresult FROM txhistory "
                         "WHERE ledgerseq >= :begin AND ledgerseq < :end ORDER "
                         "BY ledgerseq ASC, txindex ASC",
         soci::into(curLedgerSeq), soci::into(txBody), soci::into(txResult),
         soci::use(begin), soci::use(end));

    Hash h;
    TxSetFrame txSet(h); // we're setting the hash later
    TransactionHistoryResultEntry results;

    st.execute(true);

    uint32_t lastLedgerSeq = curLedgerSeq;
    results.ledgerSeq = curLedgerSeq;

    while (st.got_data())
    {
        if (curLedgerSeq != lastLedgerSeq)
        {
            saveTransactionHelper(db, sess, lastLedgerSeq, txSet, results,
                                  txOut, txResultOut);
            // reset state
            txSet.mTransactions.clear();
            results.ledgerSeq = curLedgerSeq;
            results.txResultSet.results.clear();
            lastLedgerSeq = curLedgerSeq;
        }

        std::vector<uint8_t> body;
        bn::decode_b64(txBody, body);

        std::vector<uint8_t> result;
        bn::decode_b64(txResult, result);

        xdr::xdr_get g1(&body.front(), &body.back() + 1);
        xdr_argpack_archive(g1, tx);

        TransactionFramePtr txFrame =
            make_shared<TransactionFrame>(networkID, tx);
        txSet.add(txFrame);

        xdr::xdr_get g2(&result.front(), &result.back() + 1);
        results.txResultSet.results.emplace_back();

        TransactionResultPair& p = results.txResultSet.results.back();
        xdr_argpack_archive(g2, p);

        if (p.transactionHash != txFrame->getContentsHash())
        {
            throw std::runtime_error("transaction mismatch");
        }

        ++n;
        st.fetch();
    }
    if (n != 0)
    {
        saveTransactionHelper(db, sess, lastLedgerSeq, txSet, results, txOut,
                              txResultOut);
    }
    return n;
}

void
TransactionFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS txhistory";

    db.getSession() << "DROP TABLE IF EXISTS txfeehistory";

    db.getSession() << "CREATE TABLE txhistory ("
                       "txid        CHARACTER(64) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txindex     INT NOT NULL,"
                       "txbody      TEXT NOT NULL,"
                       "txresult    TEXT NOT NULL,"
                       "txmeta      TEXT NOT NULL,"
                       "PRIMARY KEY (ledgerseq, txindex)"
                       ")";
    db.getSession() << "CREATE INDEX histbyseq ON txhistory (ledgerseq);";

    db.getSession() << "CREATE TABLE txfeehistory ("
                       "txid        CHARACTER(64) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "txindex     INT NOT NULL,"
                       "txchanges   TEXT NOT NULL,"
                       "PRIMARY KEY (ledgerseq, txindex)"
                       ")";
    db.getSession() << "CREATE INDEX histfeebyseq ON txfeehistory (ledgerseq);";
}

void
TransactionFrame::deleteOldEntries(Database& db, uint32_t ledgerSeq)
{
    db.getSession() << "DELETE FROM txhistory WHERE ledgerseq <= " << ledgerSeq;
    db.getSession() << "DELETE FROM txfeehistory WHERE ledgerseq <= "
                    << ledgerSeq;
}
}
