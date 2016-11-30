#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

namespace txtest
{

class ex_txNO_ACCOUNT {};

class ex_CREATE_ACCOUNT_MALFORMED {};
class ex_CREATE_ACCOUNT_UNDERFUNDED {};
class ex_CREATE_ACCOUNT_LOW_RESERVE {};
class ex_CREATE_ACCOUNT_ALREADY_EXIST {};

class ex_CHANGE_TRUST_MALFORMED {};
class ex_CHANGE_TRUST_NO_ISSUER {};
class ex_CHANGE_TRUST_INVALID_LIMIT {};
class ex_CHANGE_TRUST_LOW_RESERVE {};
class ex_CHANGE_TRUST_SELF_NOT_ALLOWED {};

class ex_MANAGE_OFFER_MALFORMED {};
class ex_MANAGE_OFFER_SELL_NO_TRUST {};
class ex_MANAGE_OFFER_BUY_NO_TRUST {};
class ex_MANAGE_OFFER_SELL_NOT_AUTHORIZED {};
class ex_MANAGE_OFFER_BUY_NOT_AUTHORIZED {};
class ex_MANAGE_OFFER_LINE_FULL {};
class ex_MANAGE_OFFER_UNDERFUNDED {};
class ex_MANAGE_OFFER_CROSS_SELF {};
class ex_MANAGE_OFFER_SELL_NO_ISSUER {};
class ex_MANAGE_OFFER_BUY_NO_ISSUER {};
class ex_MANAGE_OFFER_NOT_FOUND {};
class ex_MANAGE_OFFER_LOW_RESERVE {};

class ex_PAYMENT_MALFORMED {};
class ex_PAYMENT_UNDERFUNDED {};
class ex_PAYMENT_SRC_NO_TRUST {};
class ex_PAYMENT_SRC_NOT_AUTHORIZED {};
class ex_PAYMENT_NO_DESTINATION {};
class ex_PAYMENT_NO_TRUST {};
class ex_PAYMENT_NOT_AUTHORIZED {};
class ex_PAYMENT_LINE_FULL {};
class ex_PAYMENT_NO_ISSUER {};

}

}
