// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test-common/TestTxUtils.h"
#include "crypto/SignerKey.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "transactions/TransactionUtils.h"
#include "util/types.h"

namespace stellar
{
namespace txtest
{
SecretKey
getRoot(Hash const& networkID)
{
    return SecretKey::fromSeed(networkID);
}

SecretKey
getAccount(std::string const& n)
{
    // stretch seed to 32 bytes
    std::string seed(n);
    while (seed.size() < 32)
        seed += '.';
    return SecretKey::fromSeed(seed);
}

Signer
makeSigner(SecretKey key, int weight)
{
    return Signer{KeyUtils::convertKey<SignerKey>(key.getPublicKey()), weight};
}

Operation
changeTrust(Asset const& asset, int64_t limit)
{
    return changeTrust(assetToChangeTrustAsset(asset), limit);
}

Operation
changeTrust(ChangeTrustAsset const& asset, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line = asset;

    return op;
}

Operation
allowTrust(PublicKey const& trustor, Asset const& asset, uint32_t authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor;
    op.body.allowTrustOp().asset.type(asset.type());

    if (asset.type() == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        op.body.allowTrustOp().asset.assetCode4() = asset.alphaNum4().assetCode;
    }
    else
    {
        op.body.allowTrustOp().asset.assetCode12() =
            asset.alphaNum12().assetCode;
    }

    op.body.allowTrustOp().authorize = authorize;

    return op;
}

Operation
createAccount(PublicKey const& dest, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = dest;
    return op;
}

Operation
payment(PublicKey const& to, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = toMuxedAccount(to);
    op.body.paymentOp().asset.type(ASSET_TYPE_NATIVE);
    return op;
}

Operation
payment(PublicKey const& to, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = toMuxedAccount(to);
    op.body.paymentOp().asset = asset;
    return op;
}

Asset
makeNativeAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_NATIVE);
    return asset;
}

Asset
makeInvalidAsset()
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    return asset;
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

Asset
makeAssetAlphanum12(SecretKey const& issuer, std::string const& code)
{
    Asset asset;
    asset.type(ASSET_TYPE_CREDIT_ALPHANUM12);
    asset.alphaNum12().issuer = issuer.getPublicKey();
    strToAssetCode(asset.alphaNum12().assetCode, code);
    return asset;
}

Operation
pathPayment(PublicKey const& to, Asset const& sendCur, int64_t sendMax,
            Asset const& destCur, int64_t destAmount,
            std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT_STRICT_RECEIVE);
    PathPaymentStrictReceiveOp& ppop = op.body.pathPaymentStrictReceiveOp();
    ppop.sendAsset = sendCur;
    ppop.sendMax = sendMax;
    ppop.destAsset = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = toMuxedAccount(to);
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
pathPaymentStrictSend(PublicKey const& to, Asset const& sendCur,
                      int64_t sendAmount, Asset const& destCur, int64_t destMin,
                      std::vector<Asset> const& path)
{
    Operation op;
    op.body.type(PATH_PAYMENT_STRICT_SEND);
    PathPaymentStrictSendOp& ppop = op.body.pathPaymentStrictSendOp();
    ppop.sendAsset = sendCur;
    ppop.sendAmount = sendAmount;
    ppop.destAsset = destCur;
    ppop.destMin = destMin;
    ppop.destination = toMuxedAccount(to);
    std::copy(std::begin(path), std::end(path), std::back_inserter(ppop.path));

    return op;
}

Operation
createPassiveOffer(Asset const& selling, Asset const& buying,
                   Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(CREATE_PASSIVE_SELL_OFFER);
    op.body.createPassiveSellOfferOp().amount = amount;
    op.body.createPassiveSellOfferOp().selling = selling;
    op.body.createPassiveSellOfferOp().buying = buying;
    op.body.createPassiveSellOfferOp().price = price;

    return op;
}

Operation
manageOffer(int64 offerId, Asset const& selling, Asset const& buying,
            Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_SELL_OFFER);
    op.body.manageSellOfferOp().amount = amount;
    op.body.manageSellOfferOp().selling = selling;
    op.body.manageSellOfferOp().buying = buying;
    op.body.manageSellOfferOp().offerID = offerId;
    op.body.manageSellOfferOp().price = price;

    return op;
}

