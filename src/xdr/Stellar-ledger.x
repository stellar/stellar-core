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

enum CLFType {
    LIVEENTRY,
    DEADENTRY
};

union LedgerKey switch (LedgerEntryType type)
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

        case DEADENTRY:
            LedgerKey deadEntry;
    } entry;
};


struct CLFBucket
{
    CLFBucketHeader header;
    CLFEntry entries<>;
};

struct TransactionSet
{
    Hash previousLedgerHash;
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
