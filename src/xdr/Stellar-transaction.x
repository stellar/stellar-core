// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "generated/Stellar-ledger-entries.h"

namespace stellar
{

struct DecoratedSignature
{
    opaque hint[4];    // first 4 bytes of the public key, used as a hint
    uint512 signature; // actual signature
};

enum OperationType
{
    CREATE_ACCOUNT = 0,
    PAYMENT = 1,
    PATH_PAYMENT = 2,
    MANAGE_OFFER = 3,
	CREATE_PASSIVE_OFFER = 4,
    SET_OPTIONS = 5,
    CHANGE_TRUST = 6,
    ALLOW_TRUST = 7,
    ACCOUNT_MERGE = 8,
    INFLATION = 9
};

/* CreateAccount
Funds a new account with the specified starting balance

Threshold: med

Result: CreateAccountResult

*/

struct CreateAccountOp
{
    AccountID destination; // account to create
    int64 startingBalance; // amount they end up with
};

/* Payment

    send an amount to a destination account.

    Threshold: med

    Result: PaymentResult
*/
struct PaymentOp
{
    AccountID destination; // recipient of the payment
    Currency currency;     // what they end up with
    int64 amount;          // amount they end up with
};

/* PathPayment

send an amount to a destination account through a path.
(up to sendMax, sendCurrency)
(X0, Path[0]) .. (Xn, Path[n])
(destAmount, destCurrency)

Threshold: med

Result: PathPaymentResult
*/
struct PathPaymentOp
{
    Currency sendCurrency; // currency we pay with
    int64 sendMax;         // the maximum amount of sendCurrency to
                           // send (excluding fees).
                           // The operation will fail if can't be met

    AccountID destination; // recipient of the payment
    Currency destCurrency; // what they end up with
    int64 destAmount;      // amount they end up with

    Currency path<5>; // additional hops it must go through to get there
};

/* Creates, updates or deletes an offer

Threshold: med

Result: ManageOfferResult

*/
struct ManageOfferOp
{
    Currency takerGets;
    Currency takerPays;
    int64 amount; // amount taker gets. if set to 0, delete the offer
    Price price;  // =takerPaysAmount/takerGetsAmount

    // 0=create a new offer, otherwise edit an existing offer
    uint64 offerID;
};

/* Creates an offer that doesn't take offers of the same price

Threshold: med

Result: CreatePassiveOfferResult

*/
struct CreatePassiveOfferOp
{
    Currency takerGets;
    Currency takerPays;
    int64 amount; // amount taker gets. if set to 0, delete the offer
    Price price;  // =takerPaysAmount/takerGetsAmount
};

/* Set Account Options

    updates "AccountEntry" fields.
    note: updating thresholds or signers requires high threshold

    Threshold: med or high

    Result: SetOptionsResult
*/

struct SetOptionsOp
{
    AccountID* inflationDest; // sets the inflation destination

    uint32* clearFlags; // which flags to clear
    uint32* setFlags;   // which flags to set

    Thresholds* thresholds; // update the thresholds for the account

    string32* homeDomain; // sets the home domain

    // Add, update or remove a signer for the account
    // signer is deleted if the weight is 0
    Signer* signer;
};

/* Creates, updates or deletes a trust line

    Threshold: med

    Result: ChangeTrustResult

*/
struct ChangeTrustOp
{
    Currency line;

    // if limit is set to 0, deletes the trust line
    int64 limit;
};

/* Updates the "authorized" flag of an existing trust line
   this is called by the issuer of the related currency.

   note that authorize can only be set (and not cleared) if
   the issuer account does not have the AUTH_REVOCABLE_FLAG set
   Threshold: low

   Result: AllowTrustResult
*/
struct AllowTrustOp
{
    AccountID trustor;
    union switch (CurrencyType type)
    {
    // CURRENCY_TYPE_NATIVE is not allowed
    case CURRENCY_TYPE_ALPHANUM:
        opaque currencyCode[4];

        // add other currency types here in the future
    }
    currency;

    bool authorize;
};

/* Inflation
    Runs inflation

Threshold: low

Result: InflationResult

*/

/* AccountMerge
    Transfers native balance to destination account.

    Threshold: high

    Result : AccountMergeResult
*/

/* An operation is the lowest unit of work that a transaction does */
struct Operation
{
    // sourceAccount is the account used to run the operation
    // if not set, the runtime defaults to "account" specified at
    // the transaction level
    AccountID* sourceAccount;

