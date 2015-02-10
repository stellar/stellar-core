%#include "generated/Stellar-ledger-entries.h"

namespace stellar {

enum TransactionType
{
    PAYMENT,
    CREATE_OFFER,
    CANCEL_OFFER,
    SET_OPTIONS,
    CHANGE_TRUST,
    ALLOW_TRUST,
    ACCOUNT_MERGE,
    INFLATION
};

struct PaymentTx
{
    AccountID destination;  
    Currency currency;            // what they end up with
    int64 amount;                // amount they end up with
    Currency path<>;            // what hops it must go through to get there
    int64 sendMax;                // the maximum amount of the source currency this
                                // will send. The tx will fail if can't be met
    opaque memo<32>;
    opaque sourceMemo<32>;        // used to return a payment
};

// Create an offer to sell amountSell of currency "sell"
// in exchange to buy amountBuy of currency "buy"
struct CreateOfferTx
{
    Currency sell;
    int64 amountSell;

    Currency buy;
    int64 amountBuy;

    uint32 sequence;    // set if you want to change an existing offer
};

struct SetOptionsTx
{
    AccountID* inflationDest;
    uint32*    clearFlags;
    uint32*    setFlags;
    KeyValue* data;
    Thresholds* thresholds;
    Signer* signer;
};

struct ChangeTrustTx
{
    Currency line;
    int64 limit;
};

struct AllowTrustTx
{
    AccountID trustor;
    union switch(CurrencyType type)
    {
        case NATIVE:
            void;

        case ISO4217:
            opaque currencyCode[4];

        // add other currency types here in the future
    } code;

    bool authorize;
};

struct Transaction
{
    AccountID account;
    int32 maxFee;
    uint32 seqNum;
    uint64 maxLedger;    // maximum ledger this tx is valid to be applied in
    uint64 minLedger;   // minimum ledger this tx is valid to be applied in

    union switch (TransactionType type)
    {
        case PAYMENT:
            PaymentTx paymentTx;
        case CREATE_OFFER:
            CreateOfferTx createOfferTx;
        case CANCEL_OFFER:
            uint32 offerSeqNum;
        case SET_OPTIONS:
            SetOptionsTx setOptionsTx;
        case CHANGE_TRUST:
            ChangeTrustTx changeTrustTx;
        case ALLOW_TRUST:
            AllowTrustTx allowTrustTx;
        case ACCOUNT_MERGE:
            uint256 destination;
        case INFLATION:
            uint32 inflationSeq;
    } body;
};

struct TransactionEnvelope
{
    Transaction tx;
    uint512 signatures<>;
};

struct ClaimOfferAtom
{
    AccountID offerOwner;
    uint32 offerSequence;
    Currency currencyClaimed; // redundant but emited for clarity
    int64 amountClaimed;
    // should we also include the amount that the owner gets in return?
};

namespace Payment {
enum PaymentResultCode
{
    SUCCESS,
    SUCCESS_MULTI,
    UNDERFUNDED,
    NO_DESTINATION,
    MALFORMED,
    NO_TRUST,
    NOT_AUTHORIZED,
    LINE_FULL,
    OVERSENDMAX
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

struct PaymentResult
{
    union switch(PaymentResultCode code) // ideally could collapse with TransactionResult
    {
        case SUCCESS:
            void;
        case SUCCESS_MULTI:
            SuccessMultiResult multi;
        default:
            void;
    } result;
};
}

namespace CreateOffer
{
enum CreateOfferResultCode
{
    SUCCESS,
    NO_TRUST,
    NOT_AUTHORIZED,
    MALFORMED,
    UNDERFUNDED,
    NOT_FOUND
};

struct CreateOfferSuccessResult
{
    ClaimOfferAtom offersClaimed<>;
    OfferEntry *offerCreated;
};

struct CreateOfferResult
{
    union switch(CreateOfferResultCode code)
    {
        case SUCCESS:
            CreateOfferSuccessResult success;
        default:
            void;
    } result;
};
}

namespace CancelOffer
{
enum CancelOfferResultCode
{
    SUCCESS,
    NOT_FOUND
};

struct CancelOfferResult
{
    union switch(CancelOfferResultCode code)
    {
        case SUCCESS:
            void;
        default:
            void;
    } result;
};
}

namespace SetOptions
{
enum SetOptionsResultCode
{
    SUCCESS,
    RATE_FIXED,
    RATE_TOO_HIGH,
    BELOW_MIN_BALANCE,
    MALFORMED
};

struct SetOptionsResult
{
    union switch(SetOptionsResultCode code)
    {
        case SUCCESS:
            void;
        default:
            void;
    } result;
};
}

namespace ChangeTrust
{
enum ChangeTrustResultCode
{
    SUCCESS,
    NO_ACCOUNT
};

struct ChangeTrustResult
{
    union switch(ChangeTrustResultCode code)
    {
        case SUCCESS:
            void;
        default:
            void;
    } result;
};
}

namespace AllowTrust
{
enum AllowTrustResultCode
{
    SUCCESS,
    MALFORMED,
    NO_TRUST_LINE
};

struct AllowTrustResult
{
    union switch(AllowTrustResultCode code)
    {
        case SUCCESS:
            void;
        default:
            void;
    } result;
};
}

namespace AccountMerge
{
enum AccountMergeResultCode
{
    SUCCESS,
    MALFORMED,
    NO_ACCOUNT,
    HAS_CREDIT
};

struct AccountMergeResult
{
    union switch(AccountMergeResultCode code)
    {
        case SUCCESS:
            void;
        default:
            void;
    } result;
};
}

namespace Inflation
{
enum InflationResultCode
{
    SUCCESS,
    NOT_TIME
};

struct inflationPayout // or use PaymentResultAtom to limit types?
{
    AccountID destination;
    int64 amount;
};

struct InflationResult
{
    union switch(InflationResultCode code)
    {
        case SUCCESS:
            inflationPayout payouts<>;
        default:
            void;
    } result;
};
}

enum TransactionResultCode
{
    txINNER,
    txINTERNAL_ERROR,
    txBAD_AUTH,
    txBAD_SEQ, // maybe PRE_SEQ, PAST_SEQ
    txBAD_LEDGER,
    txNO_FEE,
    txNO_ACCOUNT,
    txINSUFFICIENT_FEE
};

struct TransactionResult
{
    uint64 feeCharged;
    union switch(TransactionResultCode code)
    {
        case txINNER:
            union switch(TransactionType type)
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
    } body;
};

}
