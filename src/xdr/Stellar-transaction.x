// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

// Source or destination of a payment operation
union MuxedAccount switch (CryptoKeyType type)
{
case KEY_TYPE_ED25519:
    uint256 ed25519;
case KEY_TYPE_MUXED_ED25519:
    struct
    {
        uint64 id;
        uint256 ed25519;
    } med25519;
};

struct DecoratedSignature
{
    SignatureHint hint;  // last 4 bytes of the public key, used as a hint
    Signature signature; // actual signature
};

enum OperationType
{
    CREATE_ACCOUNT = 0,
    PAYMENT = 1,
    PATH_PAYMENT_STRICT_RECEIVE = 2,
    MANAGE_SELL_OFFER = 3,
    CREATE_PASSIVE_SELL_OFFER = 4,
    SET_OPTIONS = 5,
    CHANGE_TRUST = 6,
    ALLOW_TRUST = 7,
    ACCOUNT_MERGE = 8,
    INFLATION = 9,
    MANAGE_DATA = 10,
    BUMP_SEQUENCE = 11,
    MANAGE_BUY_OFFER = 12,
    PATH_PAYMENT_STRICT_SEND = 13,
    CREATE_CLAIMABLE_BALANCE = 14,
    CLAIM_CLAIMABLE_BALANCE = 15,
    BEGIN_SPONSORING_FUTURE_RESERVES = 16,
    END_SPONSORING_FUTURE_RESERVES = 17,
    REVOKE_SPONSORSHIP = 18,
    CLAWBACK = 19,
    CLAWBACK_CLAIMABLE_BALANCE = 20,
    SET_TRUST_LINE_FLAGS = 21
};

/* CreateAccount
Creates and funds a new account with the specified starting balance.

Threshold: med

Result: CreateAccountResult

*/
struct CreateAccountOp
{
    AccountID destination; // account to create
    int64 startingBalance; // amount they end up with
};

/* Payment

    Send an amount in specified asset to a destination account.

    Threshold: med

    Result: PaymentResult
*/
struct PaymentOp
{
    MuxedAccount destination; // recipient of the payment
    Asset asset;              // what they end up with
    int64 amount;             // amount they end up with
};

/* PathPaymentStrictReceive

send an amount to a destination account through a path.
(up to sendMax, sendAsset)
(X0, Path[0]) .. (Xn, Path[n])
(destAmount, destAsset)

Threshold: med

Result: PathPaymentStrictReceiveResult
*/
struct PathPaymentStrictReceiveOp
{
    Asset sendAsset; // asset we pay with
    int64 sendMax;   // the maximum amount of sendAsset to
                     // send (excluding fees).
                     // The operation will fail if can't be met

    MuxedAccount destination; // recipient of the payment
    Asset destAsset;          // what they end up with
    int64 destAmount;         // amount they end up with

    Asset path<5>; // additional hops it must go through to get there
};

/* PathPaymentStrictSend

send an amount to a destination account through a path.
(sendMax, sendAsset)
(X0, Path[0]) .. (Xn, Path[n])
(at least destAmount, destAsset)

Threshold: med

Result: PathPaymentStrictSendResult
*/
struct PathPaymentStrictSendOp
{
    Asset sendAsset;  // asset we pay with
    int64 sendAmount; // amount of sendAsset to send (excluding fees)

    MuxedAccount destination; // recipient of the payment
    Asset destAsset;          // what they end up with
    int64 destMin;            // the minimum amount of dest asset to
                              // be received
                              // The operation will fail if it can't be met

    Asset path<5>; // additional hops it must go through to get there
};

/* Creates, updates or deletes an offer

Threshold: med

Result: ManageSellOfferResult

*/
struct ManageSellOfferOp
{
    Asset selling;
    Asset buying;
    int64 amount; // amount being sold. if set to 0, delete the offer
    Price price;  // price of thing being sold in terms of what you are buying

    // 0=create a new offer, otherwise edit an existing offer
    int64 offerID;
};

/* Creates, updates or deletes an offer with amount in terms of buying asset

Threshold: med

Result: ManageBuyOfferResult

*/
struct ManageBuyOfferOp
{
    Asset selling;
    Asset buying;
    int64 buyAmount; // amount being bought. if set to 0, delete the offer
    Price price;     // price of thing being bought in terms of what you are
                     // selling

    // 0=create a new offer, otherwise edit an existing offer
    int64 offerID;
};

/* Creates an offer that doesn't take offers of the same price

Threshold: med

Result: CreatePassiveSellOfferResult

*/
struct CreatePassiveSellOfferOp
{
    Asset selling; // A
    Asset buying;  // B
    int64 amount;  // amount taker gets
    Price price;   // cost of A in terms of B
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