Operation
manageBuyOffer(int64 offerId, Asset const& selling, Asset const& buying,
               Price const& price, int64_t amount)
{
    Operation op;
    op.body.type(MANAGE_BUY_OFFER);
    op.body.manageBuyOfferOp().buyAmount = amount;
    op.body.manageBuyOfferOp().selling = selling;
    op.body.manageBuyOfferOp().buying = buying;
    op.body.manageBuyOfferOp().offerID = offerId;
    op.body.manageBuyOfferOp().price = price;

    return op;
}

SetOptionsArguments
operator|(SetOptionsArguments const& x, SetOptionsArguments const& y)
{
    auto result = SetOptionsArguments{};
    result.masterWeight = y.masterWeight ? y.masterWeight : x.masterWeight;
    result.lowThreshold = y.lowThreshold ? y.lowThreshold : x.lowThreshold;
    result.medThreshold = y.medThreshold ? y.medThreshold : x.medThreshold;
    result.highThreshold = y.highThreshold ? y.highThreshold : x.highThreshold;
    result.signer = y.signer ? y.signer : x.signer;
    result.setFlags = y.setFlags ? y.setFlags : x.setFlags;
    result.clearFlags = y.clearFlags ? y.clearFlags : x.clearFlags;
    result.inflationDest = y.inflationDest ? y.inflationDest : x.inflationDest;
    result.homeDomain = y.homeDomain ? y.homeDomain : x.homeDomain;
    return result;
}

Operation
setOptions(SetOptionsArguments const& arguments)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    SetOptionsOp& setOp = op.body.setOptionsOp();

    if (arguments.inflationDest)
    {
        setOp.inflationDest.activate() = *arguments.inflationDest;
    }

    if (arguments.setFlags)
    {
        setOp.setFlags.activate() = *arguments.setFlags;
    }

    if (arguments.clearFlags)
    {
        setOp.clearFlags.activate() = *arguments.clearFlags;
    }

    if (arguments.masterWeight)
    {
        setOp.masterWeight.activate() = *arguments.masterWeight;
    }
    if (arguments.lowThreshold)
    {
        setOp.lowThreshold.activate() = *arguments.lowThreshold;
    }
    if (arguments.medThreshold)
    {
        setOp.medThreshold.activate() = *arguments.medThreshold;
    }
    if (arguments.highThreshold)
    {
        setOp.highThreshold.activate() = *arguments.highThreshold;
    }

    if (arguments.signer)
    {
        setOp.signer.activate() = *arguments.signer;
    }

    if (arguments.homeDomain)
    {
        setOp.homeDomain.activate() = *arguments.homeDomain;
    }

    return op;
}

SetOptionsArguments
setMasterWeight(int master)
{
    SetOptionsArguments result;
    result.masterWeight = std::make_optional<int>(master);
    return result;
}

SetOptionsArguments
setLowThreshold(int low)
{
    SetOptionsArguments result;
    result.lowThreshold = std::make_optional<int>(low);
    return result;
}

SetOptionsArguments
setMedThreshold(int med)
{
    SetOptionsArguments result;
    result.medThreshold = std::make_optional<int>(med);
    return result;
}

SetOptionsArguments
setHighThreshold(int high)
{
    SetOptionsArguments result;
    result.highThreshold = std::make_optional<int>(high);
    return result;
}

SetOptionsArguments
setSigner(Signer signer)
{
    SetOptionsArguments result;
    result.signer = std::make_optional<Signer>(signer);
    return result;
}

SetOptionsArguments
setFlags(uint32_t setFlags)
{
    SetOptionsArguments result;
    result.setFlags = std::make_optional<uint32_t>(setFlags);
    return result;
}

SetOptionsArguments
clearFlags(uint32_t clearFlags)
{
    SetOptionsArguments result;
    result.clearFlags = std::make_optional<uint32_t>(clearFlags);
    return result;
}

SetOptionsArguments
setInflationDestination(AccountID inflationDest)
{
    SetOptionsArguments result;
    result.inflationDest = std::make_optional<AccountID>(inflationDest);
    return result;
}

SetOptionsArguments
setHomeDomain(std::string const& homeDomain)
{
    SetOptionsArguments result;
    result.homeDomain = std::make_optional<std::string>(homeDomain);
    return result;
}

