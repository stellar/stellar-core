// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "generated/Stellar-transaction.h"

namespace stellar
{

/* The LedgerHeader is the highest level structure representing the
 * state of a ledger, cryptographically linked to previous ledgers.
*/
struct LedgerHeader
{
    Hash previousLedgerHash; // hash of the previous ledger header
    Hash txSetHash;          // the tx set that was SCP confirmed
    Hash txSetResultHash;    // the TransactionResultSet that led to this ledger
    Hash bucketListHash;     // hash of the ledger state

    uint32 ledgerSeq; // sequence number of this ledger
    uint64 closeTime; // network close time

    int64 totalCoins; // total number of stroops in existence

    int64 feePool;       // fees burned since last inflation run
    uint32 inflationSeq; // inflation sequence number

    uint64 idPool; // last used global ID, used for generating objects

    int32 baseFee;     // base fee per operation in stroops
    int32 baseReserve; // account base reserve in stroops
};

/* Entries used to define the bucket list */

union LedgerKey switch (LedgerEntryType type)
{
case ACCOUNT:
    struct
    {
        AccountID accountID;
    } account;

case TRUSTLINE:
    struct
    {
        AccountID accountID;
        Currency currency;
    } trustLine;

case OFFER:
    struct
    {
        AccountID accountID;
        uint64 offerID;
    } offer;
};

enum BucketEntryType
{
    LIVEENTRY = 0,
    DEADENTRY = 1
};

union BucketEntry switch (BucketEntryType type)
{
case LIVEENTRY:
    LedgerEntry liveEntry;

case DEADENTRY:
    LedgerKey deadEntry;
};

// Transaction sets are the unit used by SCP to decide on transitions
// between ledgers
struct TransactionSet
{
    Hash previousLedgerHash;
    TransactionEnvelope txs<5000>;
};

struct TransactionResultPair
{
    Hash transactionHash;
    TransactionResult result; // result for the transaction
};

// TransactionResultSet is used to recover results between ledgers
struct TransactionResultSet
{
    TransactionResultPair results<5000>;
};

// Entries below are used in the historical subsystem
struct TransactionMeta
{
    BucketEntry entries<>;
};

struct TransactionHistoryEntry
{
    uint32 ledgerSeq;
    TransactionSet txSet;
};

struct TransactionHistoryResultEntry
{
    uint32 ledgerSeq;
    TransactionResultSet txResultSet;
};

struct LedgerHeaderHistoryEntry
{
    Hash hash;
    LedgerHeader header;
};
}
