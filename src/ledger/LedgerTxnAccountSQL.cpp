// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/Decoder.h"
#include "util/XDROperators.h"
#include "util/types.h"

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadAccount(LedgerKey const& key) const
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    std::string inflationDest, homeDomain, thresholds;
    soci::indicator inflationDestInd;
    Liabilities liabilities;
    soci::indicator buyingLiabilitiesInd, sellingLiabilitiesInd;

    LedgerEntry le;
    le.data.type(ACCOUNT);
    auto& account = le.data.account();

    auto prep =
        mDatabase.getPreparedStatement("SELECT balance, seqnum, numsubentries, "
                                       "inflationdest, homedomain, thresholds, "
                                       "flags, lastmodified, "
                                       "buyingliabilities, sellingliabilities "
                                       "FROM accounts WHERE accountid=:v1");
    auto& st = prep.statement();
    st.exchange(soci::into(account.balance));
    st.exchange(soci::into(account.seqNum));
    st.exchange(soci::into(account.numSubEntries));
    st.exchange(soci::into(inflationDest, inflationDestInd));
    st.exchange(soci::into(homeDomain));
    st.exchange(soci::into(thresholds));
    st.exchange(soci::into(account.flags));
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::into(liabilities.buying, buyingLiabilitiesInd));
    st.exchange(soci::into(liabilities.selling, sellingLiabilitiesInd));
    st.exchange(soci::use(actIDStrKey));
    st.define_and_bind();
    {
        auto timer = mDatabase.getSelectTimer("account");
        st.execute(true);
    }
    if (!st.got_data())
    {
        return nullptr;
    }

    account.accountID = key.account().accountID;
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
        auto signers = loadSigners(key);
        account.signers.insert(account.signers.begin(), signers.begin(),
                               signers.end());
    }

    assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
    if (buyingLiabilitiesInd == soci::i_ok)
    {
        account.ext.v(1);
        account.ext.v1().liabilities = liabilities;
    }

    return std::make_shared<LedgerEntry const>(std::move(le));
}

std::vector<Signer>
LedgerTxnRoot::Impl::loadSigners(LedgerKey const& key) const
{
    std::vector<Signer> res;

    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    std::string pubKey;
    Signer signer;

    auto prep = mDatabase.getPreparedStatement(
        "SELECT publickey, weight FROM signers WHERE accountid =:id");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::into(pubKey));
    st.exchange(soci::into(signer.weight));
    st.define_and_bind();
    {
        auto timer = mDatabase.getSelectTimer("signer");
        st.execute(true);
    }
    while (st.got_data())
    {
        signer.key = KeyUtils::fromStrKey<SignerKey>(pubKey);
        res.push_back(signer);
        st.fetch();
    }

    std::sort(res.begin(), res.end(), [](Signer const& lhs, Signer const& rhs) {
        return lhs.key < rhs.key;
    });
    return res;
}

std::vector<InflationWinner>
LedgerTxnRoot::Impl::loadInflationWinners(size_t maxWinners,
                                          int64_t minBalance) const
{
    InflationWinner w;
    std::string inflationDest;

    auto prep = mDatabase.getPreparedStatement(
        "SELECT sum(balance) AS votes, inflationdest"
        " FROM accounts WHERE inflationdest IS NOT NULL"
        " AND balance >= 1000000000 GROUP BY inflationdest"
        " ORDER BY votes DESC, inflationdest DESC LIMIT :lim");
    auto& st = prep.statement();
    st.exchange(soci::into(w.votes));
    st.exchange(soci::into(inflationDest));
    st.exchange(soci::use(maxWinners));
    st.define_and_bind();
    st.execute(true);

    std::vector<InflationWinner> winners;
    while (st.got_data())
    {
        w.accountID = KeyUtils::fromStrKey<PublicKey>(inflationDest);
        if (w.votes < minBalance)
        {
            break;
        }
        winners.push_back(w);
        st.fetch();
    }
    return winners;
}

void
LedgerTxnRoot::Impl::insertOrUpdateAccount(LedgerEntry const& entry,
                                           bool isInsert)
{
    auto const& account = entry.data.account();
    std::string actIDStrKey = KeyUtils::toStrKey(account.accountID);

    soci::indicator inflation_ind = soci::i_null;
    std::string inflationDestStrKey;
    if (account.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*account.inflationDest);
        inflation_ind = soci::i_ok;
    }

    Liabilities liabilities;
    soci::indicator liabilitiesInd = soci::i_null;
    if (account.ext.v() == 1)
    {
        liabilities = account.ext.v1().liabilities;
        liabilitiesInd = soci::i_ok;
    }

    std::string thresholds(decoder::encode_b64(account.thresholds));
    std::string homeDomain(account.homeDomain);

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO accounts ( accountid, balance, seqnum, "
              "numsubentries, inflationdest, homedomain, thresholds, flags, "
              "lastmodified, buyingliabilities, sellingliabilities ) "
              "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10 "
              ")";
    }
    else
    {
        sql = "UPDATE accounts SET balance = :v1, seqnum = :v2, "
              "numsubentries = :v3, inflationdest = :v4, homedomain = :v5, "
              "thresholds = :v6, flags = :v7, lastmodified = :v8, "
              "buyingliabilities = :v9, sellingliabilities = :v10 "
              "WHERE accountid = :id";
    }
    auto prep = mDatabase.getPreparedStatement(sql);
    soci::statement& st = prep.statement();
    st.exchange(soci::use(actIDStrKey, "id"));
    st.exchange(soci::use(account.balance, "v1"));
    st.exchange(soci::use(account.seqNum, "v2"));
    st.exchange(soci::use(account.numSubEntries, "v3"));
    st.exchange(soci::use(inflationDestStrKey, inflation_ind, "v4"));
    st.exchange(soci::use(homeDomain, "v5"));
    st.exchange(soci::use(thresholds, "v6"));
    st.exchange(soci::use(account.flags, "v7"));
    st.exchange(soci::use(entry.lastModifiedLedgerSeq, "v8"));
    st.exchange(soci::use(liabilities.buying, liabilitiesInd, "v9"));
    st.exchange(soci::use(liabilities.selling, liabilitiesInd, "v10"));
    st.define_and_bind();
    {
        auto timer = isInsert ? mDatabase.getInsertTimer("account")
                              : mDatabase.getUpdateTimer("account");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}