SetTrustLineFlagsArguments
operator|(SetTrustLineFlagsArguments const& x,
          SetTrustLineFlagsArguments const& y)
{
    auto result = SetTrustLineFlagsArguments{};
    result.setFlags = y.setFlags | x.setFlags;
    result.clearFlags = y.clearFlags | x.clearFlags;
    return result;
}

Operation
setTrustLineFlags(PublicKey const& trustor, Asset const& asset,
                  SetTrustLineFlagsArguments const& arguments)
{
    Operation op;
    op.body.type(SET_TRUST_LINE_FLAGS);

    SetTrustLineFlagsOp& setOp = op.body.setTrustLineFlagsOp();
    setOp.trustor = trustor;
    setOp.asset = asset;
    setOp.setFlags = arguments.setFlags;
    setOp.clearFlags = arguments.clearFlags;

    return op;
}

SetTrustLineFlagsArguments
setTrustLineFlags(uint32_t setFlags)
{
    SetTrustLineFlagsArguments result;
    result.setFlags = setFlags;
    return result;
}

SetTrustLineFlagsArguments
clearTrustLineFlags(uint32_t clearFlags)
{
    SetTrustLineFlagsArguments result;
    result.clearFlags = clearFlags;
    return result;
}

Operation
inflation()
{
    Operation op;
    op.body.type(INFLATION);

    return op;
}

Operation
accountMerge(PublicKey const& dest)
{
    Operation op;
    op.body.type(ACCOUNT_MERGE);
    op.body.destination() = toMuxedAccount(dest);
    return op;
}

Operation
manageData(std::string const& name, DataValue* value)
{
    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp().dataName = name;
    if (value)
        op.body.manageDataOp().dataValue.activate() = *value;

    return op;
}

Operation
bumpSequence(SequenceNumber to)
{
    Operation op;
    op.body.type(BUMP_SEQUENCE);
    op.body.bumpSequenceOp().bumpTo = to;
    return op;
}

Operation
createClaimableBalance(Asset const& asset, int64_t amount,
                       xdr::xvector<Claimant, 10> const& claimants)
{
    Operation op;
    op.body.type(CREATE_CLAIMABLE_BALANCE);
    op.body.createClaimableBalanceOp().asset = asset;
    op.body.createClaimableBalanceOp().amount = amount;
    op.body.createClaimableBalanceOp().claimants = claimants;
    return op;
}

Operation
claimClaimableBalance(ClaimableBalanceID const& balanceID)
{
    Operation op;
    op.body.type(CLAIM_CLAIMABLE_BALANCE);
    op.body.claimClaimableBalanceOp().balanceID = balanceID;
    return op;
}

TransactionFramePtr
transactionFromOperationsV0(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            const std::vector<Operation>& ops, uint32_t fee)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX_V0);
    e.v0().tx.sourceAccountEd25519 = from.getPublicKey().ed25519();
    e.v0().tx.fee =
        fee != 0 ? fee
                 : static_cast<uint32_t>(
                       (ops.size() * app.getLedgerManager().getLastTxFee()) &
                       UINT32_MAX);
    e.v0().tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.v0().tx.operations));

    auto res = std::static_pointer_cast<TransactionFrame>(
        TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(), e));
    res->addSignature(from);
    return res;
}

TransactionFramePtr
transactionFromOperationsV1(Application& app, SecretKey const& from,
                            SequenceNumber seq,
                            const std::vector<Operation>& ops, uint32_t fee,
                            std::optional<PreconditionsV2> cond)
{
    TransactionEnvelope e(ENVELOPE_TYPE_TX);
    e.v1().tx.sourceAccount = toMuxedAccount(from.getPublicKey());
    e.v1().tx.fee =
        fee != 0 ? fee
                 : static_cast<uint32_t>(
                       (ops.size() * app.getLedgerManager().getLastTxFee()) &
                       UINT32_MAX);
    e.v1().tx.seqNum = seq;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(e.v1().tx.operations));

    if (cond)
    {
        e.v1().tx.cond.type(PRECOND_V2);
        e.v1().tx.cond.v2() = *cond;
    }

    auto res = std::static_pointer_cast<TransactionFrame>(
        TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(), e));
    res->addSignature(from);
    return res;
}

