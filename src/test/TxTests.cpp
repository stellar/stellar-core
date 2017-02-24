// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxTests.h"

#include "crypto/ByteSlice.h"
#include "ledger/DataFrame.h"
#include "ledger/LedgerDelta.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/types.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

bool
applyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app)
{
    // TODO: maybe we should just close ledger with tx instead of checking all
    // of
    // that manually?

    bool check = tx->checkValid(app, 0);
    TransactionResult checkResult = tx->getResult();

    REQUIRE((!check || checkResult.result.code() == txSUCCESS));

    bool doApply;
    // valid transaction sets ensure that
    {
        auto code = checkResult.result.code();
        if (code != txNO_ACCOUNT && code != txBAD_SEQ)
        {
            tx->processFeeSeqNum(delta, app.getLedgerManager());
        }
        doApply = (code != txBAD_SEQ);
    }

    bool res = false;

    if (doApply)
    {
        try
        {
            res = tx->apply(delta, app);
        }
        catch (...)
        {
            tx->getResult().result.code(txINTERNAL_ERROR);
        }

        REQUIRE((!res || tx->getResultCode() == txSUCCESS));

        if (!check)
        {
            REQUIRE(checkResult == tx->getResult());
        }
    }
    else
    {
        res = check;
    }

    // verify modified accounts invariants
    auto const& changes = delta.getChanges();
    for (auto const& c : changes)
    {
        switch (c.type())
        {
        case LEDGER_ENTRY_CREATED:
            checkEntry(c.created(), app);
            break;
        case LEDGER_ENTRY_UPDATED:
            checkEntry(c.updated(), app);
            break;
        default:
            break;
        }
    }

    // validates db state
    app.getLedgerManager().checkDbState();
    delta.checkAgainstDatabase(app);

    return res;
}

bool
throwingApplyCheck(TransactionFramePtr tx, LedgerDelta& delta, Application& app)
{
    auto r = applyCheck(tx, delta, app);
    switch (tx->getResultCode())
    {
    case txNO_ACCOUNT:
        throw ex_txNO_ACCOUNT{};
    case txINTERNAL_ERROR:
        throw ex_txINTERNAL_ERROR{};
    default:
        // ignore rest for now
        break;
    }
    return r;
}

void
checkEntry(LedgerEntry const& le, Application& app)
{
    auto& d = le.data;
    switch (d.type())
    {
    case ACCOUNT:
        checkAccount(d.account().accountID, app);
        break;
    case TRUSTLINE:
        checkAccount(d.trustLine().accountID, app);
        break;
    case OFFER:
        checkAccount(d.offer().sellerID, app);
        break;
    case DATA:
        checkAccount(d.data().accountID, app);
        break;
    default:
        break;
    }
}

void
checkAccount(AccountID const& id, Application& app)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(id, app.getDatabase());
    REQUIRE(!!res);
    std::vector<TrustFrame::pointer> retLines;
    TrustFrame::loadLines(id, retLines, app.getDatabase());

    std::vector<OfferFrame::pointer> retOffers;
    OfferFrame::loadOffers(id, retOffers, app.getDatabase());

    std::vector<DataFrame::pointer> retDatas;
    DataFrame::loadAccountsData(id, retDatas, app.getDatabase());

    size_t actualSubEntries = res->getAccount().signers.size() +
                              retLines.size() + retOffers.size() +
                              retDatas.size();

    REQUIRE(res->getAccount().numSubEntries == (uint32)actualSubEntries);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TransactionFramePtr tx)
{
    TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
        app.getLedgerManager().getLastClosedLedgerHeader().hash);

    if (tx)
    {
        txSet->add(tx);
        txSet->sortForHash();
    }

    return closeLedgerOn(app, ledgerSeq, day, month, year, txSet);
}

