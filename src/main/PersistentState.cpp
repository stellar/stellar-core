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

std::string PersistentState::mainMapping[kLastEntryMain] = {
    "lastclosedledgerheader", "historyarchivestate", "databaseschema",
    "networkpassphrase", "rebuildledger"};

std::string PersistentState::miscMapping[kLastEntry] = {
    "miscdatabaseschema",
    "ledgerupgrades",
    "lastscpdataxdr",
    "txset",
};

std::string PersistentState::kSQLCreateStatement =
    "CREATE TABLE IF NOT EXISTS storestate ("
    "statename   CHARACTER(70) PRIMARY KEY,"
    "state       TEXT"
    "); ";

// Persist consensus slot data
std::string PersistentState::kSQLCreateSCPStatement =
    "CREATE TABLE IF NOT EXISTS slotstate ("
    "statename   CHARACTER(70) PRIMARY KEY,"
    "state       TEXT"
    "); ";

std::string PersistentState::kLCLTableName = "storestate";
std::string PersistentState::kSlotTableName = "slotstate";

PersistentState::PersistentState(Application& app) : mApp(app)
{
    releaseAssert(threadIsMain());
}

void
PersistentState::deleteTxSets(std::unordered_set<Hash> hashesToDelete)
{
    releaseAssert(threadIsMain());
    soci::transaction tx(mApp.getDatabase().getRawMiscSession());
    for (auto const& hash : hashesToDelete)
    {
        auto name = getStoreStateNameForTxSet(hash);
        auto prep = mApp.getDatabase().getPreparedStatement(
            fmt::format("DELETE FROM {} WHERE statename = :n;", kSlotTableName),
            mApp.getDatabase().getMiscSession());

        auto& st = prep.statement();
        st.exchange(soci::use(name));
        st.define_and_bind();
        st.execute(true);
    }
    tx.commit();
}

void
PersistentState::maybeDropAndCreateNew(Database& db)
{
    releaseAssert(threadIsMain());
    db.getRawSession() << "DROP TABLE IF EXISTS storestate;";
    soci::statement st = db.getRawSession().prepare << kSQLCreateStatement;
    st.execute(true);

    db.getRawSession() << "DROP TABLE IF EXISTS slotstate;";
    soci::statement st2 = db.getRawSession().prepare << kSQLCreateSCPStatement;
    st2.execute(true);
}

void
PersistentState::createMisc(Database& db)
{
    db.getRawMiscSession() << fmt::format("DROP TABLE IF EXISTS {};",
                                          kSlotTableName);
    soci::statement st = db.getRawMiscSession().prepare
                         << kSQLCreateSCPStatement;
    st.execute(true);
}

std::string
PersistentState::getStoreStateName(PersistentState::Entry n, uint32 subscript)
{
    if (n < 0 || n >= kLastEntry)
    {
        throw out_of_range("unknown entry");
    }

    std::string res;
    if (n < kLastEntryMain)
    {
        res = mainMapping[n];
    }
    else
    {
        res = miscMapping[n - kLastEntryMain - 1];
    }

    if ((n == kLastSCPDataXDR && subscript > 0) || n == kRebuildLedger)
    {
        res += std::to_string(subscript);
    }
    return res;
}

std::string
PersistentState::getStoreStateNameForTxSet(Hash const& txSetHash)
{
    auto res = miscMapping[kTxSet - kLastEntryMain - 1];
    res += binToHex(txSetHash);
    return res;
}

bool
PersistentState::hasTxSet(Hash const& txSetHash)
{
    releaseAssert(threadIsMain());

    int res = 0;
    auto entry = getStoreStateNameForTxSet(txSetHash);

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        "SELECT COUNT(*) FROM slotstate WHERE statename = :n;",
        db.getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(res));
    st.exchange(soci::use(entry));
    st.define_and_bind();
    st.execute(true);

    return res > 0;
}

std::string
PersistentState::getTableForEntry(PersistentState::Entry entry)
{
    releaseAssert(entry != kLastEntry);
    releaseAssert(entry != kLastEntryMain);
    return entry < kLastEntryMain ? kLCLTableName : kSlotTableName;
}

std::string
PersistentState::getState(PersistentState::Entry entry, SessionWrapper& session)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  mApp.threadIsType(Application::ThreadType::APPLY));
    return getFromDb(getStoreStateName(entry), session,
                     getTableForEntry(entry));
}

void
PersistentState::setMainState(PersistentState::Entry entry,
                              std::string const& value, SessionWrapper& session)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  mApp.threadIsType(Application::ThreadType::APPLY));
    if (entry >= kLastEntryMain)
    {
        throw std::invalid_argument(fmt::format("Invalid entry {} is not main",
                                                static_cast<int>(entry)));
    }
    updateDb(getStoreStateName(entry), value, session, getTableForEntry(entry));
}

void
PersistentState::setMiscState(PersistentState::Entry entry,
                              std::string const& value)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  mApp.threadIsType(Application::ThreadType::APPLY));
    if (entry <= kLastEntryMain)
    {
        throw std::invalid_argument(fmt::format("Invalid entry {} is not misc",
                                                static_cast<int>(entry)));
    }
    updateDb(getStoreStateName(entry), value,
             mApp.getDatabase().getMiscSession(), getTableForEntry(entry));
}

