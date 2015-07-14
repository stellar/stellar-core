// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AccountFrame.h"
#include "crypto/SecretKey.h"
#include "crypto/Hex.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "ledger/LedgerManager.h"
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
    "homedomain      VARCHAR(32),"
    "thresholds      TEXT,"
    "flags           INT          NOT NULL"
    ");";

const char* AccountFrame::kSQLCreateStatement2 =
    "CREATE TABLE Signers"
    "("
    "accountid       VARCHAR(56) NOT NULL,"
    "publickey       VARCHAR(56) NOT NULL,"
    "weight          INT         NOT NULL,"
    "PRIMARY KEY (accountid, publickey)"
    ");";

const char* AccountFrame::kSQLCreateStatement3 =
    "CREATE INDEX signersaccount ON signers (accountid)";

const char* AccountFrame::kSQLCreateStatement4 =
    "CREATE INDEX accountbalances ON Accounts (balance)";

AccountFrame::AccountFrame()
    : EntryFrame(ACCOUNT), mAccountEntry(mEntry.account())
{
    mAccountEntry.thresholds[0] = 1; // by default, master key's weight is 1
    mUpdateSigners = false;
}

AccountFrame::AccountFrame(LedgerEntry const& from)
    : EntryFrame(from), mAccountEntry(mEntry.account())
{
    mUpdateSigners = !mAccountEntry.signers.empty();
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

void
AccountFrame::normalize()
{
    std::sort(mAccountEntry.signers.begin(), mAccountEntry.signers.end(),
              [](Signer const& s1, Signer const& s2)
              {
                  return s1.pubKey < s2.pubKey;
              });
}

bool
AccountFrame::isAuthRequired() const
{
    return (mAccountEntry.flags & AUTH_REQUIRED_FLAG);
}

int64_t
AccountFrame::getBalance() const
{
    return (mAccountEntry.balance);
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
    if (getBalance() < lm.getMinBalance(newEntriesCount))
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
AccountFrame::loadAccount(AccountID const& accountID, Database& db)
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(accountID);
    std::string publicKey, inflationDest, creditAuthKey;
    std::string homeDomain, thresholds;
    soci::indicator inflationDestInd, homeDomainInd, thresholdsInd;

    soci::session& session = db.getSession();

    AccountFrame::pointer res = make_shared<AccountFrame>(accountID);
    AccountEntry& account = res->getAccount();
    {
        auto timer = db.getSelectTimer("account");
        session << "SELECT balance, seqnum, numsubentries, "
                   "inflationdest, homedomain, thresholds,  flags "
                   "FROM accounts WHERE accountid=:v1",
            into(account.balance), into(account.seqNum),
            into(account.numSubEntries), into(inflationDest, inflationDestInd),
            into(homeDomain, homeDomainInd), into(thresholds, thresholdsInd),
            into(account.flags), use(actIDStrKey);
    }

    if (!session.got_data())
        return nullptr;

    if (homeDomainInd == soci::i_ok)
    {
        account.homeDomain = homeDomain;
    }

    if (thresholdsInd == soci::i_ok)
    {
        std::vector<uint8_t> bin = hexToBin(thresholds);
        for (size_t n = 0; (n < 4) && (n < bin.size()); n++)
        {
            res->mAccountEntry.thresholds[n] = bin[n];
        }
    }

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() =
            PubKeyUtils::fromStrKey(inflationDest);
    }

    account.signers.clear();

    if (account.numSubEntries != 0)
    {
        string pubKey;
        Signer signer;

        auto prep = db.getPreparedStatement("SELECT publickey, weight from "
                                            "signers where accountid =:id");
        auto& st = prep.statement();
        st.exchange(use(actIDStrKey));
        st.exchange(into(pubKey));
        st.exchange(into(signer.weight));
        st.define_and_bind();
        {
            auto timer = db.getSelectTimer("signer");
            st.execute(true);
        }
        while (st.got_data())
        {
            signer.pubKey = PubKeyUtils::fromStrKey(pubKey);

            account.signers.push_back(signer);

            st.fetch();
        }
    }

    res->normalize();
    res->mUpdateSigners = false;

    res->mKeyCalculated = false;
    return res;
}

bool
AccountFrame::exists(Database& db, LedgerKey const& key)
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(key.account().accountID);
    int exists = 0;
    {
        auto timer = db.getSelectTimer("account-exists");
        db.getSession() << "SELECT EXISTS (SELECT NULL FROM accounts "
                           "WHERE accountid=:v1)",
            use(actIDStrKey), into(exists);
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

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db,
                          LedgerKey const& key)
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(key.account().accountID);

    soci::session& session = db.getSession();
    {
        auto timer = db.getDeleteTimer("account");
        session << "DELETE from accounts where accountid= :v1",
            soci::use(actIDStrKey);
    }
    {
        auto timer = db.getDeleteTimer("signer");
        session << "DELETE from signers where accountid= :v1",
            soci::use(actIDStrKey);
    }
    delta.deleteEntry(key);
}

