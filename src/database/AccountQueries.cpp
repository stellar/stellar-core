// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AccountQueries.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "database/SignerQueries.h"
#include "ledger/AccountFrame.h"
#include "util/basen.h"

namespace stellar
{

namespace
{

const auto DROP_ACCOUNTS_TABLE = "DROP TABLE IF EXISTS accounts;";

const auto CREATE_ACCOUNTS_TABLE =
    "CREATE TABLE accounts"
    "("
    "accountid       VARCHAR(56)  PRIMARY KEY,"
    "balance         BIGINT       NOT NULL CHECK (balance >= 0),"
    "seqnum          BIGINT       NOT NULL,"
    "numsubentries   INT          NOT NULL CHECK (numsubentries >= 0),"
    "inflationdest   VARCHAR(56),"
    "homedomain      VARCHAR(32)  NOT NULL,"
    "thresholds      TEXT         NOT NULL,"
    "flags           INT          NOT NULL,"
    "lastmodified    INT          NOT NULL"
    ");";

const auto CREATE_BALANCE_INDEX = "CREATE INDEX accountbalances "
                                  "ON accounts (balance) WHERE "
                                  "balance >= 1000000000";
}

void
createAccountsTable(Database& db)
{
    db.getSession() << DROP_ACCOUNTS_TABLE;
    db.getSession() << CREATE_ACCOUNTS_TABLE;
    db.getSession() << CREATE_BALANCE_INDEX;
}

std::vector<LedgerEntry>
selectAllAccounts(Database& db)
{
    auto result = std::vector<LedgerEntry>{};

    auto entry = LedgerEntry{};
    entry.data.type(ACCOUNT);
    auto& account = entry.data.account();

    std::string actIDStrKey;
    std::string publicKey, inflationDest, creditAuthKey;
    std::string homeDomain, thresholds;
    soci::indicator inflationDestInd;

    auto prep = db.getPreparedStatement(
        "SELECT accountid, balance, seqnum, numsubentries, "
        "inflationdest, homedomain, thresholds, "
        "flags, lastmodified "
        "FROM accounts");
    auto& st = prep.statement();
    st.exchange(soci::into(actIDStrKey));
    st.exchange(soci::into(account.balance));
    st.exchange(soci::into(account.seqNum));
    st.exchange(soci::into(account.numSubEntries));
    st.exchange(soci::into(inflationDest, inflationDestInd));
    st.exchange(soci::into(homeDomain));
    st.exchange(soci::into(thresholds));
    st.exchange(soci::into(account.flags));
    st.exchange(soci::into(entry.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);

    while (st.got_data())
    {
        account.accountID = KeyUtils::fromStrKey<PublicKey>(actIDStrKey);
        account.homeDomain = homeDomain;

        bn::decode_b64(thresholds.begin(), thresholds.end(),
                       account.thresholds.begin());

        if (inflationDestInd == soci::i_ok)
        {
            account.inflationDest.activate() =
                KeyUtils::fromStrKey<PublicKey>(inflationDest);
        }
        else
        {
            account.inflationDest = nullptr;
        }

        account.signers.clear();

        if (account.numSubEntries != 0)
        {
            account.signers = loadSortedSigners(db, actIDStrKey);
        }

        result.push_back(entry);
        st.fetch();
    }

    return result;
}

optional<LedgerEntry const>
selectAccount(AccountID const& accountID, Database& db)
{
    std::string actIDStrKey = KeyUtils::toStrKey(accountID);
    std::string publicKey, inflationDest, creditAuthKey;
    std::string homeDomain, thresholds;
    soci::indicator inflationDestInd;

    auto result = LedgerEntry{};
    result.data.type(ACCOUNT);
    auto& account = result.data.account();
    account.accountID = accountID;

    auto prep =
        db.getPreparedStatement("SELECT balance, seqnum, numsubentries, "
                                "inflationdest, homedomain, thresholds, "
                                "flags, lastmodified "
                                "FROM accounts WHERE accountid=:v1");
    auto& st = prep.statement();
    st.exchange(soci::into(account.balance));
    st.exchange(soci::into(account.seqNum));
    st.exchange(soci::into(account.numSubEntries));
    st.exchange(soci::into(inflationDest, inflationDestInd));
    st.exchange(soci::into(homeDomain));
    st.exchange(soci::into(thresholds));
    st.exchange(soci::into(account.flags));
    st.exchange(soci::into(result.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("account");
        st.execute(true);
    }

    if (!st.got_data())
    {
        return nullopt<LedgerEntry>();
    }

    account.homeDomain = homeDomain;

    bn::decode_b64(thresholds.begin(), thresholds.end(),
                   account.thresholds.begin());

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() =
            KeyUtils::fromStrKey<PublicKey>(inflationDest);
    }

    account.signers.clear();

    if (account.numSubEntries != 0)
    {
        account.signers = loadSortedSigners(db, actIDStrKey);
    }

    assert(AccountFrame{result}.isValid());
    return make_optional<LedgerEntry const>(result);
}

void
insertAccount(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == ACCOUNT);

    auto& account = entry.data.account();
    auto actIDStrKey = KeyUtils::toStrKey(account.accountID);
    auto sql = std::string{
        "INSERT INTO accounts ( accountid, balance, seqnum, "
        "numsubentries, inflationdest, homedomain, thresholds, flags, "
        "lastmodified ) "
        "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8 )"};

    auto prep = db.getPreparedStatement(sql);

    soci::indicator inflation_ind = soci::i_null;
    std::string inflationDestStrKey;

    if (account.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*account.inflationDest);
        inflation_ind = soci::i_ok;
    }