    // account threshold manipulation
    uint32* masterWeight; // weight of the master account
    uint32* lowThreshold;
    uint32* medThreshold;
    uint32* highThreshold;

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
    Asset line;

    // if limit is set to 0, deletes the trust line
    int64 limit;
};

/* Updates the "authorized" flag of an existing trust line
   this is called by the issuer of the related asset.

   note that authorize can only be set (and not cleared) if
   the issuer account does not have the AUTH_REVOCABLE_FLAG set
   Threshold: low

   Result: AllowTrustResult
*/
struct AllowTrustOp
{
    AccountID trustor;
    AssetCode asset;

    // One of 0, AUTHORIZED_FLAG, or AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG
    uint32 authorize;
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

/* ManageData
    Adds, Updates, or Deletes a key value pair associated with a particular
        account.

    Threshold: med

    Result: ManageDataResult
*/
struct ManageDataOp
{
    string64 dataName;
    DataValue* dataValue; // set to null to clear
};

/* Bump Sequence

    increases the sequence to a given level

    Threshold: low

    Result: BumpSequenceResult
*/
struct BumpSequenceOp
{
    SequenceNumber bumpTo;
};

/* Creates a claimable balance entry

    Threshold: med

    Result: CreateClaimableBalanceResult
*/
struct CreateClaimableBalanceOp
{
    Asset asset;
    int64 amount;
    Claimant claimants<10>;
};

/* Claims a claimable balance entry

    Threshold: low

    Result: ClaimClaimableBalanceResult
*/
struct ClaimClaimableBalanceOp
{
    ClaimableBalanceID balanceID;
};

/* BeginSponsoringFutureReserves

    Establishes the is-sponsoring-future-reserves-for relationship between
    the source account and sponsoredID

    Threshold: med

    Result: BeginSponsoringFutureReservesResult
*/
struct BeginSponsoringFutureReservesOp
{
    AccountID sponsoredID;
};

/* EndSponsoringFutureReserves

    Terminates the current is-sponsoring-future-reserves-for relationship in
    which source account is sponsored

    Threshold: med

    Result: EndSponsoringFutureReservesResult
*/
// EndSponsoringFutureReserves is empty

/* RevokeSponsorship

    If source account is not sponsored or is sponsored by the owner of the
    specified entry or sub-entry, then attempt to revoke the sponsorship.
    If source account is sponsored, then attempt to transfer the sponsorship
    to the sponsor of source account.

    Threshold: med

    Result: RevokeSponsorshipResult
*/
enum RevokeSponsorshipType
{
    REVOKE_SPONSORSHIP_LEDGER_ENTRY = 0,
    REVOKE_SPONSORSHIP_SIGNER = 1
};

union RevokeSponsorshipOp switch (RevokeSponsorshipType type)
{
case REVOKE_SPONSORSHIP_LEDGER_ENTRY:
    LedgerKey ledgerKey;
case REVOKE_SPONSORSHIP_SIGNER:
    struct
    {
        AccountID accountID;
        SignerKey signerKey;
    } signer;
};

/* Claws back an amount of an asset from an account

    Threshold: med

    Result: ClawbackResult
*/
struct ClawbackOp
{
    Asset asset;
    MuxedAccount from;
    int64 amount;
};

/* Claws back a claimable balance

    Threshold: med

    Result: ClawbackClaimableBalanceResult
*/
struct ClawbackClaimableBalanceOp
{
    ClaimableBalanceID balanceID;
};

/* SetTrustLineFlagsOp

   Updates the flags of an existing trust line.
   This is called by the issuer of the related asset.

   Threshold: low

   Result: SetTrustLineFlagsResult
*/
struct SetTrustLineFlagsOp
{
    AccountID trustor;
    Asset asset;

    uint32 clearFlags; // which flags to clear
    uint32 setFlags;   // which flags to set
};

/* An operation is the lowest unit of work that a transaction does */
struct Operation
{
    // sourceAccount is the account used to run the operation
    // if not set, the runtime defaults to "sourceAccount" specified at
    // the transaction level
    MuxedAccount* sourceAccount;