void
LedgerTxnRoot::Impl::storeSigners(
    LedgerEntry const& entry,
    std::shared_ptr<LedgerEntry const> const& previous)
{
    auto const& account = entry.data.account();
    std::string actIDStrKey = KeyUtils::toStrKey(account.accountID);
    assert(std::adjacent_find(account.signers.begin(), account.signers.end(),
                              [](Signer const& lhs, Signer const& rhs) {
                                  return !(lhs.key < rhs.key);
                              }) == account.signers.end());

    std::vector<Signer> signers;
    if (previous)
    {
        signers = previous->data.account().signers;
        assert(std::adjacent_find(signers.begin(), signers.end(),
                                  [](Signer const& lhs, Signer const& rhs) {
                                      return !(lhs.key < rhs.key);
                                  }) == signers.end());
    }

    auto it_new = account.signers.begin();
    auto it_old = signers.begin();
    while (it_new != account.signers.end() || it_old != signers.end())
    {
        bool updated = false, added = false;

        if (it_old == signers.end())
        {
            added = true;
        }
        else if (it_new != account.signers.end())
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
                auto timer = mDatabase.getUpdateTimer("signer");
                auto prep = mDatabase.getPreparedStatement(
                    "UPDATE signers set weight=:v1 WHERE "
                    "accountid=:v2 AND publickey=:v3");
                auto& st = prep.statement();
                st.exchange(soci::use(it_new->weight));
                st.exchange(soci::use(actIDStrKey));
                st.exchange(soci::use(signerStrKey));
                st.define_and_bind();
                st.execute(true);
                if (st.get_affected_rows() != 1)
                {
                    throw std::runtime_error("Could not update data in SQL");
                }
            }
            it_new++;
            it_old++;
        }
        else if (added)
        {
            // signer was added
            std::string signerStrKey = KeyUtils::toStrKey(it_new->key);

            auto prep = mDatabase.getPreparedStatement(
                "INSERT INTO signers (accountid,publickey,weight) "
                "VALUES (:v1,:v2,:v3)");
            auto& st = prep.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(signerStrKey));
            st.exchange(soci::use(it_new->weight));
            st.define_and_bind();
            st.execute(true);
            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
            it_new++;
        }
        else
        {
            // signer was deleted
            std::string signerStrKey = KeyUtils::toStrKey(it_old->key);

            auto prep = mDatabase.getPreparedStatement(
                "DELETE from signers WHERE accountid=:v2 AND publickey=:v3");
            auto& st = prep.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(signerStrKey));
            st.define_and_bind();
            {
                auto timer = mDatabase.getDeleteTimer("signer");
                st.execute(true);
            }
            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
            it_old++;
        }
    }
}

void
LedgerTxnRoot::Impl::deleteAccount(LedgerKey const& key)
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    {
        auto prep = mDatabase.getPreparedStatement(
            "DELETE FROM accounts WHERE accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        {
            auto timer = mDatabase.getDeleteTimer("account");
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    {
        auto prep = mDatabase.getPreparedStatement(
            "DELETE FROM signers WHERE accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        {
            auto timer = mDatabase.getDeleteTimer("signer");
            st.execute(true);
        }
    }
}

void
LedgerTxnRoot::Impl::dropAccounts()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    mDatabase.getSession() << "DROP TABLE IF EXISTS accounts;";
    mDatabase.getSession() << "DROP TABLE IF EXISTS signers;";

    mDatabase.getSession()
        << "CREATE TABLE accounts"
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
    mDatabase.getSession() << "CREATE TABLE signers"
                              "("
                              "accountid       VARCHAR(56) NOT NULL,"
                              "publickey       VARCHAR(56) NOT NULL,"
                              "weight          INT         NOT NULL,"
                              "PRIMARY KEY (accountid, publickey)"
                              ");";
    mDatabase.getSession()
        << "CREATE INDEX signersaccount ON signers (accountid)";
    mDatabase.getSession()
        << "CREATE INDEX accountbalances ON accounts (balance) WHERE "
           "balance >= 1000000000";
}
}
