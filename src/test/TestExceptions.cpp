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
throwIf(PathPaymentResult const& result)
{
    switch (result.code())
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
        throw ex_PATH_PAYMENT_NO_ISSUER{};
    case PATH_PAYMENT_TOO_FEW_OFFERS:
        throw ex_PATH_PAYMENT_TOO_FEW_OFFERS{};
    case PATH_PAYMENT_OFFER_CROSS_SELF:
        throw ex_PATH_PAYMENT_OFFER_CROSS_SELF{};
    case PATH_PAYMENT_OVER_SENDMAX:
        throw ex_PATH_PAYMENT_OVER_SENDMAX{};
    case PATH_PAYMENT_SUCCESS:
        break;
    default:
        throw ex_UNKNOWN{};
    }
}

void
throwIf(ManageOfferResult const& result)
{
    switch (result.code())
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
    case MANAGE_OFFER_SUCCESS:
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
throwIf(TransactionResult const& result)
{
    switch (result.result.code())
    {
    case txNO_ACCOUNT:
        throw ex_txNO_ACCOUNT{};
    case txINTERNAL_ERROR:
        throw ex_txINTERNAL_ERROR{};
    case txINSUFFICIENT_BALANCE:
        throw ex_txINSUFFICIENT_BALANCE{};
    case txBAD_AUTH:
        throw ex_txBAD_AUTH{};
    default:
        // ignore rest for now
        break;
    }

    auto opResult = result.result.results()[0];
    switch (opResult.tr().type())
    {
    case CREATE_ACCOUNT:
        throwIf(opResult.tr().createAccountResult());
        break;
    case PAYMENT:
        throwIf(opResult.tr().paymentResult());
        break;
    case PATH_PAYMENT:
        throwIf(opResult.tr().pathPaymentResult());
        break;
    case MANAGE_OFFER:
        throwIf(opResult.tr().manageOfferResult());
        break;
    case CREATE_PASSIVE_OFFER:
        throwIf(opResult.tr().createPassiveOfferResult());
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
    }
}
}
}