    union switch (OperationType type)
    {
    case CREATE_ACCOUNT:
        CreateAccountOp createAccountOp;
    case PAYMENT:
        PaymentOp paymentOp;
    case PATH_PAYMENT_STRICT_RECEIVE:
        PathPaymentStrictReceiveOp pathPaymentStrictReceiveOp;
    case MANAGE_SELL_OFFER:
        ManageSellOfferOp manageSellOfferOp;
    case CREATE_PASSIVE_SELL_OFFER:
        CreatePassiveSellOfferOp createPassiveSellOfferOp;
    case SET_OPTIONS:
        SetOptionsOp setOptionsOp;
    case CHANGE_TRUST:
        ChangeTrustOp changeTrustOp;
    case ALLOW_TRUST:
        AllowTrustOp allowTrustOp;
    case ACCOUNT_MERGE:
        MuxedAccount destination;
    case INFLATION:
        void;
    case MANAGE_DATA:
        ManageDataOp manageDataOp;
    case BUMP_SEQUENCE:
        BumpSequenceOp bumpSequenceOp;
    case MANAGE_BUY_OFFER:
        ManageBuyOfferOp manageBuyOfferOp;
    case PATH_PAYMENT_STRICT_SEND:
        PathPaymentStrictSendOp pathPaymentStrictSendOp;
    case CREATE_CLAIMABLE_BALANCE:
        CreateClaimableBalanceOp createClaimableBalanceOp;
    case CLAIM_CLAIMABLE_BALANCE:
        ClaimClaimableBalanceOp claimClaimableBalanceOp;
    case BEGIN_SPONSORING_FUTURE_RESERVES:
        BeginSponsoringFutureReservesOp beginSponsoringFutureReservesOp;
    case END_SPONSORING_FUTURE_RESERVES:
        void;
    case REVOKE_SPONSORSHIP:
        RevokeSponsorshipOp revokeSponsorshipOp;
    case CLAWBACK:
        ClawbackOp clawbackOp;
    case CLAWBACK_CLAIMABLE_BALANCE:
        ClawbackClaimableBalanceOp clawbackClaimableBalanceOp;
    case SET_TRUST_LINE_FLAGS:
        SetTrustLineFlagsOp setTrustLineFlagsOp;
    }
    body;
};

union OperationID switch (EnvelopeType type)
{
case ENVELOPE_TYPE_OP_ID:
    struct
    {
        MuxedAccount sourceAccount;
        SequenceNumber seqNum;
        uint32 opNum;
    } id;
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
    TimePoint minTime;
    TimePoint maxTime; // 0 here means no maxTime
};

// maximum number of operations per transaction
const MAX_OPS_PER_TX = 100;

// TransactionV0 is a transaction with the AccountID discriminant stripped off,
// leaving a raw ed25519 public key to identify the source account. This is used
// for backwards compatibility starting from the protocol 12/13 boundary. If an
// "old-style" TransactionEnvelope containing a Transaction is parsed with this
// XDR definition, it will be parsed as a "new-style" TransactionEnvelope
// containing a TransactionV0.
struct TransactionV0
{
    uint256 sourceAccountEd25519;
    uint32 fee;
    SequenceNumber seqNum;
    TimeBounds* timeBounds;
    Memo memo;
    Operation operations<MAX_OPS_PER_TX>;
    union switch (int v)
    {
    case 0:
        void;
    }
    ext;
};

struct TransactionV0Envelope
{
    TransactionV0 tx;
    /* Each decorated signature is a signature over the SHA256 hash of
     * a TransactionSignaturePayload */
    DecoratedSignature signatures<20>;
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
    MuxedAccount sourceAccount;

    // the fee the sourceAccount will pay
    uint32 fee;

    // sequence number to consume in the account
    SequenceNumber seqNum;

    // validity range (inclusive) for the last ledger close time
    TimeBounds* timeBounds;

    Memo memo;

    Operation operations<MAX_OPS_PER_TX>;

    // reserved for future use
    union switch (int v)
    {
    case 0:
        void;
    }
    ext;
};

struct TransactionV1Envelope
{
    Transaction tx;
    /* Each decorated signature is a signature over the SHA256 hash of
     * a TransactionSignaturePayload */
    DecoratedSignature signatures<20>;
};

struct FeeBumpTransaction
{
    MuxedAccount feeSource;
    int64 fee;
    union switch (EnvelopeType type)
    {
    case ENVELOPE_TYPE_TX:
        TransactionV1Envelope v1;
    }
    innerTx;
    union switch (int v)
    {
    case 0:
        void;
    }
    ext;
};

struct FeeBumpTransactionEnvelope
{
    FeeBumpTransaction tx;
    /* Each decorated signature is a signature over the SHA256 hash of
     * a TransactionSignaturePayload */
    DecoratedSignature signatures<20>;
};

/* A TransactionEnvelope wraps a transaction with signatures. */
union TransactionEnvelope switch (EnvelopeType type)
{
case ENVELOPE_TYPE_TX_V0:
    TransactionV0Envelope v0;
case ENVELOPE_TYPE_TX:
    TransactionV1Envelope v1;
case ENVELOPE_TYPE_TX_FEE_BUMP:
    FeeBumpTransactionEnvelope feeBump;
};

struct TransactionSignaturePayload
{
    Hash networkId;
    union switch (EnvelopeType type)
    {
    // Backwards Compatibility: Use ENVELOPE_TYPE_TX to sign ENVELOPE_TYPE_TX_V0
    case ENVELOPE_TYPE_TX:
        Transaction tx;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
        FeeBumpTransaction feeBump;
    }
    taggedTransaction;
};

/* Operation Results section */

/* This result is used when offers are taken during an operation */
struct ClaimOfferAtom
{
    // emitted to identify the offer
    AccountID sellerID; // Account that owns the offer
    int64 offerID;

