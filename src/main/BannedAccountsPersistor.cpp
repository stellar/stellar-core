// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/BannedAccountsPersistor.h"

#include "database/Database.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{

BannedAccountsPersistor::BannedAccountsPersistor(Application& app) : mApp(app)
{
    releaseAssert(threadIsMain());
}

void
BannedAccountsPersistor::maybeDropAndCreateNew(soci::session& sess)
{
    sess << "DROP TABLE IF EXISTS bannedaccounts;";
    sess << "CREATE TABLE bannedaccounts (accountid VARCHAR(56) PRIMARY KEY);";
}

std::set<AccountID>
BannedAccountsPersistor::getBannedAccounts() const
{
    releaseAssert(threadIsMain());

    std::set<AccountID> result;

    auto& db = mApp.getDatabase();
    std::string accountid;
    auto prep = db.getPreparedStatement("SELECT accountid FROM bannedaccounts;",
                                        db.getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(accountid));
    st.define_and_bind();
    st.execute(true);

    while (st.got_data())
    {
        try
        {
            result.emplace(KeyUtils::fromStrKey<PublicKey>(accountid));
        }
        catch (std::exception const& e)
        {
            CLOG_WARNING(Herder,
                         "Ignoring invalid banned account in database: {}",
                         accountid);
        }
        st.fetch();
    }

    CLOG_INFO(Herder, "Loaded {} banned account(s) from database",
              result.size());
    return result;
}

size_t
BannedAccountsPersistor::getBannedAccountsCount() const
{
    releaseAssert(threadIsMain());

    int count = 0;
    auto& db = mApp.getDatabase();
    auto prep = db.getPreparedStatement("SELECT COUNT(*) FROM bannedaccounts;",
                                        db.getMiscSession());
    auto& st = prep.statement();
    st.exchange(soci::into(count));
    st.define_and_bind();
    st.execute(true);
    return static_cast<size_t>(count);
}

void
BannedAccountsPersistor::addBannedAccounts(
    std::vector<std::string> const& addresses)
{
    releaseAssert(threadIsMain());

    auto& db = mApp.getDatabase();
    soci::transaction tx(db.getRawMiscSession());

    for (auto const& addr : addresses)
    {
        auto key = KeyUtils::fromStrKey<PublicKey>(addr);
        auto prep = db.getPreparedStatement(
            "INSERT INTO bannedaccounts (accountid) VALUES (:id) "
            "ON CONFLICT (accountid) DO NOTHING;",
            db.getMiscSession());
        auto& st = prep.statement();
        st.exchange(soci::use(addr));
        st.define_and_bind();
        st.execute(true);
    }

    tx.commit();
}

void
BannedAccountsPersistor::removeBannedAccounts(
    std::vector<std::string> const& addresses)
{
    releaseAssert(threadIsMain());

    auto& db = mApp.getDatabase();
    soci::transaction tx(db.getRawMiscSession());

    for (auto const& addr : addresses)
    {
        auto key = KeyUtils::fromStrKey<PublicKey>(addr);

        auto prep = db.getPreparedStatement(
            "DELETE FROM bannedaccounts WHERE accountid = :id;",
            db.getMiscSession());
        auto& st = prep.statement();
        st.exchange(soci::use(addr));
        st.define_and_bind();
        st.execute(true);
    }

    tx.commit();
}

void
BannedAccountsPersistor::clearBannedAccounts()
{
    releaseAssert(threadIsMain());

    auto& db = mApp.getDatabase();
    db.getRawMiscSession() << "DELETE FROM bannedaccounts;";
}

std::vector<std::string>
BannedAccountsPersistor::getBannedAccountStrKeys() const
{
    releaseAssert(threadIsMain());

    auto result = getBannedAccounts();
    std::vector<std::string> keys;
    for (auto const& account : result)
    {
        keys.push_back(KeyUtils::toStrKey(account));
    }
    return keys;
}
}
