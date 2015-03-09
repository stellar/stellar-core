%#include "generated/Stellar-transaction.h"

namespace stellar {

struct LedgerHeader
{
    Hash hash;
    Hash previousLedgerHash;
    Hash txSetHash;            // the tx set that was SCP confirmed
    Hash clfHash;

    int64 totalCoins;
    int64 feePool;
    uint64 ledgerSeq;
    uint64 idPool;
    uint32 inflationSeq;
    int32 baseFee;
    int32 baseReserve;
    uint64 closeTime;
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
            uint256 accountID; // GRAYDON: ok to drop this since offerID is unique now?
            uint64 offerID;
        } offer;
};

enum CLFType {
    LIVEENTRY,
    DEADENTRY
};

union CLFEntry switch (CLFType type)
{
    case LIVEENTRY:
        LedgerEntry liveEntry;

    case DEADENTRY:
        LedgerKey deadEntry;
};

struct TransactionSet
{
    Hash previousLedgerHash;
    TransactionEnvelope txs<>;
};

struct TransactionMeta
{
    CLFEntry entries<>;
};

struct TransactionHistoryEntry
{
    uint64 ledgerSeq;
    TransactionEnvelope envelope;
    TransactionResult result;
};

}