    // amount and asset taken from the owner
    Asset assetSold;
    int64 amountSold;

    // amount and asset sent to the owner
    Asset assetBought;
    int64 amountBought;
};

/******* CreateAccount Result ********/

enum CreateAccountResultCode
{
    // codes considered as "success" for the operation
    CREATE_ACCOUNT_SUCCESS = 0, // account was created

    // codes considered as "failure" for the operation
    CREATE_ACCOUNT_MALFORMED = -1,   // invalid destination
    CREATE_ACCOUNT_UNDERFUNDED = -2, // not enough funds in source account
    CREATE_ACCOUNT_LOW_RESERVE =
        -3, // would create an account below the min reserve
    CREATE_ACCOUNT_ALREADY_EXIST = -4 // account already exists
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
    PAYMENT_MALFORMED = -1,          // bad input
    PAYMENT_UNDERFUNDED = -2,        // not enough funds in source account
    PAYMENT_SRC_NO_TRUST = -3,       // no trust line on source account
    PAYMENT_SRC_NOT_AUTHORIZED = -4, // source not authorized to transfer
    PAYMENT_NO_DESTINATION = -5,     // destination account does not exist
    PAYMENT_NO_TRUST = -6,       // destination missing a trust line for asset
    PAYMENT_NOT_AUTHORIZED = -7, // destination not authorized to hold asset
    PAYMENT_LINE_FULL = -8,      // destination would go above their limit
    PAYMENT_NO_ISSUER = -9       // missing issuer on asset
};

union PaymentResult switch (PaymentResultCode code)
{
case PAYMENT_SUCCESS:
    void;
default:
    void;
};

/******* PathPaymentStrictReceive Result ********/

enum PathPaymentStrictReceiveResultCode
{
    // codes considered as "success" for the operation
    PATH_PAYMENT_STRICT_RECEIVE_SUCCESS = 0, // success

    // codes considered as "failure" for the operation
    PATH_PAYMENT_STRICT_RECEIVE_MALFORMED = -1, // bad input
    PATH_PAYMENT_STRICT_RECEIVE_UNDERFUNDED =
        -2, // not enough funds in source account
    PATH_PAYMENT_STRICT_RECEIVE_SRC_NO_TRUST =
        -3, // no trust line on source account
    PATH_PAYMENT_STRICT_RECEIVE_SRC_NOT_AUTHORIZED =
        -4, // source not authorized to transfer
    PATH_PAYMENT_STRICT_RECEIVE_NO_DESTINATION =
        -5, // destination account does not exist
    PATH_PAYMENT_STRICT_RECEIVE_NO_TRUST =
        -6, // dest missing a trust line for asset
    PATH_PAYMENT_STRICT_RECEIVE_NOT_AUTHORIZED =
        -7, // dest not authorized to hold asset
    PATH_PAYMENT_STRICT_RECEIVE_LINE_FULL =
        -8, // dest would go above their limit
    PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER = -9, // missing issuer on one asset
    PATH_PAYMENT_STRICT_RECEIVE_TOO_FEW_OFFERS =
        -10, // not enough offers to satisfy path
    PATH_PAYMENT_STRICT_RECEIVE_OFFER_CROSS_SELF =
        -11, // would cross one of its own offers
    PATH_PAYMENT_STRICT_RECEIVE_OVER_SENDMAX = -12 // could not satisfy sendmax
};

struct SimplePaymentResult
{
    AccountID destination;
    Asset asset;
    int64 amount;
};

union PathPaymentStrictReceiveResult switch (
    PathPaymentStrictReceiveResultCode code)
{
case PATH_PAYMENT_STRICT_RECEIVE_SUCCESS:
    struct
    {
        ClaimOfferAtom offers<>;
        SimplePaymentResult last;
    } success;
case PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER:
    Asset noIssuer; // the asset that caused the error
default:
    void;
};

/******* PathPaymentStrictSend Result ********/

enum PathPaymentStrictSendResultCode
{
    // codes considered as "success" for the operation
    PATH_PAYMENT_STRICT_SEND_SUCCESS = 0, // success