TxSetResultMeta
closeLedgerOn(Application& app, uint32 ledgerSeq, int day, int month, int year,
              TxSetFramePtr txSet)
{

    StellarValue sv(txSet->getContentsHash(), getTestDate(day, month, year),
                    emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData(ledgerSeq, txSet, sv);
    app.getLedgerManager().closeLedger(ledgerData);

    auto z1 = TransactionFrame::getTransactionHistoryResults(app.getDatabase(),
                                                             ledgerSeq);
    auto z2 =
        TransactionFrame::getTransactionFeeMeta(app.getDatabase(), ledgerSeq);

    REQUIRE(app.getLedgerManager().getLedgerNum() == (ledgerSeq + 1));

    TxSetResultMeta res;
    std::transform(z1.results.begin(), z1.results.end(), z2.begin(),
                   std::back_inserter(res), [](TransactionResultPair const& r1,
                                               LedgerEntryChanges const& r2) {
                       return std::make_pair(r1, r2);
                   });

    return res;
}

SecretKey
getRoot(Hash const& networkID)
{
    return SecretKey::fromSeed(networkID);
}

SecretKey
getAccount(const char* n)
{
    // stretch seed to 32 bytes
    std::string seed(n);
    while (seed.size() < 32)
        seed += '.';
    return SecretKey::fromSeed(seed);
}

AccountFrame::pointer
loadAccount(SecretKey const& k, Application& app, bool mustExist)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(k.getPublicKey(), app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

void
requireNoAccount(SecretKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}

OfferFrame::pointer
loadOffer(PublicKey const& k, uint64 offerID, Application& app, bool mustExist)
{
    OfferFrame::pointer res =
        OfferFrame::loadOffer(k, offerID, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

TrustFrame::pointer
loadTrustLine(SecretKey const& k, Asset const& asset, Application& app,
              bool mustExist)
{
    TrustFrame::pointer res =
        TrustFrame::loadTrustLine(k.getPublicKey(), asset, app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

SequenceNumber
getAccountSeqNum(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getSeqNum();
}

int64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getBalance();
}

xdr::xvector<Signer, 20>
getAccountSigners(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getAccount().signers;
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 100); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

TransactionFramePtr
transactionFromOperation(Hash const& networkID, SecretKey const& from,
                         SequenceNumber seq, Operation const& op)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = 100;
    e.tx.seqNum = seq;
    e.tx.operations.push_back(op);

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
transactionFromOperations(Hash const& networkID, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = ops.size() * 100;
    e.tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.tx.operations));

    TransactionFramePtr res =
        TransactionFrame::makeTransactionFromWire(networkID, e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
createChangeTrust(Hash const& networkID, SecretKey const& from,
                  PublicKey const& to, SequenceNumber seq,
                  std::string const& assetCode, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(op.body.changeTrustOp().line.alphaNum4().assetCode,
                   assetCode);
    op.body.changeTrustOp().line.alphaNum4().issuer = to;

    return transactionFromOperation(networkID, from, seq, op);
}

TransactionFramePtr
createAllowTrust(Hash const& networkID, SecretKey const& from,
                 PublicKey const& trustor, SequenceNumber seq,
                 std::string const& assetCode, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor;
    op.body.allowTrustOp().asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(op.body.allowTrustOp().asset.assetCode4(), assetCode);
    op.body.allowTrustOp().authorize = authorize;

    return transactionFromOperation(networkID, from, seq, op);
}

void
applyAllowTrust(Application& app, SecretKey const& from,
                PublicKey const& trustor, SequenceNumber seq,
                std::string const& assetCode, bool authorize)
{
    TransactionFramePtr txFrame;
    txFrame = createAllowTrust(app.getNetworkID(), from, trustor, seq,
                               assetCode, authorize);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto result = AllowTrustOpFrame::getInnerCode(
        txFrame->getResult().result.results()[0]);
    switch (result)
    {
    case ALLOW_TRUST_MALFORMED:
        throw ex_ALLOW_TRUST_MALFORMED{};
    case ALLOW_TRUST_NO_TRUST_LINE:
        throw ex_ALLOW_TRUST_NO_TRUST_LINE{};
    case ALLOW_TRUST_TRUST_NOT_REQUIRED:
        throw ex_ALLOW_TRUST_TRUST_NOT_REQUIRED{};
    case ALLOW_TRUST_CANT_REVOKE:
        throw ex_ALLOW_TRUST_CANT_REVOKE{};
    case ALLOW_TRUST_SELF_NOT_ALLOWED:
        throw ex_ALLOW_TRUST_SELF_NOT_ALLOWED{};
    default:
        break;
    }

    REQUIRE(AllowTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) ==
            ALLOW_TRUST_SUCCESS);
}

TransactionFramePtr
createCreateAccountTx(Hash const& networkID, SecretKey const& from,
                      SecretKey const& to, SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = to.getPublicKey();

    return transactionFromOperation(networkID, from, seq, op);
}

void
applyCreateAccountTx(Application& app, SecretKey const& from,
                     SecretKey const& to, SequenceNumber seq, int64_t amount)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createCreateAccountTx(app.getNetworkID(), from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto result =
        CreateAccountOpFrame::getInnerCode(txResult.result.results()[0]);

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (result != CREATE_ACCOUNT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccountAfter);
    }
    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    switch (result)
    {
    case CREATE_ACCOUNT_MALFORMED:
        throw ex_CREATE_ACCOUNT_MALFORMED{};
    case CREATE_ACCOUNT_UNDERFUNDED:
        throw ex_CREATE_ACCOUNT_UNDERFUNDED{};
    case CREATE_ACCOUNT_LOW_RESERVE:
        throw ex_CREATE_ACCOUNT_LOW_RESERVE{};
    case CREATE_ACCOUNT_ALREADY_EXIST:
        throw ex_CREATE_ACCOUNT_ALREADY_EXIST{};
    default:
        break;
    }

    REQUIRE(result == CREATE_ACCOUNT_SUCCESS);
}

Operation
createPaymentOp(SecretKey const* from, SecretKey const& to, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);

    if (from)
        op.sourceAccount.activate() = from->getPublicKey();

    return op;
}

TransactionFramePtr
createPaymentTx(Hash const& networkID, SecretKey const& from,
                SecretKey const& to, SequenceNumber seq, int64_t amount)
{
    return transactionFromOperation(networkID, from, seq,
                                    createPaymentOp(nullptr, to, amount));
}

void
applyPaymentTx(Application& app, SecretKey const& from, SecretKey const& to,
               SequenceNumber seq, int64_t amount)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createPaymentTx(app.getNetworkID(), from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto result = PaymentOpFrame::getInnerCode(txResult.result.results()[0]);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (result != PAYMENT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccount);
        REQUIRE(toAccountAfter);
    }

    switch (result)
    {
    case PAYMENT_MALFORMED:
        throw ex_PAYMENT_MALFORMED{};
    case PAYMENT_UNDERFUNDED:
        throw ex_PAYMENT_UNDERFUNDED{};
    case PAYMENT_SRC_NO_TRUST:
        throw ex_PAYMENT_SRC_NO_TRUST{};
    case PAYMENT_SRC_NOT_AUTHORIZED:
        throw ex_PAYMENT_SRC_NOT_AUTHORIZED{};
    case PAYMENT_NO_DESTINATION:
        throw ex_PAYMENT_NO_DESTINATION{};
    case PAYMENT_NO_TRUST:
        throw ex_PAYMENT_NO_TRUST{};
    case PAYMENT_NOT_AUTHORIZED:
        throw ex_PAYMENT_NOT_AUTHORIZED{};
    case PAYMENT_LINE_FULL:
        throw ex_PAYMENT_LINE_FULL{};
    case PAYMENT_NO_ISSUER:
        throw ex_PAYMENT_NO_ISSUER{};
    default:
        break;
    }

    REQUIRE(result == PAYMENT_SUCCESS);
}

void
applyChangeTrust(Application& app, SecretKey const& from, PublicKey const& to,
                 SequenceNumber seq, std::string const& assetCode,
                 int64_t limit)
{
    TransactionFramePtr txFrame;

    txFrame =
        createChangeTrust(app.getNetworkID(), from, to, seq, assetCode, limit);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto result = ChangeTrustOpFrame::getInnerCode(
        txFrame->getResult().result.results()[0]);
    switch (result)
    {
    case CHANGE_TRUST_MALFORMED:
        throw ex_CHANGE_TRUST_MALFORMED{};
    case CHANGE_TRUST_NO_ISSUER:
        throw ex_CHANGE_TRUST_NO_ISSUER{};
    case CHANGE_TRUST_INVALID_LIMIT:
        throw ex_CHANGE_TRUST_INVALID_LIMIT{};
    case CHANGE_TRUST_LOW_RESERVE:
        throw ex_CHANGE_TRUST_LOW_RESERVE{};
    case CHANGE_TRUST_SELF_NOT_ALLOWED:
        throw ex_CHANGE_TRUST_SELF_NOT_ALLOWED{};
    default:
        break;
    }

    REQUIRE(result == CHANGE_TRUST_SUCCESS);
}

TransactionFramePtr
createCreditPaymentTx(Hash const& networkID, SecretKey const& from,
                      PublicKey const& to, Asset const& asset,
                      SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().asset = asset;
    op.body.paymentOp().destination = to;

    return transactionFromOperation(networkID, from, seq, op);
}

Asset
makeAsset(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    asset.alphaNum4().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum4().assetCode, code);
    return asset;
}

