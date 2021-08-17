// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TestExceptions.h"

namespace stellar
{

namespace txtest
{

void
throwIf(CreateAccountResult const& result)
{
    switch (result.code())
    {
    case CREATE_ACCOUNT_MALFORMED:
        throw ex_CREATE_ACCOUNT_MALFORMED{};
    case CREATE_ACCOUNT_UNDERFUNDED:
        throw ex_CREATE_ACCOUNT_UNDERFUNDED{};
    case CREATE_ACCOUNT_LOW_RESERVE:
        throw ex_CREATE_ACCOUNT_LOW_RESERVE{};
    case CREATE_ACCOUNT_ALREADY_EXIST:
        throw ex_CREATE_ACCOUNT_ALREADY_EXIST{};
    case CREATE_ACCOUNT_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(PaymentResult const& result)
{
    switch (result.code())
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
    case PAYMENT_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(PathPaymentStrictReceiveResult const& result)
{
    switch (result.code())
    {
    case PATH_PAYMENT_STRICT_RECEIVE_MALFORMED:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_MALFORMED{};
    case PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED{};
    case PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST{};
    case PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED{};
    case PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION{};
    case PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST{};
    case PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED{};
    case PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL{};
    case PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER{};
    case PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS{};
    case PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF{};
    case PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX:
        throw ex_PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX{};
    case PATH_PAYMENT_STRICT_RECEIVE_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(PathPaymentStrictSendResult const& result)
{
    switch (result.code())
    {
    case PATH_PAYMENT_STRICT_SEND_MALFORMED:
        throw ex_PATH_PAYMENT_STRICT_SEND_MALFORMED{};
    case PATH_PAYMENT_STRICT_SEND_UNDERFUNDED:
        throw ex_PATH_PAYMENT_STRICT_SEND_UNDERFUNDED{};
    case PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST:
        throw ex_PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST{};
    case PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED{};
    case PATH_PAYMENT_STRICT_SEND_NO_DESTINATION:
        throw ex_PATH_PAYMENT_STRICT_SEND_NO_DESTINATION{};
    case PATH_PAYMENT_STRICT_SEND_NO_TRUST:
        throw ex_PATH_PAYMENT_STRICT_SEND_NO_TRUST{};
    case PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED:
        throw ex_PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED{};
    case PATH_PAYMENT_STRICT_SEND_LINE_FULL:
        throw ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL{};
    case PATH_PAYMENT_STRICT_SEND_NO_ISSUER:
        throw ex_PATH_PAYMENT_STRICT_SEND_NO_ISSUER{};
    case PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS:
        throw ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS{};
    case PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF:
        throw ex_PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF{};
    case PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN:
        throw ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN{};
    case PATH_PAYMENT_STRICT_SEND_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ManageSellOfferResult const& result)
{
    switch (result.code())
    {
    case MANAGE_SELL_OFFER_MALFORMED:
        throw ex_MANAGE_SELL_OFFER_MALFORMED{};
    case MANAGE_SELL_OFFER_SELL_NO_TRUST:
        throw ex_MANAGE_SELL_OFFER_SELL_NO_TRUST{};
    case MANAGE_SELL_OFFER_BUY_NO_TRUST:
        throw ex_MANAGE_SELL_OFFER_BUY_NO_TRUST{};
    case MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED:
        throw ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED{};
    case MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED:
        throw ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED{};
    case MANAGE_SELL_OFFER_LINE_FULL:
        throw ex_MANAGE_SELL_OFFER_LINE_FULL{};
    case MANAGE_SELL_OFFER_UNDERFUNDED:
        throw ex_MANAGE_SELL_OFFER_UNDERFUNDED{};
    case MANAGE_SELL_OFFER_CROSS_SELF:
        throw ex_MANAGE_SELL_OFFER_CROSS_SELF{};
    case MANAGE_SELL_OFFER_SELL_NO_ISSUER:
        throw ex_MANAGE_SELL_OFFER_SELL_NO_ISSUER{};
    case MANAGE_SELL_OFFER_BUY_NO_ISSUER:
        throw ex_MANAGE_SELL_OFFER_BUY_NO_ISSUER{};
    case MANAGE_SELL_OFFER_NOT_FOUND:
        throw ex_MANAGE_SELL_OFFER_NOT_FOUND{};
    case MANAGE_SELL_OFFER_LOW_RESERVE:
        throw ex_MANAGE_SELL_OFFER_LOW_RESERVE{};
    case MANAGE_SELL_OFFER_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ManageBuyOfferResult const& result)
{
    switch (result.code())
    {
    case MANAGE_BUY_OFFER_MALFORMED:
        throw ex_MANAGE_BUY_OFFER_MALFORMED{};
    case MANAGE_BUY_OFFER_SELL_NO_TRUST:
        throw ex_MANAGE_BUY_OFFER_SELL_NO_TRUST{};
    case MANAGE_BUY_OFFER_BUY_NO_TRUST:
        throw ex_MANAGE_BUY_OFFER_BUY_NO_TRUST{};
    case MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED:
        throw ex_MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED{};
    case MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED:
        throw ex_MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED{};
    case MANAGE_BUY_OFFER_LINE_FULL:
        throw ex_MANAGE_BUY_OFFER_LINE_FULL{};
    case MANAGE_BUY_OFFER_UNDERFUNDED:
        throw ex_MANAGE_BUY_OFFER_UNDERFUNDED{};
    case MANAGE_BUY_OFFER_CROSS_SELF:
        throw ex_MANAGE_BUY_OFFER_CROSS_SELF{};
    case MANAGE_BUY_OFFER_SELL_NO_ISSUER:
        throw ex_MANAGE_BUY_OFFER_SELL_NO_ISSUER{};
    case MANAGE_BUY_OFFER_BUY_NO_ISSUER:
        throw ex_MANAGE_BUY_OFFER_BUY_NO_ISSUER{};
    case MANAGE_BUY_OFFER_NOT_FOUND:
        throw ex_MANAGE_BUY_OFFER_NOT_FOUND{};
    case MANAGE_BUY_OFFER_LOW_RESERVE:
        throw ex_MANAGE_BUY_OFFER_LOW_RESERVE{};
    case MANAGE_BUY_OFFER_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(SetOptionsResult const& result)
{
    switch (result.code())
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
    case SET_OPTIONS_AUTH_REVOCABLE_REQUIRED:
        throw ex_SET_OPTIONS_AUTH_REVOCABLE_REQUIRED{};
    case SET_OPTIONS_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ChangeTrustResult const& result)
{
    switch (result.code())
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
    case CHANGE_TRUST_TRUST_LINE_MISSING:
        throw ex_CHANGE_TRUST_TRUST_LINE_MISSING{};
    case CHANGE_TRUST_CANNOT_DELETE:
        throw ex_CHANGE_TRUST_CANNOT_DELETE{};
    case CHANGE_TRUST_NOT_AUTH_MAINTAIN_LIABILITIES:
        throw ex_CHANGE_TRUST_NOT_AUTH_MAINTAIN_LIABILITIES{};
    case CHANGE_TRUST_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(AllowTrustResult const& result)
{
    switch (result.code())
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
    case ALLOW_TRUST_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(AccountMergeResult const& result)
{
    switch (result.code())
    {
    case ACCOUNT_MERGE_MALFORMED:
        throw ex_ACCOUNT_MERGE_MALFORMED{};
    case ACCOUNT_MERGE_NO_ACCOUNT:
        throw ex_ACCOUNT_MERGE_NO_ACCOUNT{};
    case ACCOUNT_MERGE_IMMUTABLE_SET:
        throw ex_ACCOUNT_MERGE_IMMUTABLE_SET{};
    case ACCOUNT_MERGE_HAS_SUB_ENTRIES:
        throw ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES{};
    case ACCOUNT_MERGE_SEQNUM_TOO_FAR:
        throw ex_ACCOUNT_MERGE_SEQNUM_TOO_FAR{};
    case ACCOUNT_MERGE_DEST_FULL:
        throw ex_ACCOUNT_MERGE_DEST_FULL{};
    case ACCOUNT_MERGE_IS_SPONSOR:
        throw ex_ACCOUNT_MERGE_IS_SPONSOR{};
    case ACCOUNT_MERGE_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(InflationResult const& result)
{
    switch (result.code())
    {
    case INFLATION_NOT_TIME:
        throw ex_INFLATION_NOT_TIME{};
    case INFLATION_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ManageDataResult const& result)
{
    switch (result.code())
    {
    case MANAGE_DATA_NOT_SUPPORTED_YET:
        throw ex_MANAGE_DATA_NOT_SUPPORTED_YET{};
    case MANAGE_DATA_NAME_NOT_FOUND:
        throw ex_MANAGE_DATA_NAME_NOT_FOUND{};
    case MANAGE_DATA_LOW_RESERVE:
        throw ex_MANAGE_DATA_LOW_RESERVE{};
    case MANAGE_DATA_INVALID_NAME:
        throw ex_MANAGE_DATA_INVALID_NAME{};
    case MANAGE_DATA_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(BumpSequenceResult const& result)
{
    switch (result.code())
    {
    case BUMP_SEQUENCE_SUCCESS:
        break;
    case BUMP_SEQUENCE_BAD_SEQ:
        throw ex_BUMP_SEQUENCE_BAD_SEQ{};
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(CreateClaimableBalanceResult const& result)
{
    switch (result.code())
    {
    case CREATE_CLAIMABLE_BALANCE_MALFORMED:
        throw ex_CREATE_CLAIMABLE_BALANCE_MALFORMED{};
    case CREATE_CLAIMABLE_BALANCE_LOW_RESERVE:
        throw ex_CREATE_CLAIMABLE_BALANCE_LOW_RESERVE{};
    case CREATE_CLAIMABLE_BALANCE_NO_TRUST:
        throw ex_CREATE_CLAIMABLE_BALANCE_NO_TRUST{};
    case CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED:
        throw ex_CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED{};
    case CREATE_CLAIMABLE_BALANCE_UNDERFUNDED:
        throw ex_CREATE_CLAIMABLE_BALANCE_UNDERFUNDED{};
    case CREATE_CLAIMABLE_BALANCE_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ClaimClaimableBalanceResult const& result)
{
    switch (result.code())
    {
    case CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST:
        throw ex_CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST{};
    case CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM:
        throw ex_CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM{};
    case CLAIM_CLAIMABLE_BALANCE_LINE_FULL:
        throw ex_CLAIM_CLAIMABLE_BALANCE_LINE_FULL{};
    case CLAIM_CLAIMABLE_BALANCE_NOT_AUTHORIZED:
        throw ex_CLAIM_CLAIMABLE_BALANCE_NOT_AUTHORIZED{};
    case CLAIM_CLAIMABLE_BALANCE_NO_TRUST:
        throw ex_CLAIM_CLAIMABLE_BALANCE_NO_TRUST{};
    case CLAIM_CLAIMABLE_BALANCE_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ClawbackResult const& result)
{
    switch (result.code())
    {
    case CLAWBACK_MALFORMED:
        throw ex_CLAWBACK_MALFORMED{};
    case CLAWBACK_NO_TRUST:
        throw ex_CLAWBACK_NO_TRUST{};
    case CLAWBACK_NOT_CLAWBACK_ENABLED:
        throw ex_CLAWBACK_NOT_CLAWBACK_ENABLED{};
    case CLAWBACK_UNDERFUNDED:
        throw ex_CLAWBACK_UNDERFUNDED{};
    case CLAWBACK_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ClawbackClaimableBalanceResult const& result)
{
    switch (result.code())
    {
    case CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST:
        throw ex_CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST{};
    case CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER:
        throw ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER{};
    case CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED:
        throw ex_CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED{};
    case CLAWBACK_CLAIMABLE_BALANCE_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(SetTrustLineFlagsResult const& result)
{
    switch (result.code())
    {
    case SET_TRUST_LINE_FLAGS_MALFORMED:
        throw ex_SET_TRUST_LINE_FLAGS_MALFORMED{};
    case SET_TRUST_LINE_FLAGS_NO_TRUST_LINE:
        throw ex_SET_TRUST_LINE_FLAGS_NO_TRUST_LINE{};
    case SET_TRUST_LINE_FLAGS_CANT_REVOKE:
        throw ex_SET_TRUST_LINE_FLAGS_CANT_REVOKE{};
    case SET_TRUST_LINE_FLAGS_INVALID_STATE:
        throw ex_SET_TRUST_LINE_FLAGS_INVALID_STATE{};
    case SET_TRUST_LINE_FLAGS_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(LiquidityPoolDepositResult const& result)
{
    switch (result.code())
    {
    case LIQUIDITY_POOL_DEPOSIT_MALFORMED:
        throw ex_LIQUIDITY_POOL_DEPOSIT_MALFORMED{};
    case LIQUIDITY_POOL_DEPOSIT_NO_TRUST:
        throw ex_LIQUIDITY_POOL_DEPOSIT_NO_TRUST{};
    case LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED:
        throw ex_LIQUIDITY_POOL_DEPOSIT_NOT_AUTHORIZED{};
    case LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED:
        throw ex_LIQUIDITY_POOL_DEPOSIT_UNDERFUNDED{};
    case LIQUIDITY_POOL_DEPOSIT_LINE_FULL:
        throw ex_LIQUIDITY_POOL_DEPOSIT_LINE_FULL{};
    case LIQUIDITY_POOL_DEPOSIT_BAD_PRICE:
        throw ex_LIQUIDITY_POOL_DEPOSIT_BAD_PRICE{};
    case LIQUIDITY_POOL_DEPOSIT_POOL_FULL:
        throw ex_LIQUIDITY_POOL_DEPOSIT_POOL_FULL{};
    case LIQUIDITY_POOL_DEPOSIT_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(LiquidityPoolWithdrawResult const& result)
{
    switch (result.code())
    {
    case LIQUIDITY_POOL_WITHDRAW_MALFORMED:
        throw ex_LIQUIDITY_POOL_WITHDRAW_MALFORMED{};
    case LIQUIDITY_POOL_WITHDRAW_NO_TRUST:
        throw ex_LIQUIDITY_POOL_WITHDRAW_NO_TRUST{};
    case LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED:
        throw ex_LIQUIDITY_POOL_WITHDRAW_UNDERFUNDED{};
    case LIQUIDITY_POOL_WITHDRAW_LINE_FULL:
        throw ex_LIQUIDITY_POOL_WITHDRAW_LINE_FULL{};
    case LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM:
        throw ex_LIQUIDITY_POOL_WITHDRAW_UNDER_MINIMUM{};
    case LIQUIDITY_POOL_WITHDRAW_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(TransactionResult const& result)
{
    switch (result.result.code())
    {
    case txSUCCESS:
    case txFAILED:
        break;
    case txBAD_SEQ:
        throw ex_txBAD_SEQ{};
    case txNO_ACCOUNT:
        throw ex_txNO_ACCOUNT{};
    case txINTERNAL_ERROR:
        throw ex_txINTERNAL_ERROR{};
    case txINSUFFICIENT_BALANCE:
        throw ex_txINSUFFICIENT_BALANCE{};
    case txBAD_AUTH:
        throw ex_txBAD_AUTH{};
    default:
        throw ex_UNKNOWN{};
    }

    auto opResult = result.result.results()[0];
    switch (opResult.code())
    {
    case opINNER:
        break;
    case opBAD_AUTH:
        throw ex_opBAD_AUTH{};
        break;
    case opNO_ACCOUNT:
        throw ex_opNO_ACCOUNT{};
        break;
    case opNOT_SUPPORTED:
        throw ex_opNOT_SUPPORTED{};
        break;
    default:
        throw ex_UNKNOWN{};
    };

    switch (opResult.tr().type())
    {
    case CREATE_ACCOUNT:
        throwIf(opResult.tr().createAccountResult());
        break;
    case PAYMENT:
        throwIf(opResult.tr().paymentResult());
        break;
    case PATH_PAYMENT_STRICT_RECEIVE:
        throwIf(opResult.tr().pathPaymentStrictReceiveResult());
        break;
    case MANAGE_SELL_OFFER:
        throwIf(opResult.tr().manageSellOfferResult());
        break;
    case CREATE_PASSIVE_SELL_OFFER:
        throwIf(opResult.tr().createPassiveSellOfferResult());
        break;
    case SET_OPTIONS:
        throwIf(opResult.tr().setOptionsResult());
        break;
    case CHANGE_TRUST:
        throwIf(opResult.tr().changeTrustResult());
        break;
    case ALLOW_TRUST:
        throwIf(opResult.tr().allowTrustResult());
        break;
    case ACCOUNT_MERGE:
        throwIf(opResult.tr().accountMergeResult());
        break;
    case INFLATION:
        throwIf(opResult.tr().inflationResult());
        break;
    case MANAGE_DATA:
        throwIf(opResult.tr().manageDataResult());
        break;
    case BUMP_SEQUENCE:
        throwIf(opResult.tr().bumpSeqResult());
        break;
    case MANAGE_BUY_OFFER:
        throwIf(opResult.tr().manageBuyOfferResult());
        break;
    case PATH_PAYMENT_STRICT_SEND:
        throwIf(opResult.tr().pathPaymentStrictSendResult());
        break;
    case CREATE_CLAIMABLE_BALANCE:
        throwIf(opResult.tr().createClaimableBalanceResult());
        break;
    case CLAIM_CLAIMABLE_BALANCE:
        throwIf(opResult.tr().claimClaimableBalanceResult());
        break;
    case BEGIN_SPONSORING_FUTURE_RESERVES:
    case END_SPONSORING_FUTURE_RESERVES:
    case REVOKE_SPONSORSHIP:
        // Sponsorship tests catch error codes at a higher level than this.
        throw std::runtime_error("got error-result in test sponsorship tx");
        break;
    case CLAWBACK:
        throwIf(opResult.tr().clawbackResult());
        break;
    case CLAWBACK_CLAIMABLE_BALANCE:
        throwIf(opResult.tr().clawbackClaimableBalanceResult());
        break;
    case SET_TRUST_LINE_FLAGS:
        throwIf(opResult.tr().setTrustLineFlagsResult());
        break;
    case LIQUIDITY_POOL_DEPOSIT:
        throwIf(opResult.tr().liquidityPoolDepositResult());
        break;
    case LIQUIDITY_POOL_WITHDRAW:
        throwIf(opResult.tr().liquidityPoolWithdrawResult());
        break;
    }
}
}
}