    // codes considered as "failure" for the operation
    PATH_PAYMENT_STRICT_SEND_MALFORMED = -1, // bad input
    PATH_PAYMENT_STRICT_SEND_UNDERFUNDED =
        -2, // not enough funds in source account
    PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST =
        -3, // no trust line on source account
    PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED =
        -4, // source not authorized to transfer
    PATH_PAYMENT_STRICT_SEND_NO_DESTINATION =
        -5, // destination account does not exist
    PATH_PAYMENT_STRICT_SEND_NO_TRUST =
        -6, // dest missing a trust line for asset
    PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED =
        -7, // dest not authorized to hold asset
    PATH_PAYMENT_STRICT_SEND_LINE_FULL = -8, // dest would go above their limit
    PATH_PAYMENT_STRICT_SEND_NO_ISSUER = -9, // missing issuer on one asset
    PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS =
        -10, // not enough offers to satisfy path
    PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF =
        -11, // would cross one of its own offers
    PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN = -12 // could not satisfy destMin
};

union PathPaymentStrictSendResult switch (PathPaymentStrictSendResultCode code)
{
case PATH_PAYMENT_STRICT_SEND_SUCCESS:
    struct
    {
        ClaimOfferAtom offers<>;
        SimplePaymentResult last;
    } success;
case PATH_PAYMENT_STRICT_SEND_NO_ISSUER:
    Asset noIssuer; // the asset that caused the error
default:
    void;
};

/******* ManageSellOffer Result ********/

enum ManageSellOfferResultCode
{
    // codes considered as "success" for the operation
    MANAGE_SELL_OFFER_SUCCESS = 0,

    // codes considered as "failure" for the operation
    MANAGE_SELL_OFFER_MALFORMED = -1, // generated offer would be invalid
    MANAGE_SELL_OFFER_SELL_NO_TRUST =
        -2,                              // no trust line for what we're selling
    MANAGE_SELL_OFFER_BUY_NO_TRUST = -3, // no trust line for what we're buying
    MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED = -4, // not authorized to sell
    MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED = -5,  // not authorized to buy
    MANAGE_SELL_OFFER_LINE_FULL = -6, // can't receive more of what it's buying
    MANAGE_SELL_OFFER_UNDERFUNDED = -7, // doesn't hold what it's trying to sell
    MANAGE_SELL_OFFER_CROSS_SELF =
        -8, // would cross an offer from the same user
    MANAGE_SELL_OFFER_SELL_NO_ISSUER = -9, // no issuer for what we're selling
    MANAGE_SELL_OFFER_BUY_NO_ISSUER = -10, // no issuer for what we're buying

    // update errors
    MANAGE_SELL_OFFER_NOT_FOUND =
        -11, // offerID does not match an existing offer

    MANAGE_SELL_OFFER_LOW_RESERVE =
        -12 // not enough funds to create a new Offer
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

union ManageSellOfferResult switch (ManageSellOfferResultCode code)
{
case MANAGE_SELL_OFFER_SUCCESS:
    ManageOfferSuccessResult success;
default:
    void;
};

/******* ManageBuyOffer Result ********/

enum ManageBuyOfferResultCode
{
    // codes considered as "success" for the operation
    MANAGE_BUY_OFFER_SUCCESS = 0,

    // codes considered as "failure" for the operation
    MANAGE_BUY_OFFER_MALFORMED = -1,     // generated offer would be invalid
    MANAGE_BUY_OFFER_SELL_NO_TRUST = -2, // no trust line for what we're selling
    MANAGE_BUY_OFFER_BUY_NO_TRUST = -3,  // no trust line for what we're buying
    MANAGE_BUY_OFFER_SELL_NOT_AUTHORIZED = -4, // not authorized to sell
    MANAGE_BUY_OFFER_BUY_NOT_AUTHORIZED = -5,  // not authorized to buy
    MANAGE_BUY_OFFER_LINE_FULL = -6,   // can't receive more of what it's buying
    MANAGE_BUY_OFFER_UNDERFUNDED = -7, // doesn't hold what it's trying to sell
    MANAGE_BUY_OFFER_CROSS_SELF = -8, // would cross an offer from the same user
    MANAGE_BUY_OFFER_SELL_NO_ISSUER = -9, // no issuer for what we're selling
    MANAGE_BUY_OFFER_BUY_NO_ISSUER = -10, // no issuer for what we're buying

    // update errors
    MANAGE_BUY_OFFER_NOT_FOUND =
        -11, // offerID does not match an existing offer

