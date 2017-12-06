// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AccountFrame.h"
#include "LedgerDelta.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerRange.h"
#include "lib/util/format.h"
#include "util/basen.h"
#include "util/types.h"
#include <algorithm>

using namespace soci;
using namespace std;

namespace stellar
{
using xdr::operator<;

const char* AccountFrame::kSQLCreateStatement1 =
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

const char* AccountFrame::kSQLCreateStatement2 =
    "CREATE TABLE signers"
    "("
    "accountid       VARCHAR(56) NOT NULL,"
    "publickey       VARCHAR(56) NOT NULL,"
    "weight          INT         NOT NULL,"
    "PRIMARY KEY (accountid, publickey)"
    ");";

const char* AccountFrame::kSQLCreateStatement3 =
    "CREATE INDEX signersaccount ON signers (accountid)";

const char* AccountFrame::kSQLCreateStatement4 = "CREATE INDEX accountbalances "
                                                 "ON accounts (balance) WHERE "
                                                 "balance >= 1000000000";

AccountFrame::AccountFrame()
    : EntryFrame(ACCOUNT), mAccountEntry(mEntry.data.account())
{
    mAccountEntry.thresholds[0] = 1; // by default, master key's weight is 1
    mUpdateSigners = true;
}

AccountFrame::AccountFrame(LedgerEntry const& from)
    : EntryFrame(from), mAccountEntry(mEntry.data.account())
{
    // we cannot make any assumption on mUpdateSigners:
    // it's possible we're constructing an account with no signers
    // but that the database's state had a previous version with signers
    mUpdateSigners = true;
}

AccountFrame::AccountFrame(AccountFrame const& from) : AccountFrame(from.mEntry)
{
}

AccountFrame::AccountFrame(AccountID const& id) : AccountFrame()
{
    mAccountEntry.accountID = id;
}

AccountFrame::pointer
AccountFrame::makeAuthOnlyAccount(AccountID const& id)
{
    AccountFrame::pointer ret = make_shared<AccountFrame>(id);
    // puts a negative balance to trip any attempt to save this
    ret->mAccountEntry.balance = INT64_MIN;

    return ret;
}

bool
AccountFrame::signerCompare(Signer const& s1, Signer const& s2)
{
    return s1.key < s2.key;
}

void
AccountFrame::normalize()
{
    std::sort(mAccountEntry.signers.begin(), mAccountEntry.signers.end(),
              &AccountFrame::signerCompare);
}

bool
AccountFrame::isAuthRequired() const
{
    return (mAccountEntry.flags & AUTH_REQUIRED_FLAG) != 0;
}

bool
AccountFrame::isImmutableAuth() const
{
    return (mAccountEntry.flags & AUTH_IMMUTABLE_FLAG) != 0;
}

int64_t
AccountFrame::getBalance() const
{
    return (mAccountEntry.balance);
}

bool
AccountFrame::addBalance(int64_t delta)
{
    return stellar::addBalance(mAccountEntry.balance, delta);
}

int64_t
AccountFrame::getMinimumBalance(LedgerManager const& lm) const
{
    return lm.getMinBalance(mAccountEntry.numSubEntries);
}

int64_t
AccountFrame::getBalanceAboveReserve(LedgerManager const& lm) const
{
    int64_t avail =
        getBalance() - lm.getMinBalance(mAccountEntry.numSubEntries);
    if (avail < 0)
    {
        // nothing can leave this account if below the reserve
        // (this can happen if the reserve is raised)
        avail = 0;
    }
    return avail;
}

// returns true if successfully updated,
// false if balance is not sufficient
bool
AccountFrame::addNumEntries(int count, LedgerManager const& lm)
{
    int newEntriesCount = mAccountEntry.numSubEntries + count;
    if (newEntriesCount < 0)
    {
        throw std::runtime_error("invalid account state");
    }
    // only check minBalance when attempting to add subEntries
    if (count > 0 && getBalance() < lm.getMinBalance(newEntriesCount))
    {
        // balance too low
        return false;
    }
    mAccountEntry.numSubEntries = newEntriesCount;
    return true;
}

AccountID const&
AccountFrame::getID() const
{
    return (mAccountEntry.accountID);
}

uint32_t
AccountFrame::getMasterWeight() const
{
    return mAccountEntry.thresholds[THRESHOLD_MASTER_WEIGHT];
}

uint32_t
AccountFrame::getHighThreshold() const
{
    return mAccountEntry.thresholds[THRESHOLD_HIGH];
}

uint32_t
AccountFrame::getMediumThreshold() const
{
    return mAccountEntry.thresholds[THRESHOLD_MED];
}

uint32_t
AccountFrame::getLowThreshold() const
{
    return mAccountEntry.thresholds[THRESHOLD_LOW];
}

AccountFrame::pointer
AccountFrame::loadAccount(LedgerDelta& delta, AccountID const& accountID,
                          Database& db)
{
    auto a = loadAccount(accountID, db);
    if (a)
    {
        delta.recordEntry(*a);
    }
    return a;
}

AccountFrame::pointer
AccountFrame::loadAccount(AccountID const& accountID, Database& db)
{
    LedgerKey key;
    key.type(ACCOUNT);
    key.account().accountID = accountID;
    if (cachedEntryExists(key, db))
    {
        auto p = getCachedEntry(key, db);
        return p ? std::make_shared<AccountFrame>(*p) : nullptr;
    }

    std::string actIDStrKey = KeyUtils::toStrKey(accountID);

    std::string publicKey, inflationDest, creditAuthKey;
    std::string homeDomain, thresholds;
    soci::indicator inflationDestInd;

    AccountFrame::pointer res = make_shared<AccountFrame>(accountID);
    AccountEntry& account = res->getAccount();

    auto prep =
        db.getPreparedStatement("SELECT balance, seqnum, numsubentries, "
                                "inflationdest, homedomain, thresholds, "
                                "flags, lastmodified "
                                "FROM accounts WHERE accountid=:v1");
    auto& st = prep.statement();
    st.exchange(into(account.balance));
    st.exchange(into(account.seqNum));
    st.exchange(into(account.numSubEntries));
    st.exchange(into(inflationDest, inflationDestInd));
    st.exchange(into(homeDomain));
    st.exchange(into(thresholds));
    st.exchange(into(account.flags));
    st.exchange(into(res->getLastModified()));
    st.exchange(use(actIDStrKey));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("account");
        st.execute(true);
    }