    std::string thresholds(bn::encode_b64(account.thresholds));

    {
        soci::statement& st = prep.statement();
        st.exchange(soci::use(actIDStrKey, "id"));
        st.exchange(soci::use(account.balance, "v1"));
        st.exchange(soci::use(account.seqNum, "v2"));
        st.exchange(soci::use(account.numSubEntries, "v3"));
        st.exchange(soci::use(inflationDestStrKey, inflation_ind, "v4"));
        std::string homeDomain(account.homeDomain);
        st.exchange(soci::use(homeDomain, "v5"));
        st.exchange(soci::use(thresholds, "v6"));
        st.exchange(soci::use(account.flags, "v7"));
        st.exchange(soci::use(entry.lastModifiedLedgerSeq, "v8"));
        st.define_and_bind();
        {
            auto timer = db.getInsertTimer("account");
            st.execute(true);
        }

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    insertSigners(actIDStrKey, account.signers, db);
}

void
updateAccount(LedgerEntry const& entry, Database& db)
{
    assert(entry.data.type() == ACCOUNT);

    auto& account = entry.data.account();
    auto actIDStrKey = KeyUtils::toStrKey(account.accountID);
    auto sql =
        std::string{"UPDATE accounts SET balance = :v1, seqnum = :v2, "
                    "numsubentries = :v3, "
                    "inflationdest = :v4, homedomain = :v5, thresholds = :v6, "
                    "flags = :v7, lastmodified = :v8 WHERE accountid = :id"};

    auto prep = db.getPreparedStatement(sql);

    soci::indicator inflation_ind = soci::i_null;
    std::string inflationDestStrKey;

    if (account.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*account.inflationDest);
        inflation_ind = soci::i_ok;
    }

    std::string thresholds(bn::encode_b64(account.thresholds));

    {
        soci::statement& st = prep.statement();
        st.exchange(soci::use(actIDStrKey, "id"));
        st.exchange(soci::use(account.balance, "v1"));
        st.exchange(soci::use(account.seqNum, "v2"));
        st.exchange(soci::use(account.numSubEntries, "v3"));
        st.exchange(soci::use(inflationDestStrKey, inflation_ind, "v4"));
        std::string homeDomain(account.homeDomain);
        st.exchange(soci::use(homeDomain, "v5"));
        st.exchange(soci::use(thresholds, "v6"));
        st.exchange(soci::use(account.flags, "v7"));
        st.exchange(soci::use(entry.lastModifiedLedgerSeq, "v8"));
        st.define_and_bind();
        {
            auto timer = db.getUpdateTimer("account");
            st.execute(true);
        }

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    updateSigners(actIDStrKey, account.signers, db);
}

bool
accountExists(LedgerKey const& key, Database& db)
{
    auto actIDStrKey = KeyUtils::toStrKey(key.account().accountID);
    int exists = 0;
    {
        auto timer = db.getSelectTimer("account-exists");
        auto prep =
            db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM accounts "
                                    "WHERE accountid=:v1)");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.exchange(soci::into(exists));
        st.define_and_bind();
        st.execute(true);
    }
    return exists != 0;
}

void
deleteAccount(LedgerKey const& key, Database& db)
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);
    {
        auto timer = db.getDeleteTimer("account");
        auto prep = db.getPreparedStatement(
            "DELETE from accounts where accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        st.execute(true);
    }
    {
        auto timer = db.getDeleteTimer("signer");
        auto prep =
            db.getPreparedStatement("DELETE from signers where accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        st.execute(true);
    }
}

uint64_t
countAccounts(Database& db)
{
    auto query = std::string{R"(
        SELECT COUNT(*) FROM accounts
    )"};

    auto result = 0;
    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::into(result));
    st.define_and_bind();
    st.execute(true);

    return result;
}

uint64_t
sumOfBalances(Database& db)
{
    auto sum = uint64_t{0};
    auto prep = db.getPreparedStatement("SELECT SUM(balance) FROM accounts;");

    auto& st = prep.statement();
    st.exchange(soci::into(sum));
    st.define_and_bind();
    st.execute(true);

    return sum;
}

NumberOfSubentries
numberOfSubentries(AccountID const& accountID, Database& db)
{
    auto result = NumberOfSubentries{};
    auto actIDStrKey = KeyUtils::toStrKey(accountID);

    auto query = std::string{R"(
        SELECT numsubentries,
              (SELECT COUNT(*) FROM trustlines WHERE accountid = :id)
            + (SELECT COUNT(*) FROM offers WHERE sellerid = :id)
            + (SELECT COUNT(*) FROM accountdata WHERE accountid = :id)
            + (SELECT COUNT(*) FROM signers WHERE accountid = :id)
        FROM accounts
        WHERE accountid = :id
    )"};

    auto prep = db.getPreparedStatement(query);
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    st.exchange(soci::into(result.inAccountsTable));
    st.exchange(soci::into(result.calculated));
    st.define_and_bind();
    st.execute(true);

    return result;
}
}