    union switch (OperationType type)
    {
    case CREATE_ACCOUNT:
        CreateAccountOp createAccountOp;
    case PAYMENT:
        PaymentOp paymentOp;
    case PATH_PAYMENT:
        PathPaymentOp pathPaymentOp;
    case MANAGE_OFFER:
        ManageOfferOp manageOfferOp;
	case CREATE_PASSIVE_OFFER:
        CreatePassiveOfferOp createPassiveOfferOp;
    case SET_OPTIONS:
        SetOptionsOp setOptionsOp;
    case CHANGE_TRUST:
        ChangeTrustOp changeTrustOp;
    case ALLOW_TRUST:
        AllowTrustOp allowTrustOp;
    case ACCOUNT_MERGE:
        uint256 destination;
    case INFLATION:
        void;
    }
    body;
};

enum MemoType
{
    MEMO_NONE = 0,
    MEMO_TEXT = 1,
    MEMO_ID = 2,
    MEMO_HASH = 3,
    MEMO_RETURN = 4
};

union Memo switch (MemoType type)
{
case MEMO_NONE:
    void;
case MEMO_TEXT:
    string text<28>;
case MEMO_ID:
    uint64 id;
case MEMO_HASH:
    Hash hash; // the hash of what to pull from the content server
case MEMO_RETURN:
    Hash retHash; // the hash of the tx you are rejecting
};

struct TimeBounds
{
    uint64 minTime;
    uint64 maxTime;
};

/* a transaction is a container for a set of operations
    - is executed by an account
    - fees are collected from the account
    - operations are executed in order as one ACID transaction
          either all operations are applied or none are
          if any returns a failing code
*/

struct Transaction
{
    // account used to run the transaction
    AccountID sourceAccount;

    // the fee the sourceAccount will pay
    int32 fee;

    // sequence number to consume in the account
    SequenceNumber seqNum;

    // validity range (inclusive) for the last ledger close time
    TimeBounds* timeBounds;

    Memo memo;

    Operation operations<100>;
};

/* A TransactionEnvelope wraps a transaction with signatures. */
struct TransactionEnvelope
{
    Transaction tx;
    DecoratedSignature signatures<20>;
};

/* Operation Results section */

/* This result is used when offers are taken during an operation */
struct ClaimOfferAtom
{
    // emited to identify the offer
    AccountID offerOwner; // Account that owns the offer
    uint64 offerID;

    // amount and currency taken from the owner
    Currency currencyClaimed;
    int64 amountClaimed;

    // should we also include the amount that the owner gets in return?
};

/******* CreateAccount Result ********/

enum CreateAccountResultCode
{
    // codes considered as "success" for the operation
    CREATE_ACCOUNT_SUCCESS = 0, // account was created

    // codes considered as "failure" for the operation
    CREATE_ACCOUNT_MALFORMED = 1,   // invalid destination
    CREATE_ACCOUNT_UNDERFUNDED = 2, // not enough funds in source account
    CREATE_ACCOUNT_LOW_RESERVE =
        3, // would create an account below the min reserve
    CREATE_ACCOUNT_ALREADY_EXIST = 4 // account already exists
};

union CreateAccountResult switch (CreateAccountResultCode code)
{
case CREATE_ACCOUNT_SUCCESS:
    void;
default:
    void;
};

/******* Payment Result ********/

enum PaymentResultCode
{
    // codes considered as "success" for the operation
    PAYMENT_SUCCESS = 0, // payment successfuly completed

    // codes considered as "failure" for the operation
    PAYMENT_MALFORMED = -1,      // bad input
    PAYMENT_UNDERFUNDED = -2,    // not enough funds in source account
    PAYMENT_NO_DESTINATION = -3, // destination account does not exist
    PAYMENT_NO_TRUST = -4, // destination missing a trust line for currency
    PAYMENT_NOT_AUTHORIZED = -5, // destination not authorized to hold currency
    PAYMENT_LINE_FULL = -6       // destination would go above their limit
};

union PaymentResult switch (PaymentResultCode code)
{
case PAYMENT_SUCCESS:
    void;
default:
    void;
};

/******* Payment Result ********/

enum PathPaymentResultCode
{
    // codes considered as "success" for the operation
    PATH_PAYMENT_SUCCESS = 0, // success

