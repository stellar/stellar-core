%#include "generated/Stellar-ledger-entries.h"

namespace stellar {

struct DecoratedSignature
{
    opaque hint[4];     // first 4 bytes of the public key, used as a hint
    uint512 signature;  // actual signature
};

enum OperationType
{
    PAYMENT = 0,
    CREATE_OFFER = 1,
    CANCEL_OFFER = 2,
    SET_OPTIONS = 3,
    CHANGE_TRUST = 4,
    ALLOW_TRUST = 5,
    ACCOUNT_MERGE = 6,
    INFLATION = 7
};

struct PaymentOp
{
    AccountID destination;
    Currency currency;      // what they end up with
    int64 amount;           // amount they end up with
    Currency path<5>;        // what hops it must go through to get there
    int64 sendMax;          // the maximum amount of the source currency to
                            // send (excluding fees).
                            // The operation will fail if can't be met
    opaque memo<32>;
    opaque sourceMemo<32>;  // used to return a payment
};

struct CreateOfferOp
{
    Currency takerGets;
    Currency takerPays;
    int64 amount;        // amount taker gets
    Price price;         // =takerPaysAmount/takerGetsAmount

    uint64 offerID;      // set if you want to change an existing offer
    uint32 flags;        // passive: only take offers that cross this. not offers that match it
};

struct SetOptionsOp
{
    AccountID* inflationDest;
    uint32*    clearFlags;
    uint32*    setFlags;
    Thresholds* thresholds;
    Signer* signer;
};

struct ChangeTrustOp
{
    Currency line;
    int64 limit;
};

struct AllowTrustOp
{
    AccountID trustor;
    union switch(CurrencyType type)
    {
        // NATIVE is not allowed
        case ISO4217:
            opaque currencyCode[4];

        // add other currency types here in the future
    } currency;

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
        case CANCEL_OFFER:
            uint64 offerID;
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
    } body;
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

struct ClaimOfferAtom
{
    AccountID offerOwner;
    uint64 offerID;
    Currency currencyClaimed; // redundant but emited for clarity
    int64 amountClaimed;
    // should we also include the amount that the owner gets in return?
};

namespace Payment {
enum PaymentResultCode
{
    SUCCESS = 0,
    SUCCESS_MULTI = 1,
    UNDERFUNDED = 2,
    NO_DESTINATION = 3,
    MALFORMED = 4,
    NO_TRUST = 5,
    NOT_AUTHORIZED = 6,
    LINE_FULL = 7,
    OVERSENDMAX = 8
};

struct SimplePaymentResult
{
    AccountID destination;
    Currency currency;
    int64 amount;
};

struct SuccessMultiResult
{
    ClaimOfferAtom offers<>;
    SimplePaymentResult last;
};

union PaymentResult switch(PaymentResultCode code)
{
    case SUCCESS:
        void;
    case SUCCESS_MULTI:
        SuccessMultiResult multi;
    default:
        void;
};

}

namespace CreateOffer
{
enum CreateOfferResultCode
{
    SUCCESS = 0,
    NO_TRUST = 1,
    NOT_AUTHORIZED = 2,
    MALFORMED = 3,
    UNDERFUNDED = 4,
    CROSS_SELF = 5,
    NOT_FOUND = 6
};

enum CreateOfferEffect
{
    CREATED = 0,
    UPDATED = 1,
    EMPTY = 2
};

struct CreateOfferSuccessResult
{
    ClaimOfferAtom offersClaimed<>;

    union switch(CreateOfferEffect effect)
    {
        case CREATED:
            OfferEntry offerCreated;
        default:
            void;
    } offer;
};

union CreateOfferResult switch(CreateOfferResultCode code)
{
    case SUCCESS:
        CreateOfferSuccessResult success;
    default:
        void;
};

}

namespace CancelOffer
{
enum CancelOfferResultCode
{
    SUCCESS = 0,
    NOT_FOUND = 1
};

union CancelOfferResult switch(CancelOfferResultCode code)
{
    case SUCCESS:
        void;
    default:
        void;
};

}

namespace SetOptions
{
enum SetOptionsResultCode
{
    SUCCESS = 0,
    RATE_FIXED = 1,
    RATE_TOO_HIGH = 2,
    BELOW_MIN_BALANCE = 3,
    MALFORMED = 4
};

union SetOptionsResult switch(SetOptionsResultCode code)
{
    case SUCCESS:
        void;
    default:
        void;
};

}

namespace ChangeTrust
{
enum ChangeTrustResultCode
{
    SUCCESS = 0,
    NO_ACCOUNT = 1,
    INVALID_LIMIT =2
};

union ChangeTrustResult switch(ChangeTrustResultCode code)
{
    case SUCCESS:
        void;
    default:
        void;
};

}

namespace AllowTrust
{
enum AllowTrustResultCode
{
    SUCCESS = 0,
    MALFORMED = 1,
    NO_TRUST_LINE = 2
};

union AllowTrustResult switch(AllowTrustResultCode code)
{
    case SUCCESS:
        void;
    default:
        void;
};

}

namespace AccountMerge
{
enum AccountMergeResultCode
{
    SUCCESS = 0,
    MALFORMED = 1,
    NO_ACCOUNT = 2,
    HAS_CREDIT = 3
};

union AccountMergeResult switch(AccountMergeResultCode code)
{
    case SUCCESS:
        void;
    default:
        void;
};

}

namespace Inflation
{
enum InflationResultCode
{
    SUCCESS = 0,
    NOT_TIME = 1
};

struct inflationPayout // or use PaymentResultAtom to limit types?
{
    AccountID destination;
    int64 amount;
};

union InflationResult switch(InflationResultCode code)
{
    case SUCCESS:
        inflationPayout payouts<>;
    default:
        void;
};

}

enum OperationResultCode
{
    opSKIP = 0,
    opINNER = 1,

    opBAD_AUTH = 2, // not enough signatures to perform operation
    opNO_ACCOUNT = 3
};

union OperationResult switch(OperationResultCode code)
{
    case opINNER:
        union switch(OperationType type)
        {
            case PAYMENT:
                Payment::PaymentResult paymentResult;
            case CREATE_OFFER:
                CreateOffer::CreateOfferResult createOfferResult;
            case CANCEL_OFFER:
                CancelOffer::CancelOfferResult cancelOfferResult;
            case SET_OPTIONS:
                SetOptions::SetOptionsResult setOptionsResult;
            case CHANGE_TRUST:
                ChangeTrust::ChangeTrustResult changeTrustResult;
            case ALLOW_TRUST:
                AllowTrust::AllowTrustResult allowTrustResult;
            case ACCOUNT_MERGE:
                AccountMerge::AccountMergeResult accountMergeResult;
            case INFLATION:
                Inflation::InflationResult inflationResult;
        } tr;
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

    union switch(TransactionResultCode code)
    {
        case txSUCCESS:
        case txFAILED:
            OperationResult results<>;
        default:
            void;
    } result;
};

}