    if (!st.got_data())
    {
        putCachedEntry(key, nullptr, db);
        return nullptr;
    }

    account.homeDomain = homeDomain;

    bn::decode_b64(thresholds.begin(), thresholds.end(),
                   res->mAccountEntry.thresholds.begin());

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() =
            KeyUtils::fromStrKey<PublicKey>(inflationDest);
    }

    account.signers.clear();

    if (account.numSubEntries != 0)
    {
        auto signers = loadSigners(db, actIDStrKey);
        account.signers.insert(account.signers.begin(), signers.begin(),
                               signers.end());
    }

    res->normalize();
    res->mUpdateSigners = false;
    res->mKeyCalculated = false;
    res->putCachedEntry(db);
    return res;
}

std::vector<Signer>
AccountFrame::loadSigners(Database& db, std::string const& actIDStrKey)
{
    std::vector<Signer> res;
    string pubKey;
    Signer signer;

    auto prep2 = db.getPreparedStatement("SELECT publickey, weight FROM "
                                         "signers WHERE accountid =:id");
    auto& st2 = prep2.statement();
    st2.exchange(use(actIDStrKey));
    st2.exchange(into(pubKey));
    st2.exchange(into(signer.weight));
    st2.define_and_bind();
    {
        auto timer = db.getSelectTimer("signer");
        st2.execute(true);
    }
    while (st2.got_data())
    {
        signer.key = KeyUtils::fromStrKey<SignerKey>(pubKey);
        res.push_back(signer);
        st2.fetch();
    }

    std::sort(res.begin(), res.end(), &AccountFrame::signerCompare);

    return res;
}

bool
AccountFrame::exists(Database& db, LedgerKey const& key)
{
    if (cachedEntryExists(key, db) && getCachedEntry(key, db) != nullptr)
    {
        return true;
    }

    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);
    int exists = 0;
    {
        auto timer = db.getSelectTimer("account-exists");
        auto prep =
            db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM accounts "
                                    "WHERE accountid=:v1)");
        auto& st = prep.statement();
        st.exchange(use(actIDStrKey));
        st.exchange(into(exists));
        st.define_and_bind();
        st.execute(true);
    }
    return exists != 0;
}

uint64_t
AccountFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM accounts;", into(count);
    return count;
}

uint64_t
AccountFrame::countObjects(soci::session& sess, LedgerRange const& ledgers)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM accounts"
            " WHERE lastmodified >= :v1 AND lastmodified <= :v2;",
        into(count), use(ledgers.first()), use(ledgers.last());
    return count;
}