TransactionFramePtr
transactionFromOperations(Application& app, SecretKey const& from,
                          SequenceNumber seq, const std::vector<Operation>& ops,
                          uint32_t fee)
{
    uint32_t ledgerVersion;
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    }
    if (protocolVersionIsBefore(ledgerVersion, ProtocolVersion::V_13))
    {
        return transactionFromOperationsV0(app, from, seq, ops, fee);
    }
    return transactionFromOperationsV1(app, from, seq, ops, fee);
}

TransactionFramePtr
transactionWithV2Precondition(Application& app, TestAccount& account,
                              int64_t sequenceDelta, uint32_t fee,
                              PreconditionsV2 const& cond)
{
    return transactionFromOperationsV1(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), 1)}, fee, cond);
}

TransactionFramePtr
createPaymentTx(Application& app, SecretKey const& from, PublicKey const& to,
                SequenceNumber seq, int64_t amount)
{
    return transactionFromOperations(app, from, seq, {payment(to, amount)});
}

TransactionFramePtr
createCreditPaymentTx(Application& app, SecretKey const& from,
                      PublicKey const& to, Asset const& asset,
                      SequenceNumber seq, int64_t amount)
{
    auto op = payment(to, asset, amount);
    return transactionFromOperations(app, from, seq, {op});
}

Operation
beginSponsoringFutureReserves(PublicKey const& sponsoredID)
{
    Operation op;
    op.body.type(BEGIN_SPONSORING_FUTURE_RESERVES);
    op.body.beginSponsoringFutureReservesOp().sponsoredID = sponsoredID;
    return op;
}

Operation
endSponsoringFutureReserves()
{
    Operation op;
    op.body.type(END_SPONSORING_FUTURE_RESERVES);
    return op;
}

Operation
revokeSponsorship(LedgerKey const& key)
{
    Operation op;
    op.body.type(REVOKE_SPONSORSHIP);
    op.body.revokeSponsorshipOp().type(REVOKE_SPONSORSHIP_LEDGER_ENTRY);
    op.body.revokeSponsorshipOp().ledgerKey() = key;
    return op;
}

Operation
revokeSponsorship(AccountID const& accID, SignerKey const& key)
{
    Operation op;
    op.body.type(REVOKE_SPONSORSHIP);
    op.body.revokeSponsorshipOp().type(REVOKE_SPONSORSHIP_SIGNER);
    op.body.revokeSponsorshipOp().signer().accountID = accID;
    op.body.revokeSponsorshipOp().signer().signerKey = key;
    return op;
}

Operation
clawback(AccountID const& from, Asset const& asset, int64_t amount)
{
    Operation op;
    op.body.type(CLAWBACK);
    op.body.clawbackOp().from = toMuxedAccount(from);
    op.body.clawbackOp().amount = amount;
    op.body.clawbackOp().asset = asset;

    return op;
}

Operation
clawbackClaimableBalance(ClaimableBalanceID const& balanceID)
{
    Operation op;
    op.body.type(CLAWBACK_CLAIMABLE_BALANCE);
    op.body.clawbackClaimableBalanceOp().balanceID = balanceID;
    return op;
}

Operation
liquidityPoolDeposit(PoolID const& poolID, int64_t maxAmountA,
                     int64_t maxAmountB, Price const& minPrice,
                     Price const& maxPrice)
{
    Operation op;
    op.body.type(LIQUIDITY_POOL_DEPOSIT);
    op.body.liquidityPoolDepositOp().liquidityPoolID = poolID;
    op.body.liquidityPoolDepositOp().maxAmountA = maxAmountA;
    op.body.liquidityPoolDepositOp().maxAmountB = maxAmountB;
    op.body.liquidityPoolDepositOp().minPrice = minPrice;
    op.body.liquidityPoolDepositOp().maxPrice = maxPrice;
    return op;
}

Operation
liquidityPoolWithdraw(PoolID const& poolID, int64_t amount, int64_t minAmountA,
                      int64_t minAmountB)
{
    Operation op;
    op.body.type(LIQUIDITY_POOL_WITHDRAW);
    op.body.liquidityPoolWithdrawOp().liquidityPoolID = poolID;
    op.body.liquidityPoolWithdrawOp().amount = amount;
    op.body.liquidityPoolWithdrawOp().minAmountA = minAmountA;
    op.body.liquidityPoolWithdrawOp().minAmountB = minAmountB;
    return op;
}
}
}