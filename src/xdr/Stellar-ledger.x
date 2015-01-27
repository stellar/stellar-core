
%#include "generated/Stellar-transaction.h"

namespace stellar {

struct CLFBucketHeader
{
    uint64 ledgerSeq;
    uint32 ledgerCount;
    Hash hash;
};

struct CLFLevel
{
    CLFBucketHeader curr;
    CLFBucketHeader snap;
};

struct LedgerHeader
{
    Hash hash;
    Hash previousLedgerHash;
    Hash txSetHash;            // the tx set that was FBA confirmed
    Hash clfHash;
    CLFLevel clfLevels[5];

    int64 totalCoins;
    int64 feePool;
    uint64 ledgerSeq;
    uint32 inflationSeq;
    int32 baseFee;
    int32 baseReserve;
    uint64 closeTime;
};

enum LedgerType {
    ACCOUNT,
    TRUSTLINE,
    OFFER
};

struct AccountEntry
{
    uint256 accountID;
    int64 balance;
    uint32 sequence;
    uint32 ownerCount;
    uint32 transferRate;    // amountsent/transferrate is how much you charge
    uint256 *inflationDest;
    opaque thresholds[4]; // weight of master/threshold1/threshold2/threshold3
    Signer signers<>; // do we want some max or just increase the min balance
    KeyValue data<>;

    uint32 flags; // require dt, require auth,
};

struct TrustLineEntry
{
    uint256 accountID;
    Currency currency;
    int64 limit;
    int64 balance;
    bool authorized;  // if the issuer has authorized this guy to hold its credit
};

// selling 10A @ 2B/A
struct OfferEntry
{
    uint256 accountID;
    uint32 sequence;
    Currency takerGets;  // A
    Currency takerPays;  // B
    int64 amount;    // amount of A
    int64 price;    // price of A in terms of B
                    // price*10,000,000
                    // price=AmountB/AmountA
                    // price is after fees
    int32 flags;
};

union LedgerEntry switch (LedgerType type)
{
    case ACCOUNT:
        AccountEntry account;

    case TRUSTLINE:
        TrustLineEntry trustLine;

    case OFFER:
        OfferEntry offer;
};


enum CLFType {
    LIVEENTRY,
    TOMBSTONE
};

union LedgerKey switch (LedgerType type)
{
    case ACCOUNT:
        struct {
            uint256 accountID;
        } account;

    case TRUSTLINE:
        struct {
            uint256 accountID;
            Currency currency;
        } trustLine;

    case OFFER:
        struct {
            uint256 accountID;
            uint32 sequence;
        } offer;
};

struct CLFEntry
{
    Hash hash;
    union switch (CLFType type)
    {
        case LIVEENTRY:
            LedgerEntry liveEntry;

        case TOMBSTONE:
            LedgerKey tombstone;
    } entry;
};


struct CLFBucket
{
    CLFBucketHeader header;
    CLFEntry entries<>;
};

struct TransactionSet
{
    uint256 previousLedgerHash;
    TransactionEnvelope txs<>;
};

struct HistoryEntry
{
    LedgerHeader header;
    TransactionSet txSet;
};

struct History
{
    uint64 fromLedger;
    uint64 toLedger;
    HistoryEntry entries<>;
};

}
