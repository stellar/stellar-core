#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"
#include "overlay/StellarXDR.h"
#include "util/NonCopyable.h"

#include <soci.h>
#include <libpq-fe.h>
#include <string>
#include <sstream>

/*
Frame
Parent of AccountFrame, TrustFrame, OfferFrame

These just hold the xdr LedgerEntry objects and have some associated functions
*/

namespace stellar
{
class Database;
class LedgerDelta;

class EntryFrame : public NonMovableOrCopyable
{
  protected:
    mutable bool mKeyCalculated;
    mutable LedgerKey mKey;
    void
    clearCached()
    {
        mKeyCalculated = false;
    }

  public:
    typedef std::shared_ptr<EntryFrame> pointer;

    LedgerEntry mEntry;

    EntryFrame(LedgerEntryType type);
    EntryFrame(LedgerEntry const& from);
    virtual ~EntryFrame() = default;

    static pointer FromXDR(LedgerEntry const& from);
    static pointer storeLoad(LedgerKey const& key, Database& db);

    // Static helpers for working with the DB LedgerEntry cache.
    static void flushCachedEntry(LedgerKey const& key, Database& db);
    static bool cachedEntryExists(LedgerKey const& key, Database& db);
    static std::shared_ptr<LedgerEntry const>
    getCachedEntry(LedgerKey const& key, Database& db);
    static void putCachedEntry(LedgerKey const& key,
                               std::shared_ptr<LedgerEntry const> p,
                               Database& db);

    // helpers to get/set the last modified field
    uint32 getLastModified() const;
    uint32& getLastModified();
    void touch(uint32 ledgerSeq);

    // touch the entry if the delta is tracking a ledger header with
    // a sequence that is not 0 (0 is used when importing buckets)
    void touch(LedgerDelta const& delta);

    // Member helpers that call cache flush/put for self.
    void flushCachedEntry(Database& db) const;
    void putCachedEntry(Database& db) const;

    static std::string checkAgainstDatabase(LedgerEntry const& entry,
                                            Database& db);

    virtual EntryFrame::pointer copy() const = 0;

    class Accumulator
    {
      public:
      virtual ~Accumulator() noexcept(false) = 0;
    };

    class AccumulatorGroup
    {
      public:
        AccumulatorGroup(Database& db);
        Accumulator*
        accountsAccum()
        {
            return accounts.get();
        }
        Accumulator*
        accountdataAccum()
        {
            return accountdata.get();
        }
        Accumulator*
        offersAccum()
        {
            return offers.get();
        }
        Accumulator*
        trustlinesAccum()
        {
            return trustlines.get();
        }

      private:
        std::unique_ptr<Accumulator> accounts, accountdata, offers, trustlines;
    };

    LedgerKey const& getKey() const;
    virtual void storeDelete(LedgerDelta& delta, Database& db,
                             AccumulatorGroup* accums = 0) const = 0;
    // change/add may update the entry (last modified)
    virtual void storeAddOrChange(LedgerDelta& delta, Database& db,
                                  AccumulatorGroup* accums = 0) = 0;

    void storeChange(LedgerDelta& delta, Database& db,
                     AccumulatorGroup* accums = 0);
    void storeAdd(LedgerDelta& delta, Database& db,
                  AccumulatorGroup* accums = 0);
    static bool exists(Database& db, LedgerKey const& key);
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key, AccumulatorGroup* accums = 0);
};

// static helper for getting a LedgerKey from a LedgerEntry.
LedgerKey LedgerEntryKey(LedgerEntry const& e);

template <typename T>
std::string
marshalpgvecitem(const T& item) {
  std::stringstream ss;
  ss << item;
  return ss.str();
}

template <typename T>
std::string marshalpgvec(const std::vector<T>& v, const std::vector<soci::indicator>* ind = 0) {
  std::string result = "{";
  for (int i = 0; i < v.size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    if (ind && (*ind)[i] == soci::i_null) {
      result += "NULL";
    } else {
      result += marshalpgvecitem(v[i]);
    }
  }
  result += "}";
  return result;
}

}
