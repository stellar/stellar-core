// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PersistentState.h"

#include "crypto/Hex.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderUtils.h"
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
    "rebuildledger",    "lastscpdataxdr",      "txset",
    "dbbackend"};

std::string PersistentState::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS storestate ("
    "statename   CHARACTER(70) PRIMARY KEY,"
    "state       TEXT"
    "); ";

PersistentState::PersistentState(Application& app) : mApp(app)
{
}

void
PersistentState::deleteTxSets(std::unordered_set<Hash> hashesToDelete)
{
    soci::transaction tx(mApp.getDatabase().getSession());
    for (auto const& hash : hashesToDelete)
    {
        auto name = getStoreStateNameForTxSet(hash);
        auto prep = mApp.getDatabase().getPreparedStatement(
            "DELETE FROM storestate WHERE statename = :n;");

        auto& st = prep.statement();
        st.exchange(soci::use(name));
        st.define_and_bind();
        st.execute(true);
    }
    tx.commit();
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
PersistentState::getStoreStateNameForTxSet(Hash const& txSetHash)
{
    auto res = mapping[kTxSet];
    res += binToHex(txSetHash);
    return res;
}

bool
PersistentState::hasTxSet(Hash const& txSetHash)
{
    return entryExists(getStoreStateNameForTxSet(txSetHash));
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

std::vector<std::string>
PersistentState::getSCPStateAllSlots()
{
    ZoneScoped;
    // Collect all slots persisted
    std::vector<std::string> states;
    for (uint32 i = 0; i <= mApp.getConfig().MAX_SLOTS_TO_REMEMBER; i++)
    {
        auto val = getFromDb(getStoreStateName(kLastSCPDataXDR, i));
        if (!val.empty())
        {
            states.push_back(val);
        }
    }

    return states;
}

void
PersistentState::setSCPStateForSlot(uint64 slot, std::string const& value)
{
    ZoneScoped;
    auto slotIdx = static_cast<uint32>(
        slot % (mApp.getConfig().MAX_SLOTS_TO_REMEMBER + 1));
    updateDb(getStoreStateName(kLastSCPDataXDR, slotIdx), value);
}

void
PersistentState::setSCPStateV1ForSlot(
    uint64 slot, std::string const& value,
    std::unordered_map<Hash, std::string> const& txSets)
{
    soci::transaction tx(mApp.getDatabase().getSession());
    setSCPStateForSlot(slot, value);

    for (auto const& txSet : txSets)
    {
        updateDb(getStoreStateNameForTxSet(txSet.first), txSet.second);
    }
    tx.commit();
}

bool
PersistentState::shouldRebuildForOfferTable()
{
    ZoneScoped;
    return !getFromDb(getStoreStateName(kRebuildLedger, OFFER)).empty();
}

void
PersistentState::clearRebuildForOfferTable()
{
    ZoneScoped;
    updateDb(getStoreStateName(kRebuildLedger, OFFER), "");
}

void
PersistentState::setRebuildForOfferTable()
{
    ZoneScoped;
    updateDb(getStoreStateName(kRebuildLedger, OFFER), "1");
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

std::vector<std::string>
PersistentState::getTxSetsForAllSlots()
{
    ZoneScoped;
    std::vector<std::string> result;
    std::string val;

    std::string pattern = mapping[kTxSet] + "%";
    std::string statementStr =
        "SELECT state FROM storestate WHERE statename LIKE :n;";
    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(statementStr);
    auto& st = prep.statement();
    st.exchange(soci::into(val));
    st.exchange(soci::use(pattern));
    st.define_and_bind();
    {
        ZoneNamedN(selectStoreStateZone, "select storestate", true);
        st.execute(true);
    }

    while (st.got_data())
    {
        result.push_back(val);
        st.fetch();
    }

    return result;
}

std::unordered_set<Hash>
PersistentState::getTxSetHashesForAllSlots()
{
    ZoneScoped;
    std::unordered_set<Hash> result;
    std::string val;

    std::string pattern = mapping[kTxSet] + "%";
    std::string statementStr =
        "SELECT statename FROM storestate WHERE statename LIKE :n;";
    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(statementStr);
    auto& st = prep.statement();
    st.exchange(soci::into(val));
    st.exchange(soci::use(pattern));
    st.define_and_bind();
    {
        ZoneNamedN(selectStoreStateZone, "select storestate", true);
        st.execute(true);
    }

    size_t offset = mapping[kTxSet].size();
    Hash hash;
    size_t len = binToHex(hash).size();

    while (st.got_data())
    {
        result.insert(hexToBin256(val.substr(offset, len)));
        st.fetch();
    }

    return result;
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

bool
PersistentState::entryExists(std::string const& entry)
{
    ZoneScoped;
    int res = 0;

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        "SELECT COUNT(*) FROM storestate WHERE statename = :n;");
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(entry));
    st.define_and_bind();
    st.execute(true);

    return res > 0;
}
}
