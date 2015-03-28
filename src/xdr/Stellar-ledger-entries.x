// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

%#include "generated/Stellar-types.h"

namespace stellar
{

enum LedgerEntryType
{
    ACCOUNT = 0,
    TRUSTLINE = 1,
    OFFER = 2
};

struct Signer
{
    uint256 pubKey;
    uint32 weight; // really only need 1byte
};

enum AccountFlags
{ // masks for each flag
    AUTH_REQUIRED_FLAG = 0x1
};

/* AccountEntry

    Main entry representing a user in Stellar. All transactions are performed
    using an account.

    Other ledger entries created require an account.

*/

struct AccountEntry
{
    AccountID accountID;      // master public key for this account
    int64 balance;            // in stroops
    SequenceNumber seqNum;    // last sequence number used for this account
    uint32 numSubEntries;     // number of sub-entries this account has
                              // drives the reserve
    AccountID* inflationDest; // Account to vote during inflation
    uint32 flags;             // see AccountFlags

    // fields used for signatures
    // thresholds stores unsigned bytes: [weight of master|low|medium|high]
    Thresholds thresholds;

    Signer signers<20>; // possible signers for this account
};

/* TrustLineEntry
    A trust line represents a specific trust relationship with
    a currency/issuer (limit, authorization)
    as well as the balance.
*/

struct TrustLineEntry
{
    AccountID accountID; // account this trustline belongs to
    Currency currency;   // currency (with issuer)
    int64 balance;       // how much of this currency the user has.
                         // Currency defines the unit for this;

    int64 limit;     // balance cannot be above this
    bool authorized; // issuer has authorized account to hold its credit
};

/* OfferEntry
    An offer is the building block of the offer book, they are automatically
    claimed by payments when the price set by the owner is met.

    For example an Offer is selling 10A where 1A is priced at 1.5B

*/
struct OfferEntry
{
    AccountID accountID;
    uint64 offerID;
    Currency takerGets; // A
    Currency takerPays; // B
    int64 amount;       // amount of A

    /* price for this offer:
        price of A in terms of B
        price=AmountB/AmountA=priceNumerator/priceDenominator
        price is after fees
    */
    Price price;
};

union LedgerEntry switch (LedgerEntryType type)
{
case ACCOUNT:
    AccountEntry account;

case TRUSTLINE:
    TrustLineEntry trustLine;

case OFFER:
    OfferEntry offer;
};
}