void
applyCreditPaymentTx(Application& app, SecretKey const& from,
                     PublicKey const& to, Asset const& ci, SequenceNumber seq,
                     int64_t amount)
{
    TransactionFramePtr txFrame;

    txFrame =
        createCreditPaymentTx(app.getNetworkID(), from, to, ci, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    auto res = firstResult.tr().paymentResult();
    auto result = res.code();

    switch (result)
    {
    case PAYMENT_MALFORMED:
        throw ex_PAYMENT_MALFORMED{};
    case PAYMENT_UNDERFUNDED:
        throw ex_PAYMENT_UNDERFUNDED{};
    case PAYMENT_SRC_NO_TRUST:
        throw ex_PAYMENT_SRC_NO_TRUST{};
    case PAYMENT_SRC_NOT_AUTHORIZED:
        throw ex_PAYMENT_SRC_NOT_AUTHORIZED{};
    case PAYMENT_NO_DESTINATION:
        throw ex_PAYMENT_NO_DESTINATION{};
    case PAYMENT_NO_TRUST:
        throw ex_PAYMENT_NO_TRUST{};
    case PAYMENT_NOT_AUTHORIZED:
        throw ex_PAYMENT_NOT_AUTHORIZED{};
    case PAYMENT_LINE_FULL:
        throw ex_PAYMENT_LINE_FULL{};
    case PAYMENT_NO_ISSUER:
        throw ex_PAYMENT_NO_ISSUER{};
    default:
        break;
    }

    REQUIRE(result == PAYMENT_SUCCESS);
}

TransactionFramePtr
createPathPaymentTx(Hash const& networkID, SecretKey const& from,
                    PublicKey const& to, Asset const& sendCur, int64_t sendMax,
                    Asset const& destCur, int64_t destAmount,
                    SequenceNumber seq, std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to;
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return transactionFromOperation(networkID, from, seq, op);
}

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey const& from, PublicKey const& to,
                   Asset const& sendCur, int64_t sendMax, Asset const& destCur,
                   int64_t destAmount, SequenceNumber seq,
                   std::vector<Asset> const& path, Asset* noIssuer)
{
    TransactionFramePtr txFrame;

    txFrame = createPathPaymentTx(app.getNetworkID(), from, to, sendCur,
                                  sendMax, destCur, destAmount, seq, path);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    auto res = firstResult.tr().pathPaymentResult();
    auto result = res.code();

    if (result != PATH_PAYMENT_NO_ISSUER)
    {
        REQUIRE(!noIssuer);
    }

    switch (result)
    {
    case PATH_PAYMENT_MALFORMED:
        throw ex_PATH_PAYMENT_MALFORMED{};
    case PATH_PAYMENT_UNDERFUNDED:
        throw ex_PATH_PAYMENT_UNDERFUNDED{};
    case PATH_PAYMENT_SRC_NO_TRUST:
        throw ex_PATH_PAYMENT_SRC_NO_TRUST{};
    case PATH_PAYMENT_SRC_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_SRC_NOT_AUTHORIZED{};
    case PATH_PAYMENT_NO_DESTINATION:
        throw ex_PATH_PAYMENT_NO_DESTINATION{};
    case PATH_PAYMENT_NO_TRUST:
        throw ex_PATH_PAYMENT_NO_TRUST{};
    case PATH_PAYMENT_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_NOT_AUTHORIZED{};
    case PATH_PAYMENT_LINE_FULL:
        throw ex_PATH_PAYMENT_LINE_FULL{};
    case PATH_PAYMENT_NO_ISSUER:
        REQUIRE(noIssuer);
        REQUIRE(*noIssuer == res.noIssuer());
        throw ex_PATH_PAYMENT_NO_ISSUER{};
    case PATH_PAYMENT_TOO_FEW_OFFERS:
        throw ex_PATH_PAYMENT_TOO_FEW_OFFERS{};
    case PATH_PAYMENT_OFFER_CROSS_SELF:
        throw ex_PATH_PAYMENT_OFFER_CROSS_SELF{};
    case PATH_PAYMENT_OVER_SENDMAX:
        throw ex_PATH_PAYMENT_OVER_SENDMAX{};
    default:
        break;
    }

    REQUIRE(result == PATH_PAYMENT_SUCCESS);
    return res;
}

