#ifndef __CANONICALLEDGERFORM__
#define __CANONICALLEDGERFORM__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include <memory>
#include "generated/StellarXDR.h"
#include "ledger/LedgerEntry.h"
#include "clf/CLFGateway.h"

/*
This is the form of the ledger that is hashed to create the ledger id.
*/

using namespace std;

namespace stellar
{
class CanonicalLedgerForm : public CLFGateway
{
  public:
    typedef shared_ptr<CanonicalLedgerForm> pointer;

    // load up our last known version - hash passed in is used for verification
    // purpose
    virtual bool load(stellarxdr::uint256 ledgerHash) = 0;

    // ledger state manipulation
    virtual void addEntry(stellarxdr::uint256& newHash,
                          LedgerEntry::pointer newEntry) = 0;
    virtual void updateEntry(stellarxdr::uint256& oldHash,
                             stellarxdr::uint256& newHash,
                             LedgerEntry::pointer updatedEntry) = 0;
    // virtual void deleteEntry(const stellarxdr::uint256& hash)=0;

    virtual void closeLedger() = 0; // need to call after all the tx have been
                                    // applied to save that last versions of the
                                    // ledger entries into the buckets

    // returns current ledger hash (recomputes if necessary)
    virtual stellarxdr::uint256 getHash() = 0;

    // computes delta with a particular ledger in the past
    // virtual void getDeltaSince() = 0;
};
}

#endif
