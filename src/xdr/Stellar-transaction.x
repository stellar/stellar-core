// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    PAYMENT = 0,
    CREATE_OFFER = 1,
    SET_OPTIONS = 2,
    CHANGE_TRUST = 3,
    ALLOW_TRUST = 4,
    ACCOUNT_MERGE = 5,
    INFLATION = 6
};

struct PaymentOp
{
    AccountID destination; // recipient of the payment
    Currency currency;     // what they end up with
    int64 amount;          // amount they end up with

    Currency path<5>; // what hops it must go through to get there

    int64 sendMax; // the maximum amount of the source currency to
                   // send (excluding fees).
                   // The operation will fail if can't be met

    opaque memo<32>;
    opaque sourceMemo<32>; // used to return a payment
};

/* Creates or updates an offer
*/
struct CreateOfferOp
{
    Currency takerGets;
    Currency takerPays;
    int64 amount; // amount taker gets
    Price price;  // =takerPaysAmount/takerGetsAmount

    // 0=create a new offer, otherwise edit an existing offer
    uint64 offerID;
};

/* Set Account Options
    set the fields that needs to be updated
*/
struct SetOptionsOp
{
    AccountID* inflationDest; // sets the inflation

    uint32* clearFlags; // which flags to clear
    uint32* setFlags;   // which flags to set

    Thresholds* thresholds; // update the thresholds for the account

    // Add, update or remove a signer for the account
    // signer is deleted if the weight is 0
    Signer* signer;
};

/* Creates, updates or deletes a trust line
*/
struct ChangeTrustOp
{
    Currency line;

    // if limit is set to 0, deletes the trust line
    int64 limit;
};

/* Updates the "authorized" flag of an existing trust line
   this is called by the issuer of the related currency
*/
struct AllowTrustOp
{
    AccountID trustor;
    union switch (CurrencyType type)
    {
    // NATIVE is not allowed
    case ISO4217:
        opaque currencyCode[4];

        // add other currency types here in the future
    }
    currency;

    bool authorize;
};

struct Operation
{
    AccountID* sourceAccount; // defaults to the account from the transaction
    union switch (OperationType type)
    {
    case PAYMENT:
        PaymentOp paymentOp;
    case CREATE_OFFER:
        CreateOfferOp createOfferOp;
    case SET_OPTIONS:
        SetOptionsOp setOptionsOp;
    case CHANGE_TRUST:
        ChangeTrustOp changeTrustOp;
    case ALLOW_TRUST:
        AllowTrustOp allowTrustOp;
    case ACCOUNT_MERGE:
        uint256 destination;
    case INFLATION:
        uint32 inflationSeq;
    }
    body;
};

struct Transaction
{
    AccountID account;
    int32 maxFee;
    SequenceNumber seqNum;
    uint32 minLedger;
    uint32 maxLedger;

    Operation operations<100>;
};

struct TransactionEnvelope
{
    Transaction tx;
    DecoratedSignature signatures<20>;
};

/* Operation Results section */

struct ClaimOfferAtom
{
    AccountID offerOwner;
    uint64 offerID;
    Currency currencyClaimed; // redundant but emited for clarity
    int64 amountClaimed;
    // should we also include the amount that the owner gets in return?
};

/******* Payment Result ********/

enum PaymentResultCode
{
    PAYMENT_SUCCESS = 0,
    PAYMENT_SUCCESS_MULTI = 1,
    PAYMENT_UNDERFUNDED = 2,
    PAYMENT_NO_DESTINATION = 3,
    PAYMENT_MALFORMED = 4,
    PAYMENT_NO_TRUST = 5,
    PAYMENT_NOT_AUTHORIZED = 6,
    PAYMENT_LINE_FULL = 7,
    PAYMENT_OVERSENDMAX = 8
};

struct SimplePaymentResult
{
    AccountID destination;
    Currency currency;
    int64 amount;
};

struct PaymentSuccessMultiResult
{
    ClaimOfferAtom offers<>;
    SimplePaymentResult last;
};

union PaymentResult switch (PaymentResultCode code)
{
case PAYMENT_SUCCESS:
    void;
case PAYMENT_SUCCESS_MULTI:
    PaymentSuccessMultiResult multi;
default:
    void;
};

/******* CreateOffer Result ********/

enum CreateOfferResultCode
{
    CREATE_OFFER_SUCCESS = 0,
    CREATE_OFFER_NO_TRUST = 1,
    CREATE_OFFER_NOT_AUTHORIZED = 2,
    CREATE_OFFER_MALFORMED = 3,
    CREATE_OFFER_UNDERFUNDED = 4,
    CREATE_OFFER_CROSS_SELF = 5,
    CREATE_OFFER_NOT_FOUND = 6,
    CREATE_OFFER_BELOW_MIN_BALANCE = 7 // not enough funds to create a new Offer
};

enum CreateOfferEffect
{
    CREATE_OFFER_CREATED = 0,
    CREATE_OFFER_UPDATED = 1,
    CREATE_OFFER_EMPTY = 2,
	CREATE_OFFER_CANCELLED = 3
};

struct CreateOfferSuccessResult
{
    ClaimOfferAtom offersClaimed<>;

    union switch (CreateOfferEffect effect)
    {
    case CREATE_OFFER_CREATED:
        OfferEntry offerCreated;
    default:
        void;
    }
    offer;
};

union CreateOfferResult switch (CreateOfferResultCode code)
{
case CREATE_OFFER_SUCCESS:
    CreateOfferSuccessResult success;
default:
    void;
};


/******* SetOptions Result ********/

enum SetOptionsResultCode
{
    SET_OPTIONS_SUCCESS = 0,
    SET_OPTIONS_RATE_FIXED = 1,
    SET_OPTIONS_RATE_TOO_HIGH = 2,
    SET_OPTIONS_BELOW_MIN_BALANCE = 3, // not enough funds to add a signer
    SET_OPTIONS_MALFORMED = 4
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
    CHANGE_TRUST_SUCCESS = 0,
    CHANGE_TRUST_NO_ACCOUNT = 1,
    CHANGE_TRUST_INVALID_LIMIT = 2,
    CHANGE_TRUST_BELOW_MIN_BALANCE =
        3 // not enough funds to create a new trust line
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
    ALLOW_TRUST_SUCCESS = 0,
    ALLOW_TRUST_MALFORMED = 1,
    ALLOW_TRUST_NO_TRUST_LINE = 2
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
    ACCOUNT_MERGE_SUCCESS = 0,
    ACCOUNT_MERGE_MALFORMED = 1,
    ACCOUNT_MERGE_NO_ACCOUNT = 2,
    ACCOUNT_MERGE_HAS_CREDIT = 3
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
    INFLATION_SUCCESS = 0,
    INFLATION_NOT_TIME = 1
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

    opBAD_AUTH = 1,  // not enough signatures to perform operation
    opNO_ACCOUNT = 2 // account was not found
};

union OperationResult switch (OperationResultCode code)
{
case opINNER:
    union switch (OperationType type)
    {
    case PAYMENT:
        PaymentResult paymentResult;
    case CREATE_OFFER:
        CreateOfferResult createOfferResult;
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
    txSUCCESS = 0,
    txFAILED = 1,
    txBAD_LEDGER = 2,
    txDUPLICATE = 3,
    txMALFORMED = 4,
    txBAD_SEQ = 5,

    txBAD_AUTH = 6, // not enough signatures to perform transaction
    txINSUFFICIENT_BALANCE = 7,
    txNO_ACCOUNT = 8,
    txINSUFFICIENT_FEE = 9 // max fee is too small
};

struct TransactionResult
{
    int64 feeCharged;

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