TransactionFramePtr
createPassiveOfferOp(Hash const& networkID, SecretKey const& source,
                     Asset const& selling, Asset const& buying,
                     Price const& price, int64_t amount, SequenceNumber seq)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_OFFER);
    op.body.createPassiveOfferOp().amount = amount;
    op.body.createPassiveOfferOp().selling = selling;
    op.body.createPassiveOfferOp().buying = buying;
    op.body.createPassiveOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

TransactionFramePtr
manageOfferOp(Hash const& networkID, uint64 offerId, SecretKey const& source,
              Asset const& selling, Asset const& buying, Price const& price,
              int64_t amount, SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_OFFER);
    op.body.manageOfferOp().amount = amount;
    op.body.manageOfferOp().selling = selling;
    op.body.manageOfferOp().buying = buying;
    op.body.manageOfferOp().offerID = offerId;
    op.body.manageOfferOp().price = price;

    return transactionFromOperation(networkID, source, seq, op);
}

static ManageOfferResult
applyCreateOfferHelper(Application& app, uint64 offerId,
                       SecretKey const& source, Asset const& selling,
                       Asset const& buying, Price const& price, int64_t amount,
                       SequenceNumber seq)
{
    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    auto lastGeneratedID = delta.getHeaderFrame().getLastGeneratedID();
    auto expectedOfferID = lastGeneratedID + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    TransactionFramePtr txFrame;

    txFrame = manageOfferOp(app.getNetworkID(), offerId, source, selling,
                            buying, price, amount, seq);

    try
    {
        throwingApplyCheck(txFrame, delta, app);
    }
    catch (...)
    {
        REQUIRE(delta.getHeaderFrame().getLastGeneratedID() == lastGeneratedID);
        throw;
    }

    checkTransaction(*txFrame);
    delta.commit();

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageOfferResult = results[0].tr().manageOfferResult();

    if (manageOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        OfferFrame::pointer offer;

        auto& offerResult = manageOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            offer =
                loadOffer(source.getPublicKey(), expectedOfferID, app, true);
            auto& offerEntry = offer->getOffer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
        }
        break;
        case MANAGE_OFFER_DELETED:
            REQUIRE(
                !loadOffer(source.getPublicKey(), expectedOfferID, app, false));
            break;
        default:
            abort();
        }
    }
    else
    {
        REQUIRE(delta.getHeaderFrame().getLastGeneratedID() == lastGeneratedID);
    }

    return manageOfferResult;
}