    MANAGE_BUY_OFFER_LOW_RESERVE = -12 // not enough funds to create a new Offer
};

union ManageBuyOfferResult switch (ManageBuyOfferResultCode code)
{
case MANAGE_BUY_OFFER_SUCCESS:
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
    SET_OPTIONS_INVALID_INFLATION = -4,      // inflation account does not exist
    SET_OPTIONS_CANT_CHANGE = -5,            // can no longer change this option
    SET_OPTIONS_UNKNOWN_FLAG = -6,           // can't set an unknown flag
    SET_OPTIONS_THRESHOLD_OUT_OF_RANGE = -7, // bad value for weight/threshold
    SET_OPTIONS_BAD_SIGNER = -8,             // signer cannot be masterkey
    SET_OPTIONS_INVALID_HOME_DOMAIN = -9,    // malformed home domain
    SET_OPTIONS_AUTH_REVOCABLE_REQUIRED =
        -10 // auth revocable is required for clawback
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
                                     // cannot create with a limit of 0
    CHANGE_TRUST_LOW_RESERVE =
        -4, // not enough funds to create a new trust line,
    CHANGE_TRUST_SELF_NOT_ALLOWED = -5 // trusting self is not allowed
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
    ALLOW_TRUST_MALFORMED = -1,     // asset is not ASSET_TYPE_ALPHANUM
    ALLOW_TRUST_NO_TRUST_LINE = -2, // trustor does not have a trustline
                                    // source account does not require trust
    ALLOW_TRUST_TRUST_NOT_REQUIRED = -3,
    ALLOW_TRUST_CANT_REVOKE = -4,     // source account can't revoke trust,
    ALLOW_TRUST_SELF_NOT_ALLOWED = -5 // trusting self is not allowed
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
    ACCOUNT_MERGE_MALFORMED = -1,       // can't merge onto itself
    ACCOUNT_MERGE_NO_ACCOUNT = -2,      // destination does not exist
    ACCOUNT_MERGE_IMMUTABLE_SET = -3,   // source account has AUTH_IMMUTABLE set
    ACCOUNT_MERGE_HAS_SUB_ENTRIES = -4, // account has trust lines/offers
    ACCOUNT_MERGE_SEQNUM_TOO_FAR = -5,  // sequence number is over max allowed
    ACCOUNT_MERGE_DEST_FULL = -6,       // can't add source balance to
                                        // destination balance
    ACCOUNT_MERGE_IS_SPONSOR = -7       // can't merge account that is a sponsor
};

union AccountMergeResult switch (AccountMergeResultCode code)
{
case ACCOUNT_MERGE_SUCCESS:
    int64 sourceAccountBalance; // how much got transfered from source account
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

struct InflationPayout // or use PaymentResultAtom to limit types?
{
    AccountID destination;
    int64 amount;
};

union InflationResult switch (InflationResultCode code)
{
case INFLATION_SUCCESS:
    InflationPayout payouts<>;
default:
    void;
};

/******* ManageData Result ********/

enum ManageDataResultCode
{
    // codes considered as "success" for the operation
    MANAGE_DATA_SUCCESS = 0,
    // codes considered as "failure" for the operation
    MANAGE_DATA_NOT_SUPPORTED_YET =
        -1, // The network hasn't moved to this protocol change yet
    MANAGE_DATA_NAME_NOT_FOUND =
        -2, // Trying to remove a Data Entry that isn't there
    MANAGE_DATA_LOW_RESERVE = -3, // not enough funds to create a new Data Entry
    MANAGE_DATA_INVALID_NAME = -4 // Name not a valid string
};

union ManageDataResult switch (ManageDataResultCode code)
{
case MANAGE_DATA_SUCCESS:
    void;
default:
    void;
};

/******* BumpSequence Result ********/

enum BumpSequenceResultCode
{
    // codes considered as "success" for the operation
    BUMP_SEQUENCE_SUCCESS = 0,
    // codes considered as "failure" for the operation
    BUMP_SEQUENCE_BAD_SEQ = -1 // `bumpTo` is not within bounds
};

union BumpSequenceResult switch (BumpSequenceResultCode code)
{
case BUMP_SEQUENCE_SUCCESS:
    void;
default:
    void;
};

/******* CreateClaimableBalance Result ********/

enum CreateClaimableBalanceResultCode
{
    CREATE_CLAIMABLE_BALANCE_SUCCESS = 0,
    CREATE_CLAIMABLE_BALANCE_MALFORMED = -1,
    CREATE_CLAIMABLE_BALANCE_LOW_RESERVE = -2,
    CREATE_CLAIMABLE_BALANCE_NO_TRUST = -3,
    CREATE_CLAIMABLE_BALANCE_NOT_AUTHORIZED = -4,
    CREATE_CLAIMABLE_BALANCE_UNDERFUNDED = -5
};

union CreateClaimableBalanceResult switch (
    CreateClaimableBalanceResultCode code)
{
case CREATE_CLAIMABLE_BALANCE_SUCCESS:
    ClaimableBalanceID balanceID;
default:
    void;
};

/******* ClaimClaimableBalance Result ********/

enum ClaimClaimableBalanceResultCode
{
    CLAIM_CLAIMABLE_BALANCE_SUCCESS = 0,
    CLAIM_CLAIMABLE_BALANCE_DOES_NOT_EXIST = -1,
    CLAIM_CLAIMABLE_BALANCE_CANNOT_CLAIM = -2,
    CLAIM_CLAIMABLE_BALANCE_LINE_FULL = -3,
    CLAIM_CLAIMABLE_BALANCE_NO_TRUST = -4,
    CLAIM_CLAIMABLE_BALANCE_NOT_AUTHORIZED = -5

};

union ClaimClaimableBalanceResult switch (ClaimClaimableBalanceResultCode code)
{
case CLAIM_CLAIMABLE_BALANCE_SUCCESS:
    void;
default:
    void;
};

/******* BeginSponsoringFutureReserves Result ********/

enum BeginSponsoringFutureReservesResultCode
{
    // codes considered as "success" for the operation
    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS = 0,

