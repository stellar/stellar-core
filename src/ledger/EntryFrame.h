#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"
#include "clf/LedgerCmp.h"
#include "lib/json/json-forwards.h"

/*
Frame
Parent of AccountFrame, TrustFrame, OfferFrame

These just hold the xdr LedgerEntry objects and have some associated functions
*/

namespace stellar
{
class Database;
class LedgerDelta;

class EntryFrame
{
  protected:
    mutable bool mKeyCalculated;
    mutable LedgerKey mKey;
    void clearCached() { mKeyCalculated = false; }

  public:
    typedef std::shared_ptr<EntryFrame> pointer;

    LedgerEntry mEntry;

    EntryFrame() = delete;
    EntryFrame(LedgerEntryType type);
    EntryFrame(const LedgerEntry& from);

    static pointer FromXDR(LedgerEntry const& from);

    virtual EntryFrame::pointer copy() const = 0;

    LedgerKey const& getKey() const;
    virtual void storeDelete(LedgerDelta& delta, Database& db) const = 0;
    virtual void storeChange(LedgerDelta& delta, Database& db) const = 0;
    virtual void storeAdd(LedgerDelta& delta, Database& db) const = 0;

    void storeAddOrChange(LedgerDelta& delta, Database& db) const;
    static bool exists(Database& db, LedgerKey const& key);
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
};
}