uint64_t
applyManageOffer(Application& app, uint64 offerId, SecretKey const& source,
                 Asset const& selling, Asset const& buying, Price const& price,
                 int64_t amount, SequenceNumber seq,
                 ManageOfferEffect expectedEffect)
{
    ManageOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, offerId, source, selling, buying, price, amount, seq);

    switch (createOfferRes.code())
    {
    case MANAGE_OFFER_MALFORMED:
        throw ex_MANAGE_OFFER_MALFORMED{};
    case MANAGE_OFFER_SELL_NO_TRUST:
        throw ex_MANAGE_OFFER_SELL_NO_TRUST{};
    case MANAGE_OFFER_BUY_NO_TRUST:
        throw ex_MANAGE_OFFER_BUY_NO_TRUST{};
    case MANAGE_OFFER_SELL_NOT_AUTHORIZED:
        throw ex_MANAGE_OFFER_SELL_NOT_AUTHORIZED{};
    case MANAGE_OFFER_BUY_NOT_AUTHORIZED:
        throw ex_MANAGE_OFFER_BUY_NOT_AUTHORIZED{};
    case MANAGE_OFFER_LINE_FULL:
        throw ex_MANAGE_OFFER_LINE_FULL{};
    case MANAGE_OFFER_UNDERFUNDED:
        throw ex_MANAGE_OFFER_UNDERFUNDED{};
    case MANAGE_OFFER_CROSS_SELF:
        throw ex_MANAGE_OFFER_CROSS_SELF{};
    case MANAGE_OFFER_SELL_NO_ISSUER:
        throw ex_MANAGE_OFFER_SELL_NO_ISSUER{};
    case MANAGE_OFFER_BUY_NO_ISSUER:
        throw ex_MANAGE_OFFER_BUY_NO_ISSUER{};
    case MANAGE_OFFER_NOT_FOUND:
        throw ex_MANAGE_OFFER_NOT_FOUND{};
    case MANAGE_OFFER_LOW_RESERVE:
        throw ex_MANAGE_OFFER_LOW_RESERVE{};
    default:
        break;
    }

    REQUIRE(createOfferRes.code() == MANAGE_OFFER_SUCCESS);

    auto& success = createOfferRes.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