    // codes considered as "failure" for the operation
    BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED = -1,
    BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED = -2,
    BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE = -3
};

union BeginSponsoringFutureReservesResult switch (
    BeginSponsoringFutureReservesResultCode code)
{
case BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS:
    void;
default:
    void;
};

/******* EndSponsoringFutureReserves Result ********/

enum EndSponsoringFutureReservesResultCode
{
    // codes considered as "success" for the operation
    END_SPONSORING_FUTURE_RESERVES_SUCCESS = 0,

    // codes considered as "failure" for the operation
    END_SPONSORING_FUTURE_RESERVES_NOT_SPONSORED = -1
};

union EndSponsoringFutureReservesResult switch (
    EndSponsoringFutureReservesResultCode code)
{
case END_SPONSORING_FUTURE_RESERVES_SUCCESS:
    void;
default:
    void;
};

/******* RevokeSponsorship Result ********/

enum RevokeSponsorshipResultCode
{
    // codes considered as "success" for the operation
    REVOKE_SPONSORSHIP_SUCCESS = 0,

    // codes considered as "failure" for the operation
    REVOKE_SPONSORSHIP_DOES_NOT_EXIST = -1,
    REVOKE_SPONSORSHIP_NOT_SPONSOR = -2,
    REVOKE_SPONSORSHIP_LOW_RESERVE = -3,
    REVOKE_SPONSORSHIP_ONLY_TRANSFERABLE = -4
};

union RevokeSponsorshipResult switch (RevokeSponsorshipResultCode code)
{
case REVOKE_SPONSORSHIP_SUCCESS:
    void;
default:
    void;
};

/******* Clawback Result ********/

enum ClawbackResultCode
{
    // codes considered as "success" for the operation
    CLAWBACK_SUCCESS = 0,

    // codes considered as "failure" for the operation
    CLAWBACK_MALFORMED = -1,
    CLAWBACK_NOT_CLAWBACK_ENABLED = -2,
    CLAWBACK_NO_TRUST = -3,
    CLAWBACK_UNDERFUNDED = -4
};

union ClawbackResult switch (ClawbackResultCode code)
{
case CLAWBACK_SUCCESS:
    void;
default:
    void;
};

/******* ClawbackClaimableBalance Result ********/

enum ClawbackClaimableBalanceResultCode
{
    // codes considered as "success" for the operation
    CLAWBACK_CLAIMABLE_BALANCE_SUCCESS = 0,

    // codes considered as "failure" for the operation
    CLAWBACK_CLAIMABLE_BALANCE_DOES_NOT_EXIST = -1,
    CLAWBACK_CLAIMABLE_BALANCE_NOT_ISSUER = -2,
    CLAWBACK_CLAIMABLE_BALANCE_NOT_CLAWBACK_ENABLED = -3
};

union ClawbackClaimableBalanceResult switch (
    ClawbackClaimableBalanceResultCode code)
{
case CLAWBACK_CLAIMABLE_BALANCE_SUCCESS:
    void;
default:
    void;
};

/******* SetTrustLineFlags Result ********/

enum SetTrustLineFlagsResultCode
{
    // codes considered as "success" for the operation
    SET_TRUST_LINE_FLAGS_SUCCESS = 0,

    // codes considered as "failure" for the operation
    SET_TRUST_LINE_FLAGS_MALFORMED = -1,
    SET_TRUST_LINE_FLAGS_NO_TRUST_LINE = -2,
    SET_TRUST_LINE_FLAGS_CANT_REVOKE = -3,
    SET_TRUST_LINE_FLAGS_INVALID_STATE = -4
};

union SetTrustLineFlagsResult switch (SetTrustLineFlagsResultCode code)
{
case SET_TRUST_LINE_FLAGS_SUCCESS:
    void;
default:
    void;
};

/* High level Operation Result */
enum OperationResultCode
{
    opINNER = 0, // inner object result is valid