void
AccountFrame::deleteAccountsModifiedOnOrAfterLedger(Database& db,
                                                    uint32_t oldestLedger)
{
    db.getEntryCache().erase_if(
        [oldestLedger](std::shared_ptr<LedgerEntry const> le) -> bool {
            return le && le->data.type() == ACCOUNT &&
                   le->lastModifiedLedgerSeq >= oldestLedger;
        });

    {
        auto prep = db.getPreparedStatement(
            "DELETE FROM signers WHERE accountid IN"
            " (SELECT accountid FROM accounts WHERE lastmodified >= :v1)");
        auto& st = prep.statement();
        st.exchange(soci::use(oldestLedger));
        st.define_and_bind();
        st.execute(true);
    }
    {
        auto prep = db.getPreparedStatement(
            "DELETE FROM accounts WHERE lastmodified >= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(oldestLedger));
        st.define_and_bind();
        st.execute(true);
    }
}

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db,
                          LedgerKey const& key)
{
    flushCachedEntry(key, db);

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
    delta.deleteEntry(key);
}

void
AccountFrame::storeUpdate(LedgerDelta& delta, Database& db, bool insert)
{
    touch(delta);

    flushCachedEntry(db);

    std::string actIDStrKey = KeyUtils::toStrKey(mAccountEntry.accountID);
    std::string sql;

    if (insert)
    {
        sql = std::string(
            "INSERT INTO accounts ( accountid, balance, seqnum, "
            "numsubentries, inflationdest, homedomain, thresholds, flags, "
            "lastmodified ) "
            "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8 )");
    }
    else
    {
        sql = std::string(
            "UPDATE accounts SET balance = :v1, seqnum = :v2, "
            "numsubentries = :v3, "
            "inflationdest = :v4, homedomain = :v5, thresholds = :v6, "
            "flags = :v7, lastmodified = :v8 WHERE accountid = :id");
    }

    auto prep = db.getPreparedStatement(sql);

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStrKey;

    if (mAccountEntry.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*mAccountEntry.inflationDest);
        inflation_ind = soci::i_ok;
    }

    string thresholds(bn::encode_b64(mAccountEntry.thresholds));

    {
        soci::statement& st = prep.statement();
        st.exchange(use(actIDStrKey, "id"));
        st.exchange(use(mAccountEntry.balance, "v1"));
        st.exchange(use(mAccountEntry.seqNum, "v2"));
        st.exchange(use(mAccountEntry.numSubEntries, "v3"));
        st.exchange(use(inflationDestStrKey, inflation_ind, "v4"));
        string homeDomain(mAccountEntry.homeDomain);
        st.exchange(use(homeDomain, "v5"));
        st.exchange(use(thresholds, "v6"));
        st.exchange(use(mAccountEntry.flags, "v7"));
        st.exchange(use(getLastModified(), "v8"));
        st.define_and_bind();
        {
            auto timer = insert ? db.getInsertTimer("account")
                                : db.getUpdateTimer("account");
            st.execute(true);
        }

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
        if (insert)
        {
            delta.addEntry(*this);
        }
        else
        {
            delta.modEntry(*this);
        }
    }

    if (mUpdateSigners)
    {
        applySigners(db, insert);
    }
}