std::unordered_map<uint32_t, std::string>
PersistentState::getSCPStateAllSlots()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    // Collect all slots persisted
    std::unordered_map<uint32_t, std::string> states;
    for (uint32 i = 0; i <= mApp.getConfig().MAX_SLOTS_TO_REMEMBER; i++)
    {
        auto val =
            getFromDb(getStoreStateName(kLastSCPDataXDR, i),
                      mApp.getDatabase().getMiscSession(), kSlotTableName);
        if (!val.empty())
        {
            states.emplace(i, val);
        }
    }

    return states;
}

void
PersistentState::setSCPStateForSlot(uint64 slot, std::string const& value)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    auto slotIdx = static_cast<uint32>(
        slot % (mApp.getConfig().MAX_SLOTS_TO_REMEMBER + 1));
    updateDb(getStoreStateName(kLastSCPDataXDR, slotIdx), value,
             mApp.getDatabase().getMiscSession(), kSlotTableName);
}

void
PersistentState::setSCPStateV1ForSlot(
    uint64 slot, std::string const& value,
    std::unordered_map<Hash, std::string> const& txSets)
{
    releaseAssert(threadIsMain());
    soci::transaction tx(mApp.getDatabase().getRawMiscSession());
    setSCPStateForSlot(slot, value);

    for (auto const& txSet : txSets)
    {
        updateDb(getStoreStateNameForTxSet(txSet.first), txSet.second,
                 mApp.getDatabase().getMiscSession(), kSlotTableName);
    }
    tx.commit();
}

bool
PersistentState::shouldRebuildForOfferTable()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    return !getFromDb(getStoreStateName(kRebuildLedger, OFFER),
                      mApp.getDatabase().getSession(), kLCLTableName)
                .empty();
}

void
PersistentState::clearRebuildForOfferTable()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    updateDb(getStoreStateName(kRebuildLedger, OFFER), "",
             mApp.getDatabase().getSession(), kLCLTableName);
}

void
PersistentState::setRebuildForOfferTable()
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    updateDb(getStoreStateName(kRebuildLedger, OFFER), "1",
             mApp.getDatabase().getSession(), kLCLTableName);
}

void
PersistentState::updateDb(std::string const& entry, std::string const& value,
                          SessionWrapper& sess, std::string const& tableName)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  mApp.threadIsType(Application::ThreadType::APPLY));
    auto prep = mApp.getDatabase().getPreparedStatement(
        fmt::format("UPDATE {} SET state = :v WHERE statename = :n;",
                    tableName),
        sess);

    auto& st = prep.statement();
    st.exchange(soci::use(value));
    st.exchange(soci::use(entry));
    st.define_and_bind();
    {
        ZoneNamedN(updateStoreStateZone, "update storestate", true);
        st.execute(true);
    }

    if (st.get_affected_rows() != 1 &&
        getFromDb(entry, sess, tableName).empty())
    {
        ZoneNamedN(insertStoreStateZone, "insert storestate", true);
        auto prep2 = mApp.getDatabase().getPreparedStatement(
            fmt::format("INSERT INTO {} (statename, state) VALUES (:n, :v);",
                        tableName),
            sess);
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

std::unordered_map<Hash, std::string>
PersistentState::getTxSetsForAllSlots()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    std::unordered_map<Hash, std::string> result;
    std::string key;
    std::string val;

    std::string pattern = miscMapping[kTxSet - kLastEntryMain - 1] + "%";
    std::string statementStr =
        fmt::format("SELECT statename, state FROM {} WHERE statename LIKE :n;",
                    kSlotTableName);
    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(statementStr, db.getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(key));
    st.exchange(soci::into(val));
    st.exchange(soci::use(pattern));
    st.define_and_bind();
    {
        ZoneNamedN(selectStoreStateZone, "select storestate", true);
        st.execute(true);
    }

    Hash hash;
    size_t len = binToHex(hash).size();

    while (st.got_data())
    {
        result.emplace(
            hexToBin256(key.substr(
                miscMapping[kTxSet - kLastEntryMain - 1].size(), len)),
            val);
        st.fetch();
    }

    return result;
}

std::unordered_set<Hash>
PersistentState::getTxSetHashesForAllSlots()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    std::unordered_set<Hash> result;
    std::string val;

    std::string pattern = miscMapping[kTxSet - kLastEntryMain - 1] + "%";
    std::string statementStr =
        "SELECT statename FROM slotstate WHERE statename LIKE :n;";
    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(statementStr, db.getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(val));
    st.exchange(soci::use(pattern));
    st.define_and_bind();
    {
        ZoneNamedN(selectSlotStateZone, "select slotstate", true);
        st.execute(true);
    }

    size_t offset = miscMapping[kTxSet - kLastEntryMain - 1].size();
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
PersistentState::getFromDb(std::string const& entry, SessionWrapper& sess,
                           std::string const& tableName)
{
    ZoneScoped;
    releaseAssert(threadIsMain() ||
                  mApp.threadIsType(Application::ThreadType::APPLY));
    std::string res;

    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement(
        fmt::format("SELECT state FROM {} WHERE statename = :n;", tableName),
        sess);
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