uint64_t
applyCreatePassiveOffer(Application& app, SecretKey const& source,
                        Asset const& selling, Asset const& buying,
                        Price const& price, int64_t amount, SequenceNumber seq,
                        ManageOfferEffect expectedEffect)
{
    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    auto lastGeneratedID = delta.getHeaderFrame().getLastGeneratedID();
    auto expectedOfferID = lastGeneratedID + 1;

    TransactionFramePtr txFrame;
    txFrame = createPassiveOfferOp(app.getNetworkID(), source, selling, buying,
                                   price, amount, seq);

    try
    {
        throwingApplyCheck(txFrame, delta, app);
    }
    catch (...)
    {
        REQUIRE(delta.getHeaderFrame().getLastGeneratedID() == lastGeneratedID);
        throw;
    }

    checkTransaction(*txFrame);
    delta.commit();

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createPassiveOfferResult = results[0].tr().manageOfferResult();

    if (createPassiveOfferResult.code() == MANAGE_OFFER_SUCCESS)
    {
        OfferFrame::pointer offer;

        auto& offerResult = createPassiveOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            offer =
                loadOffer(source.getPublicKey(), expectedOfferID, app, true);
            auto& offerEntry = offer->getOffer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
            REQUIRE(
                !loadOffer(source.getPublicKey(), expectedOfferID, app, false));
            break;
        default:
            abort();
        }
    }

    if (createPassiveOfferResult.code() != MANAGE_OFFER_SUCCESS)
    {
        REQUIRE(delta.getHeaderFrame().getLastGeneratedID() == lastGeneratedID);
    }

    switch (createPassiveOfferResult.code())
    {
    case MANAGE_OFFER_MALFORMED:
        throw ex_MANAGE_OFFER_MALFORMED{};
    case MANAGE_OFFER_SELL_NO_TRUST:
        throw ex_MANAGE_OFFER_SELL_NO_TRUST{};
    case MANAGE_OFFER_BUY_NO_TRUST:
        throw ex_MANAGE_OFFER_BUY_NO_TRUST{};
    case MANAGE_OFFER_SELL_NOT_AUTHORIZED:
        throw ex_MANAGE_OFFER_SELL_NOT_AUTHORIZED{};
    case MANAGE_OFFER_BUY_NOT_AUTHORIZED:
        throw ex_MANAGE_OFFER_BUY_NOT_AUTHORIZED{};
    case MANAGE_OFFER_LINE_FULL:
        throw ex_MANAGE_OFFER_LINE_FULL{};
    case MANAGE_OFFER_UNDERFUNDED:
        throw ex_MANAGE_OFFER_UNDERFUNDED{};
    case MANAGE_OFFER_CROSS_SELF:
        throw ex_MANAGE_OFFER_CROSS_SELF{};
    case MANAGE_OFFER_SELL_NO_ISSUER:
        throw ex_MANAGE_OFFER_SELL_NO_ISSUER{};
    case MANAGE_OFFER_BUY_NO_ISSUER:
        throw ex_MANAGE_OFFER_BUY_NO_ISSUER{};
    case MANAGE_OFFER_NOT_FOUND:
        throw ex_MANAGE_OFFER_NOT_FOUND{};
    case MANAGE_OFFER_LOW_RESERVE:
        throw ex_MANAGE_OFFER_LOW_RESERVE{};
    default:
        break;
    }

    REQUIRE(createPassiveOfferResult.code() == MANAGE_OFFER_SUCCESS);

    auto& success = createPassiveOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
}