    // codes considered as "failure" for the operation
    PATH_PAYMENT_MALFORMED = -1,      // bad input
    PATH_PAYMENT_UNDERFUNDED = -2,    // not enough funds in source account
    PATH_PAYMENT_NO_DESTINATION = -3, // destination account does not exist
    PATH_PAYMENT_NO_TRUST = -4, // destination missing a trust line for currency
    PATH_PAYMENT_NOT_AUTHORIZED =
        -5,                      // destination not authorized to hold currency
    PATH_PAYMENT_LINE_FULL = -6, // destination would go above their limit
    PATH_PAYMENT_TOO_FEW_OFFERS = -7, // not enough offers to satisfy path
    PATH_PAYMENT_OVER_SENDMAX = -8    // could not satisfy sendmax
};

struct SimplePaymentResult
{
    AccountID destination;
    Currency currency;
    int64 amount;
};

union PathPaymentResult switch (PathPaymentResultCode code)
{
case PATH_PAYMENT_SUCCESS:
    struct
    {
        ClaimOfferAtom offers<>;
        SimplePaymentResult last;
    } success;
default:
    void;
};

/******* ManageOffer Result ********/

enum ManageOfferResultCode
{
    // codes considered as "success" for the operation
    MANAGE_OFFER_SUCCESS = 0,

    // codes considered as "failure" for the operation
    MANAGE_OFFER_MALFORMED = -1,      // generated offer would be invalid
    MANAGE_OFFER_NO_TRUST = -2,       // can't hold what it's buying
    MANAGE_OFFER_NOT_AUTHORIZED = -3, // not authorized to sell or buy
    MANAGE_OFFER_LINE_FULL = -4,      // can't receive more of what it's buying
    MANAGE_OFFER_UNDERFUNDED = -5,    // doesn't hold what it's trying to sell
    MANAGE_OFFER_CROSS_SELF = -6,     // would cross an offer from the same user

    // update errors
    MANAGE_OFFER_NOT_FOUND = -7, // offerID does not match an existing offer
    MANAGE_OFFER_MISMATCH = -8,  // currencies don't match offer

    MANAGE_OFFER_LOW_RESERVE = -9 // not enough funds to create a new Offer
};


enum ManageOfferEffect
{
    MANAGE_OFFER_CREATED = 0,
    MANAGE_OFFER_UPDATED = 1,
    MANAGE_OFFER_DELETED = 2
};

struct ManageOfferSuccessResult
{
    // offers that got claimed while creating this offer
    ClaimOfferAtom offersClaimed<>;

    union switch (ManageOfferEffect effect)
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
        OfferEntry offer;
    default:
        void;
    }
    offer;
};

union ManageOfferResult switch (ManageOfferResultCode code)
{
case MANAGE_OFFER_SUCCESS:
    ManageOfferSuccessResult success;
default:
    void;
};

/******* SetOptions Result ********/

enum SetOptionsResultCode
{
    // codes considered as "success" for the operation
    SET_OPTIONS_SUCCESS = 0,
    // codes considered as "failure" for the operation
    SET_OPTIONS_LOW_RESERVE = -1,      // not enough funds to add a signer
    SET_OPTIONS_TOO_MANY_SIGNERS = -2, // max number of signers already reached
    SET_OPTIONS_BAD_FLAGS = -3,        // invalid combination of clear/set flags
    SET_OPTIONS_INVALID_INFLATION = -4, // inflation account does not exist
    SET_OPTIONS_CANT_CHANGE = -5,       // can no longer change this option
    SET_OPTIONS_UNKNOWN_FLAG = -6		// can't set an unknown flag
};

union SetOptionsResult switch (SetOptionsResultCode code)
{
case SET_OPTIONS_SUCCESS:
    void;
default:
    void;
};

/******* ChangeTrust Result ********/

enum ChangeTrustResultCode
{
    // codes considered as "success" for the operation
    CHANGE_TRUST_SUCCESS = 0,
    // codes considered as "failure" for the operation
    CHANGE_TRUST_MALFORMED = -1,     // bad input
    CHANGE_TRUST_NO_ISSUER = -2,     // could not find issuer
    CHANGE_TRUST_INVALID_LIMIT = -3, // cannot drop limit below balance
    CHANGE_TRUST_LOW_RESERVE = -4 // not enough funds to create a new trust line
};

union ChangeTrustResult switch (ChangeTrustResultCode code)
{
case CHANGE_TRUST_SUCCESS:
    void;
default:
    void;
};

