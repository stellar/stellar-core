%#include "generated/Stellar-transaction.h"

namespace stellar {

struct LedgerHeader
{
    Hash previousLedgerHash;// hash of the previous ledger header
    Hash txSetHash;         // the tx set that was SCP confirmed
    Hash clfHash;           // hash of the ledger state

    uint32 ledgerSeq;       // sequence number of this ledger
    uint64 closeTime;       // network close time

    int64 totalCoins;       // total number of stroops in existence

    int64 feePool;          // fees burned since last inflation run
    uint32 inflationSeq;    // inflation sequence number

    uint64 idPool;          // last used global ID, used for generating objects

    int32 baseFee;          // base fee per operation in stroops
    int32 baseReserve;      // account base reserve in stroops

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
    uint32 ledgerSeq;
    TransactionEnvelope envelope;
    TransactionResult result;
};

struct LedgerHeaderHistoryEntry
{
    Hash hash;
    LedgerHeader header;
};

}