    opBAD_AUTH = -1,            // too few valid signatures / wrong network
    opNO_ACCOUNT = -2,          // source account was not found
    opNOT_SUPPORTED = -3,       // operation not supported at this time
    opTOO_MANY_SUBENTRIES = -4, // max number of subentries already reached
    opEXCEEDED_WORK_LIMIT = -5, // operation did too much work
    opTOO_MANY_SPONSORING = -6  // account is sponsoring too many entries
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
    case PATH_PAYMENT_STRICT_RECEIVE:
        PathPaymentStrictReceiveResult pathPaymentStrictReceiveResult;
    case MANAGE_SELL_OFFER:
        ManageSellOfferResult manageSellOfferResult;
    case CREATE_PASSIVE_SELL_OFFER:
        ManageSellOfferResult createPassiveSellOfferResult;
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
    case MANAGE_DATA:
        ManageDataResult manageDataResult;
    case BUMP_SEQUENCE:
        BumpSequenceResult bumpSeqResult;
    case MANAGE_BUY_OFFER:
        ManageBuyOfferResult manageBuyOfferResult;
    case PATH_PAYMENT_STRICT_SEND:
        PathPaymentStrictSendResult pathPaymentStrictSendResult;
    case CREATE_CLAIMABLE_BALANCE:
        CreateClaimableBalanceResult createClaimableBalanceResult;
    case CLAIM_CLAIMABLE_BALANCE:
        ClaimClaimableBalanceResult claimClaimableBalanceResult;
    case BEGIN_SPONSORING_FUTURE_RESERVES:
        BeginSponsoringFutureReservesResult beginSponsoringFutureReservesResult;
    case END_SPONSORING_FUTURE_RESERVES:
        EndSponsoringFutureReservesResult endSponsoringFutureReservesResult;
    case REVOKE_SPONSORSHIP:
        RevokeSponsorshipResult revokeSponsorshipResult;
    case CLAWBACK:
        ClawbackResult clawbackResult;
    case CLAWBACK_CLAIMABLE_BALANCE:
        ClawbackClaimableBalanceResult clawbackClaimableBalanceResult;
    case SET_TRUST_LINE_FLAGS:
        SetTrustLineFlagsResult setTrustLineFlagsResult;
    }
    tr;
default:
    void;
};

enum TransactionResultCode
{
    txFEE_BUMP_INNER_SUCCESS = 1, // fee bump inner transaction succeeded
    txSUCCESS = 0,                // all operations succeeded

    txFAILED = -1, // one of the operations failed (none were applied)

    txTOO_EARLY = -2,         // ledger closeTime before minTime
    txTOO_LATE = -3,          // ledger closeTime after maxTime
    txMISSING_OPERATION = -4, // no operation was specified
    txBAD_SEQ = -5,           // sequence number does not match source account

    txBAD_AUTH = -6,             // too few valid signatures / wrong network
    txINSUFFICIENT_BALANCE = -7, // fee would bring account below reserve
    txNO_ACCOUNT = -8,           // source account not found
    txINSUFFICIENT_FEE = -9,     // fee is too small
    txBAD_AUTH_EXTRA = -10,      // unused signatures attached to transaction
    txINTERNAL_ERROR = -11,      // an unknown error occured

    txNOT_SUPPORTED = -12,         // transaction type not supported
    txFEE_BUMP_INNER_FAILED = -13, // fee bump inner transaction failed
    txBAD_SPONSORSHIP = -14        // sponsorship not confirmed
};

// InnerTransactionResult must be binary compatible with TransactionResult
// because it is be used to represent the result of a Transaction.
struct InnerTransactionResult
{
    // Always 0. Here for binary compatibility.
    int64 feeCharged;

    union switch (TransactionResultCode code)
    {
    // txFEE_BUMP_INNER_SUCCESS is not included
    case txSUCCESS:
    case txFAILED:
        OperationResult results<>;
    case txTOO_EARLY:
    case txTOO_LATE:
    case txMISSING_OPERATION:
    case txBAD_SEQ:
    case txBAD_AUTH:
    case txINSUFFICIENT_BALANCE:
    case txNO_ACCOUNT:
    case txINSUFFICIENT_FEE:
    case txBAD_AUTH_EXTRA:
    case txINTERNAL_ERROR:
    case txNOT_SUPPORTED:
    // txFEE_BUMP_INNER_FAILED is not included
    case txBAD_SPONSORSHIP:
        void;
    }
    result;

    // reserved for future use
    union switch (int v)
    {
    case 0:
        void;
    }
    ext;
};

struct InnerTransactionResultPair
{
    Hash transactionHash;          // hash of the inner transaction
    InnerTransactionResult result; // result for the inner transaction
};

struct TransactionResult
{
    int64 feeCharged; // actual fee charged for the transaction

    union switch (TransactionResultCode code)
    {
    case txFEE_BUMP_INNER_SUCCESS:
    case txFEE_BUMP_INNER_FAILED:
        InnerTransactionResultPair innerResultPair;
    case txSUCCESS:
    case txFAILED:
        OperationResult results<>;
    default:
        void;
    }
    result;

    // reserved for future use
    union switch (int v)
    {
    case 0:
        void;
    }
    ext;
};
}