void
AccountFrame::storeUpdate(LedgerDelta& delta, Database& db, bool insert) const
{
    std::string actIDStrKey = PubKeyUtils::toStrKey(mAccountEntry.accountID);

    std::string sql;

    if (insert)
    {
        sql = std::string(
            "INSERT INTO accounts ( accountid, balance, seqnum, "
            "numsubentries, inflationdest, homedomain, thresholds, flags) "
            "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7 )");
    }
    else
    {
        sql = std::string(
            "UPDATE accounts SET balance = :v1, seqnum = :v2, "
            "numsubentries = :v3, "
            "inflationdest = :v4, homedomain = :v5, thresholds = :v6, "
            "flags = :v7 WHERE accountid = :id");
    }

    auto prep = db.getPreparedStatement(sql);

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStrKey;

    if (mAccountEntry.inflationDest)
    {
        inflationDestStrKey =
            PubKeyUtils::toStrKey(*mAccountEntry.inflationDest);
        inflation_ind = soci::i_ok;
    }

    string thresholds(binToHex(mAccountEntry.thresholds));

    {
        soci::statement& st = prep.statement();
        st.exchange(use(actIDStrKey, "id"));
        st.exchange(use(mAccountEntry.balance, "v1"));
        st.exchange(use(mAccountEntry.seqNum, "v2"));
        st.exchange(use(mAccountEntry.numSubEntries, "v3"));
        st.exchange(use(inflationDestStrKey, inflation_ind, "v4"));
        st.exchange(use(string(mAccountEntry.homeDomain), "v5"));
        st.exchange(use(thresholds, "v6"));
        st.exchange(use(mAccountEntry.flags, "v7"));
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
        // instead separate signatures from account, just like offers are
        // separate entities
        AccountFrame::pointer startAccountFrame;
        startAccountFrame = loadAccount(getID(), db);
        if (!startAccountFrame)
        {
            throw runtime_error("could not load account!");
        }
        AccountEntry& startAccount = startAccountFrame->mAccountEntry;

        // deal with changes to Signers
        if (mAccountEntry.signers.size() < startAccount.signers.size())
        { // some signers were removed
            for (auto const& startSigner : startAccount.signers)
            {
                bool found = false;
                for (auto const& finalSigner : mAccountEntry.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string signerStrKey =
                                PubKeyUtils::toStrKey(finalSigner.pubKey);
                            {
                                auto timer = db.getUpdateTimer("signer");
                                db.getSession()
                                    << "UPDATE signers set weight=:v1 where "
                                       "accountid=:v2 and publickey=:v3",
                                    use(finalSigner.weight), use(actIDStrKey),
                                    use(signerStrKey);
                            }
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                { // delete signer
                    std::string signerStrKey =
                        PubKeyUtils::toStrKey(startSigner.pubKey);

                    soci::statement st =
                        (db.getSession().prepare << "DELETE from signers where "
                                                    "accountid=:v2 and "
                                                    "publickey=:v3",
                         use(actIDStrKey), use(signerStrKey));

                    {
                        auto timer = db.getDeleteTimer("signer");
                        st.execute(true);
                    }

                    if (st.get_affected_rows() != 1)
                    {
                        throw std::runtime_error(
                            "Could not update data in SQL");
                    }
                }
            }
        }
        else
        { // signers added or the same
            for (auto const& finalSigner : mAccountEntry.signers)
            {
                bool found = false;
                for (auto const& startSigner : startAccount.signers)
                {
                    if (finalSigner.pubKey == startSigner.pubKey)
                    {
                        if (finalSigner.weight != startSigner.weight)
                        {
                            std::string signerStrKey =
                                PubKeyUtils::toStrKey(finalSigner.pubKey);

                            soci::statement st =
                                (db.getSession().prepare
                                     << "UPDATE signers set weight=:v1 where "
                                        "accountid=:v2 and publickey=:v3",
                                 use(finalSigner.weight), use(actIDStrKey),
                                 use(signerStrKey));

                            st.execute(true);

                            if (st.get_affected_rows() != 1)
                            {
                                throw std::runtime_error(
                                    "Could not update data in SQL");
                            }
                        }
                        found = true;
                        break;
                    }
                }
                if (!found)
                { // new signer
                    std::string signerStrKey =
                        PubKeyUtils::toStrKey(finalSigner.pubKey);

                    soci::statement st = (db.getSession().prepare
                                              << "INSERT INTO signers "
                                                 "(accountid,publickey,weight) "
                                                 "VALUES (:v1,:v2,:v3)",
                                          use(actIDStrKey), use(signerStrKey),
                                          use(finalSigner.weight));

                    st.execute(true);

                    if (st.get_affected_rows() != 1)
                    {
                        throw std::runtime_error(
                            "Could not update data in SQL");
                    }
                }
            }
        }
    }
}

void
AccountFrame::storeChange(LedgerDelta& delta, Database& db) const
{
    storeUpdate(delta, db, false);
}

void
AccountFrame::storeAdd(LedgerDelta& delta, Database& db) const
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
        v.mInflationDest = PubKeyUtils::fromStrKey(inflationDest);
        if (!inflationProcessor(v))
        {
            break;
        }
        st.fetch();
    }
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
