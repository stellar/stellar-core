#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>
#include <unordered_map>

namespace soci
{
class session;
}

namespace stellar
{
class LedgerRange;
class ManageDataOpFrame;
class StatementContext;

class DataFrame : public EntryFrame
{
    static void loadData(StatementContext& prep,
                         std::function<void(LedgerEntry const&)> dataProcessor);

    DataEntry& mData;

    void storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert);

  public:
    typedef std::shared_ptr<DataFrame> pointer;

    DataFrame();
    DataFrame(LedgerEntry const& from);
    DataFrame(DataFrame const& from);

    DataFrame& operator=(DataFrame const& other);

    EntryFrame::pointer
    copy() const override
    {
        return std::make_shared<DataFrame>(*this);
    }

    std::string const& getName() const;
    stellar::DataValue const& getValue() const;
    AccountID const& getAccountID() const;

    DataEntry const&
    getData() const
    {
        return mData;
    }

    DataEntry&
    getData()
    {
        return mData;
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helpers that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);
    static uint64_t countObjects(soci::session& sess,
                                 LedgerRange const& ledgers);
    static void deleteDataModifiedOnOrAfterLedger(Database& db,
                                                  uint32_t oldestLedger);

    // database utilities
    static pointer loadData(AccountID const& accountID, std::string dataName,
                            Database& db);

    // load all data entries from the database (very slow)
    static std::unordered_map<AccountID, std::vector<DataFrame::pointer>>
    loadAllData(Database& db);

    static void dropAll(Database& db);

  private:
    static const char* kSQLCreateStatement1;
};
}