TransactionFramePtr
createSetOptions(Hash const& networkID, SecretKey const& source,
                 SequenceNumber seq, AccountID* inflationDest,
                 uint32_t* setFlags, uint32_t* clearFlags,
                 ThresholdSetter* thrs, Signer* signer, std::string* homeDomain)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (inflationDest)
    {
        setOp.inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        setOp.setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        setOp.clearFlags.activate() = *clearFlags;
    }

    if (thrs)
    {
        if (thrs->masterWeight)
        {
            setOp.masterWeight.activate() = *thrs->masterWeight;
        }
        if (thrs->lowThreshold)
        {
            setOp.lowThreshold.activate() = *thrs->lowThreshold;
        }
        if (thrs->medThreshold)
        {
            setOp.medThreshold.activate() = *thrs->medThreshold;
        }
        if (thrs->highThreshold)
        {
            setOp.highThreshold.activate() = *thrs->highThreshold;
        }
    }

    if (signer)
    {
        setOp.signer.activate() = *signer;
    }

    if (homeDomain)
    {
        setOp.homeDomain.activate() = *homeDomain;
    }

    return transactionFromOperation(networkID, source, seq, op);
}

void
applySetOptions(Application& app, SecretKey const& source, SequenceNumber seq,
                AccountID* inflationDest, uint32_t* setFlags,
                uint32_t* clearFlags, ThresholdSetter* thrs, Signer* signer,
                std::string* homeDomain)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(app.getNetworkID(), source, seq, inflationDest,
                               setFlags, clearFlags, thrs, signer, homeDomain);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    auto result = SetOptionsOpFrame::getInnerCode(
        txFrame->getResult().result.results()[0]);

    switch (result)
    {
    case SET_OPTIONS_LOW_RESERVE:
        throw ex_SET_OPTIONS_LOW_RESERVE{};
    case SET_OPTIONS_TOO_MANY_SIGNERS:
        throw ex_SET_OPTIONS_TOO_MANY_SIGNERS{};
    case SET_OPTIONS_BAD_FLAGS:
        throw ex_SET_OPTIONS_BAD_FLAGS{};
    case SET_OPTIONS_INVALID_INFLATION:
        throw ex_SET_OPTIONS_INVALID_INFLATION{};
    case SET_OPTIONS_CANT_CHANGE:
        throw ex_SET_OPTIONS_CANT_CHANGE{};
    case SET_OPTIONS_UNKNOWN_FLAG:
        throw ex_SET_OPTIONS_UNKNOWN_FLAG{};
    case SET_OPTIONS_THRESHOLD_OUT_OF_RANGE:
        throw ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE{};
    case SET_OPTIONS_BAD_SIGNER:
        throw ex_SET_OPTIONS_BAD_SIGNER{};
    case SET_OPTIONS_INVALID_HOME_DOMAIN:
        throw ex_SET_OPTIONS_INVALID_HOME_DOMAIN{};
    default:
        break;
    }

    REQUIRE(SET_OPTIONS_SUCCESS == result);
}

