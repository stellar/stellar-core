#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

namespace txtest
{

class ex_txNO_ACCOUNT
{
};

class ex_txINTERNAL_ERROR
{
};

class ex_ACCOUNT_MERGE_MALFORMED
{
};
class ex_ACCOUNT_MERGE_NO_ACCOUNT
{
};
class ex_ACCOUNT_MERGE_IMMUTABLE_SET
{
};
class ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES
{
};

class ex_ALLOW_TRUST_MALFORMED
{
};
class ex_ALLOW_TRUST_NO_TRUST_LINE
{
};
class ex_ALLOW_TRUST_TRUST_NOT_REQUIRED
{
};
class ex_ALLOW_TRUST_CANT_REVOKE
{
};
class ex_ALLOW_TRUST_SELF_NOT_ALLOWED
{
};

class ex_CREATE_ACCOUNT_MALFORMED
{
};
class ex_CREATE_ACCOUNT_UNDERFUNDED
{
};
class ex_CREATE_ACCOUNT_LOW_RESERVE
{
};
class ex_CREATE_ACCOUNT_ALREADY_EXIST
{
};

class ex_CHANGE_TRUST_MALFORMED
{
};
class ex_CHANGE_TRUST_NO_ISSUER
{
};
class ex_CHANGE_TRUST_INVALID_LIMIT
{
};
class ex_CHANGE_TRUST_LOW_RESERVE
{
};
class ex_CHANGE_TRUST_SELF_NOT_ALLOWED
{
};

class ex_MANAGE_DATA_NOT_SUPPORTED_YET
{
};
class ex_MANAGE_DATA_NAME_NOT_FOUND
{
};
class ex_MANAGE_DATA_LOW_RESERVE
{
};
class ex_MANAGE_DATA_INVALID_NAME
{
};

class ex_MANAGE_OFFER_MALFORMED
{
};
class ex_MANAGE_OFFER_SELL_NO_TRUST
{
};
class ex_MANAGE_OFFER_BUY_NO_TRUST
{
};
class ex_MANAGE_OFFER_SELL_NOT_AUTHORIZED
{
};
class ex_MANAGE_OFFER_BUY_NOT_AUTHORIZED
{
};
class ex_MANAGE_OFFER_LINE_FULL
{
};
class ex_MANAGE_OFFER_UNDERFUNDED
{
};
class ex_MANAGE_OFFER_CROSS_SELF
{
};
class ex_MANAGE_OFFER_SELL_NO_ISSUER
{
};
class ex_MANAGE_OFFER_BUY_NO_ISSUER
{
};
class ex_MANAGE_OFFER_NOT_FOUND
{
};
class ex_MANAGE_OFFER_LOW_RESERVE
{
};

class ex_PATH_PAYMENT_SUCCESS
{
};
class ex_PATH_PAYMENT_MALFORMED
{
};
class ex_PATH_PAYMENT_UNDERFUNDED
{
};
class ex_PATH_PAYMENT_SRC_NO_TRUST
{
};
class ex_PATH_PAYMENT_SRC_NOT_AUTHORIZED
{
};
class ex_PATH_PAYMENT_NO_DESTINATION
{
};
class ex_PATH_PAYMENT_NO_TRUST
{
};
class ex_PATH_PAYMENT_NOT_AUTHORIZED
{
};
class ex_PATH_PAYMENT_LINE_FULL
{
};
class ex_PATH_PAYMENT_NO_ISSUER
{
};
class ex_PATH_PAYMENT_TOO_FEW_OFFERS
{
};
class ex_PATH_PAYMENT_OFFER_CROSS_SELF
{
};
class ex_PATH_PAYMENT_OVER_SENDMAX
{
};

class ex_PAYMENT_MALFORMED
{
};
class ex_PAYMENT_UNDERFUNDED
{
};
class ex_PAYMENT_SRC_NO_TRUST
{
};
class ex_PAYMENT_SRC_NOT_AUTHORIZED
{
};
class ex_PAYMENT_NO_DESTINATION
{
};
class ex_PAYMENT_NO_TRUST
{
};
class ex_PAYMENT_NOT_AUTHORIZED
{
};
class ex_PAYMENT_LINE_FULL
{
};
class ex_PAYMENT_NO_ISSUER
{
};

class ex_SET_OPTIONS_LOW_RESERVE
{
};
class ex_SET_OPTIONS_TOO_MANY_SIGNERS
{
};
class ex_SET_OPTIONS_BAD_FLAGS
{
};
class ex_SET_OPTIONS_INVALID_INFLATION
{
};
class ex_SET_OPTIONS_CANT_CHANGE
{
};
class ex_SET_OPTIONS_UNKNOWN_FLAG
{
};
class ex_SET_OPTIONS_THRESHOLD_OUT_OF_RANGE
{
};
class ex_SET_OPTIONS_BAD_SIGNER
{
};
class ex_SET_OPTIONS_INVALID_HOME_DOMAIN
{
};
}
}