/******* AllowTrust Result ********/

enum AllowTrustResultCode
{
    // codes considered as "success" for the operation
    ALLOW_TRUST_SUCCESS = 0,
    // codes considered as "failure" for the operation
    ALLOW_TRUST_MALFORMED = -1,     // currency is not CURRENCY_TYPE_ALPHANUM
    ALLOW_TRUST_NO_TRUST_LINE = -2, // trustor does not have a trustline
									// source account does not require trust
    ALLOW_TRUST_TRUST_NOT_REQUIRED = -3, 
    ALLOW_TRUST_CANT_REVOKE = -4    // source account can't revoke trust
};

union AllowTrustResult switch (AllowTrustResultCode code)
{
case ALLOW_TRUST_SUCCESS:
    void;
default:
    void;
};

/******* AccountMerge Result ********/

enum AccountMergeResultCode
{
    // codes considered as "success" for the operation
    ACCOUNT_MERGE_SUCCESS = 0,
    // codes considered as "failure" for the operation
    ACCOUNT_MERGE_MALFORMED = -1,  // can't merge onto itself
    ACCOUNT_MERGE_NO_ACCOUNT = -2, // destination does not exist
    ACCOUNT_MERGE_HAS_CREDIT = -3, // account has active trust lines
    ACCOUNT_MERGE_CREDIT_HELD = -4 // an issuer cannot be merged if used
};

union AccountMergeResult switch (AccountMergeResultCode code)
{
case ACCOUNT_MERGE_SUCCESS:
    void;
default:
    void;
};

/******* Inflation Result ********/

enum InflationResultCode
{
    // codes considered as "success" for the operation
    INFLATION_SUCCESS = 0,
    // codes considered as "failure" for the operation
    INFLATION_NOT_TIME = -1
};

struct inflationPayout // or use PaymentResultAtom to limit types?
{
    AccountID destination;
    int64 amount;
};

union InflationResult switch (InflationResultCode code)
{
case INFLATION_SUCCESS:
    inflationPayout payouts<>;
default:
    void;
};

/* High level Operation Result */

enum OperationResultCode
{
    opINNER = 0, // inner object result is valid

    opBAD_AUTH = -1,  // not enough signatures to perform operation
    opNO_ACCOUNT = -2 // source account was not found
};

union OperationResult switch (OperationResultCode code)
{
case opINNER:
    union switch (OperationType type)
    {
    case CREATE_ACCOUNT:
        CreateAccountResult createAccountResult;
    case PAYMENT:
        PaymentResult paymentResult;
    case PATH_PAYMENT:
        PathPaymentResult pathPaymentResult;
    case MANAGE_OFFER:
        ManageOfferResult manageOfferResult;
    case CREATE_PASSIVE_OFFER:
        ManageOfferResult createPassiveOfferResult;
    case SET_OPTIONS:
        SetOptionsResult setOptionsResult;
    case CHANGE_TRUST:
        ChangeTrustResult changeTrustResult;
    case ALLOW_TRUST:
        AllowTrustResult allowTrustResult;
    case ACCOUNT_MERGE:
        AccountMergeResult accountMergeResult;
    case INFLATION:
        InflationResult inflationResult;
    }
    tr;
default:
    void;
};

enum TransactionResultCode
{
    txSUCCESS = 0, // all operations succeeded

    txFAILED = -1, // one of the operations failed (but none were applied)

    txTOO_EARLY = -2,         // ledger closeTime before minTime
    txTOO_LATE = -3,          // ledger closeTime after maxTime
    txMISSING_OPERATION = -4, // no operation was specified
    txBAD_SEQ = -5,           // sequence number does not match source account

    txBAD_AUTH = -6,             // not enough signatures to perform transaction
    txINSUFFICIENT_BALANCE = -7, // fee would bring account below reserve
    txNO_ACCOUNT = -8,           // source account not found
    txINSUFFICIENT_FEE = -9,     // fee is too small
    txBAD_AUTH_EXTRA = -10,      // too many signatures on transaction
    txINTERNAL_ERROR = -11       // an unknown error occured
};

struct TransactionResult
{
    int64 feeCharged; // actual fee charged for the transaction

    union switch (TransactionResultCode code)
    {
    case txSUCCESS:
    case txFAILED:
        OperationResult results<>;
    default:
        void;
    }
    result;
};
}