void
AccountFrame::applySigners(Database& db, bool insert)
{
    std::string actIDStrKey = KeyUtils::toStrKey(mAccountEntry.accountID);

    // generates a diff with the signers stored in the database

    // first, load the signers stored in the database for this account
    std::vector<Signer> signers;
    if (!insert)
    {
        signers = loadSigners(db, actIDStrKey);
    }

    auto it_new = mAccountEntry.signers.begin();
    auto it_old = signers.begin();
    bool changed = false;
    // iterate over both sets from smallest to biggest key
    while (it_new != mAccountEntry.signers.end() || it_old != signers.end())
    {
        bool updated = false, added = false;

        if (it_old == signers.end())
        {
            added = true;
        }
        else if (it_new != mAccountEntry.signers.end())
        {
            updated = (it_new->key == it_old->key);
            if (!updated)
            {
                added = (it_new->key < it_old->key);
            }
        }
        else
        {
            // deleted
        }

        if (updated)
        {
            if (it_new->weight != it_old->weight)
            {
                std::string signerStrKey = KeyUtils::toStrKey(it_new->key);
                auto timer = db.getUpdateTimer("signer");
                auto prep2 = db.getPreparedStatement(
                    "UPDATE signers set weight=:v1 WHERE "
                    "accountid=:v2 AND publickey=:v3");
                auto& st = prep2.statement();
                st.exchange(use(it_new->weight));
                st.exchange(use(actIDStrKey));
                st.exchange(use(signerStrKey));
                st.define_and_bind();
                st.execute(true);
                if (st.get_affected_rows() != 1)
                {
                    throw std::runtime_error("Could not update data in SQL");
                }
                changed = true;
            }
            it_new++;
            it_old++;
        }
        else if (added)
        {
            // signer was added
            std::string signerStrKey = KeyUtils::toStrKey(it_new->key);

            auto prep2 = db.getPreparedStatement("INSERT INTO signers "
                                                 "(accountid,publickey,weight) "
                                                 "VALUES (:v1,:v2,:v3)");
            auto& st = prep2.statement();
            st.exchange(use(actIDStrKey));
            st.exchange(use(signerStrKey));
            st.exchange(use(it_new->weight));
            st.define_and_bind();
            st.execute(true);

            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
            changed = true;
            it_new++;
        }
        else
        {
            // signer was deleted
            std::string signerStrKey = KeyUtils::toStrKey(it_old->key);

            auto prep2 = db.getPreparedStatement("DELETE from signers WHERE "
                                                 "accountid=:v2 AND "
                                                 "publickey=:v3");
            auto& st = prep2.statement();
            st.exchange(use(actIDStrKey));
            st.exchange(use(signerStrKey));
            st.define_and_bind();
            {
                auto timer = db.getDeleteTimer("signer");
                st.execute(true);
            }

            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }

            changed = true;
            it_old++;
        }
    }

    if (changed)
    {
        // Flush again to ensure changed signers are reloaded.
        flushCachedEntry(db);
    }
}

void
AccountFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdate(delta, db, false);
}

void
AccountFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdate(delta, db, true);
}

void
AccountFrame::processForInflation(
    std::function<bool(AccountFrame::InflationVotes const&)> inflationProcessor,
    int maxWinners, Database& db)
{
    soci::session& session = db.getSession();

    InflationVotes v;
    std::string inflationDest;

    soci::statement st =
        (session.prepare
             << "SELECT"
                " sum(balance) AS votes, inflationdest FROM accounts WHERE"
                " inflationdest IS NOT NULL"
                " AND balance >= 1000000000 GROUP BY inflationdest"
                " ORDER BY votes DESC, inflationdest DESC LIMIT :lim",
         into(v.mVotes), into(inflationDest), use(maxWinners));

    st.execute(true);

    while (st.got_data())
    {
        v.mInflationDest = KeyUtils::fromStrKey<PublicKey>(inflationDest);
        if (!inflationProcessor(v))
        {
            break;
        }
        st.fetch();
    }
}

std::unordered_map<AccountID, AccountFrame::pointer>
AccountFrame::checkDB(Database& db)
{
    std::unordered_map<AccountID, AccountFrame::pointer> state;
    {
        std::string id;
        soci::statement st =
            (db.getSession().prepare << "select accountid from accounts",
             soci::into(id));
        st.execute(true);
        while (st.got_data())
        {
            state.insert(
                std::make_pair(KeyUtils::fromStrKey<PublicKey>(id), nullptr));
            st.fetch();
        }
    }
    // load all accounts
    for (auto& s : state)
    {
        s.second = AccountFrame::loadAccount(s.first, db);
    }

    {
        std::string id;
        size_t n;
        // sanity check signers state
        soci::statement st =
            (db.getSession().prepare << "select count(*), accountid from "
                                        "signers group by accountid",
             soci::into(n), soci::into(id));
        st.execute(true);
        while (st.got_data())
        {
            AccountID aid(KeyUtils::fromStrKey<PublicKey>(id));
            auto it = state.find(aid);
            if (it == state.end())
            {
                throw std::runtime_error(fmt::format(
                    "Found extra signers in database for account {}", id));
            }
            else if (n != it->second->mAccountEntry.signers.size())
            {
                throw std::runtime_error(
                    fmt::format("Mismatch signers for account {}", id));
            }
            st.fetch();
        }
    }
    return state;
}

void
AccountFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS accounts;";
    db.getSession() << "DROP TABLE IF EXISTS signers;";

    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
    db.getSession() << kSQLCreateStatement3;
    db.getSession() << kSQLCreateStatement4;
}
}
