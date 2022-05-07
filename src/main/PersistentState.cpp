// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PersistentState.h"

#include "database/Database.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>

namespace stellar
{

using namespace std;

std::string PersistentState::mapping[kLastEntry] = {
    "lastclosedledger", "historyarchivestate", "lastscpdata",
    "databaseschema",   "networkpassphrase",   "ledgerupgrades",
    "rebuildledger",    "lastscpdataxdr"};

std::string PersistentState::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS storestate ("
    "statename   CHARACTER(32) PRIMARY KEY,"
    "state       TEXT"
    "); ";

PersistentState::PersistentState(Application& app) : mApp(app)
{
}

void
PersistentState::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS storestate;";

    soci::statement st = db.getSession().prepare << kSQLCreateStatement;
    st.execute(true);
}

std::string
PersistentState::getStoreStateName(PersistentState::Entry n, uint32 subscript)
{
    if (n < 0 || n >= kLastEntry)
    {
        throw out_of_range("unknown entry");
    }
    auto res = mapping[n];
    if (((n == kLastSCPData || n == kLastSCPDataXDR) && subscript > 0) ||
        n == kRebuildLedger)
    {
        res += std::to_string(subscript);
    }
    return res;
}

std::string
PersistentState::getState(PersistentState::Entry entry)
{
    ZoneScoped;
    return getFromDb(getStoreStateName(entry));
}

void
PersistentState::setState(PersistentState::Entry entry,
                          std::string const& value)
{
    ZoneScoped;
    updateDb(getStoreStateName(entry), value);
}

std::vector<std::pair<std::string, bool>>
PersistentState::getSCPStateAllSlots()
{
    ZoneScoped;
    // Collect all slots persisted
    std::vector<std::pair<std::string, bool>> states;
    for (uint32 i = 0; i <= mApp.getConfig().MAX_SLOTS_TO_REMEMBER; i++)
    {
        bool useXDR = true;
        auto val = getFromDb(getStoreStateName(kLastSCPDataXDR, i));
        if (val.empty())
        {
            useXDR = false;
            val = getFromDb(getStoreStateName(kLastSCPData, i));
        }
        if (!val.empty())
        {
            states.emplace_back(val, useXDR);
        }
    }

    return states;
}

void
PersistentState::setSCPStateForSlot(uint64 slot, std::string const& value,
                                    bool useXDR)
{
    ZoneScoped;
    auto slotIdx = static_cast<uint32>(
        slot % (mApp.getConfig().MAX_SLOTS_TO_REMEMBER + 1));
    auto entry = useXDR ? kLastSCPDataXDR : kLastSCPData;
    updateDb(getStoreStateName(entry, slotIdx), value);
}

bool
PersistentState::shouldRebuildForType(LedgerEntryType let)
{
    ZoneScoped;
    return !getFromDb(getStoreStateName(kRebuildLedger, let)).empty();
}

void
PersistentState::clearRebuildForType(LedgerEntryType let)
{
    ZoneScoped;
    updateDb(getStoreStateName(kRebuildLedger, let), "");
}

void
PersistentState::setRebuildForType(LedgerEntryType let)
{
    ZoneScoped;
    updateDb(getStoreStateName(kRebuildLedger, let), "1");
}

void
PersistentState::updateDb(std::string const& entry, std::string const& value)
{
    ZoneScoped;
    auto prep = mApp.getDatabase().getPreparedStatement(
        "UPDATE storestate SET state = :v WHERE statename = :n;");

    auto& st = prep.statement();
    st.exchange(soci::use(value));
    st.exchange(soci::use(entry));
    st.define_and_bind();
    {
        ZoneNamedN(updateStoreStateZone, "update storestate", true);
        st.execute(true);
    }

    if (st.get_affected_rows() != 1 && getFromDb(entry).empty())
    {
        ZoneNamedN(insertStoreStateZone, "insert storestate", true);
        auto prep2 = mApp.getDatabase().getPreparedStatement(
            "INSERT INTO storestate (statename, state) VALUES (:n, :v);");
        auto& st2 = prep2.statement();
        st2.exchange(soci::use(entry));
        st2.exchange(soci::use(value));
        st2.define_and_bind();
        st2.execute(true);
        if (st2.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not insert data in SQL");
        }
    }
}

std::string
PersistentState::getFromDb(std::string const& entry)
{
    ZoneScoped;
    std::string res;

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        "SELECT state FROM storestate WHERE statename = :n;");
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(entry));
    st.define_and_bind();
    {
        ZoneNamedN(selectStoreStateZone, "select storestate", true);
        st.execute(true);
    }

    if (!st.got_data())
    {
        res.clear();
    }

    return res;
}
}