TransactionFramePtr
createInflation(Hash const& networkID, SecretKey const& from,
                SequenceNumber seq)
{
    Operation op;
    op.body.type(INFLATION);

    return transactionFromOperation(networkID, from, seq, op);
}

OperationResult
applyInflation(Application& app, SecretKey const& from, SequenceNumber seq,
               InflationResultCode result)
{
    TransactionFramePtr txFrame =
        createInflation(app.getNetworkID(), from, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    bool res = applyCheck(txFrame, delta, app);

    checkTransaction(*txFrame);
    REQUIRE(InflationOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
    if (res)
    {
        delta.commit();
    }
    return getFirstResult(*txFrame);
}

TransactionFramePtr
createAccountMerge(Hash const& networkID, SecretKey const& source,
                   PublicKey const& dest, SequenceNumber seq)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = dest;

    return transactionFromOperation(networkID, source, seq, op);
}

void
applyAccountMerge(Application& app, SecretKey const& source,
                  PublicKey const& dest, SequenceNumber seq)
{
    TransactionFramePtr txFrame =
        createAccountMerge(app.getNetworkID(), source, dest, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    auto result =
        MergeOpFrame::getInnerCode(txFrame->getResult().result.results()[0]);
    switch (result)
    {
    case ACCOUNT_MERGE_MALFORMED:
        throw ex_ACCOUNT_MERGE_MALFORMED{};
    case ACCOUNT_MERGE_NO_ACCOUNT:
        throw ex_ACCOUNT_MERGE_NO_ACCOUNT{};
    case ACCOUNT_MERGE_IMMUTABLE_SET:
        throw ex_ACCOUNT_MERGE_IMMUTABLE_SET{};
    case ACCOUNT_MERGE_HAS_SUB_ENTRIES:
        throw ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES{};
    default:
        break;
    }

    REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
}

TransactionFramePtr
createManageData(Hash const& networkID, SecretKey const& source,
                 std::string const& name, DataValue* value, SequenceNumber seq)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if (value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return transactionFromOperation(networkID, source, seq, op);
}

void
applyManageData(Application& app, SecretKey const& source,
                std::string const& name, DataValue* value, SequenceNumber seq)
{
    TransactionFramePtr txFrame =
        createManageData(app.getNetworkID(), source, name, value, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    throwingApplyCheck(txFrame, delta, app);

    auto result = ManageDataOpFrame::getInnerCode(
        txFrame->getResult().result.results()[0]);
    switch (result)
    {
    case MANAGE_DATA_NOT_SUPPORTED_YET:
        throw ex_MANAGE_DATA_NOT_SUPPORTED_YET{};
    case MANAGE_DATA_NAME_NOT_FOUND:
        throw ex_MANAGE_DATA_NAME_NOT_FOUND{};
    case MANAGE_DATA_LOW_RESERVE:
        throw ex_MANAGE_DATA_LOW_RESERVE{};
    case MANAGE_DATA_INVALID_NAME:
        throw ex_MANAGE_DATA_INVALID_NAME{};
    default:
        break;
    }

    auto dataFrame =
        DataFrame::loadData(source.getPublicKey(), name, app.getDatabase());
    if (value)
    {
        REQUIRE(dataFrame != nullptr);
        REQUIRE(dataFrame->getData().dataValue == *value);
    }
    else
    {
        REQUIRE(dataFrame == nullptr);
    }
}

OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

Operation&
getFirstOperation(TransactionFrame& tx)
{
    return tx.getEnvelope().tx.operations[0];
}

void
reSignTransaction(TransactionFrame& tx, SecretKey const& source)
{
    tx.getEnvelope().signatures.clear();
    tx.addSignature(source);
}

void
checkAmounts(int64_t a, int64_t b, int64_t maxd)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
}

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected)
{
    REQUIRE(r[index].first.result.result.code() == expected);
};

void
checkTx(int index, TxSetResultMeta& r, TransactionResultCode expected,
        OperationResultCode code)
{
    checkTx(index, r, expected);
    REQUIRE(r[index].first.result.result.results()[0].code() == code);
};
}
}
