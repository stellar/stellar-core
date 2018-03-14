// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "util/basen.h"
#include "util/types.h"

namespace stellar
{
using xdr::operator<;

LedgerState::StateEntry
LedgerState::loadAccountFromDatabase(LedgerKey const& key)
{
    auto& db = mRoot->getDatabase();

    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    std::string inflationDest, homeDomain, thresholds;
    soci::indicator inflationDestInd;

    LedgerEntry le;
    le.data.type(ACCOUNT);
    auto& account = le.data.account();

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
    st.exchange(soci::into(le.lastModifiedLedgerSeq));
    st.exchange(soci::use(actIDStrKey));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("account");
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
        auto signers = loadSignersFromDatabase(key);
        account.signers.insert(account.signers.begin(), signers.begin(),
                               signers.end());
    }

    auto entry = std::make_shared<LedgerEntry>(std::move(le));
    return makeStateEntry(entry, entry);
}

void
LedgerState::storeAccountInDatabase(StateEntry const& state)
{
    bool isInsert = !state->ignoreInvalid().previousEntry();
    auto& db = mRoot->getDatabase();

    auto const& entry = *state->ignoreInvalid().entry();
    auto const& account = entry.data.account();
    std::string actIDStrKey = KeyUtils::toStrKey(account.accountID);

    soci::indicator inflation_ind = soci::i_null;
    std::string inflationDestStrKey;
    if (account.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*account.inflationDest);
        inflation_ind = soci::i_ok;
    }

    std::string thresholds(bn::encode_b64(account.thresholds));
    std::string homeDomain(account.homeDomain);

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO accounts ( accountid, balance, seqnum, "
              "numsubentries, inflationdest, homedomain, thresholds, flags, "
              "lastmodified ) "
              "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8 )";
    }
    else
    {
        sql = "UPDATE accounts SET balance = :v1, seqnum = :v2, "
              "numsubentries = :v3, inflationdest = :v4, homedomain = :v5, "
              "thresholds = :v6, flags = :v7, lastmodified = :v8 "
              "WHERE accountid = :id";
    }
    auto prep = db.getPreparedStatement(sql);
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
    st.define_and_bind();
    {
        auto timer = isInsert ? db.getInsertTimer("account")
                              : db.getUpdateTimer("account");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    storeSignersInDatabase(state);
}

void
LedgerState::deleteAccountFromDatabase(StateEntry const& state)
{
    auto& db = mRoot->getDatabase();

    auto const& previousEntry = state->ignoreInvalid().previousEntry();
    auto const& account = previousEntry->data.account();
    std::string actIDStrKey = KeyUtils::toStrKey(account.accountID);

    {
        auto prep = db.getPreparedStatement(
            "DELETE FROM accounts WHERE accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        {
            auto timer = db.getDeleteTimer("account");
            st.execute(true);
        }
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    {
        auto prep =
            db.getPreparedStatement("DELETE FROM signers WHERE accountid= :v1");
        auto& st = prep.statement();
        st.exchange(soci::use(actIDStrKey));
        st.define_and_bind();
        {
            auto timer = db.getDeleteTimer("signer");
            st.execute(true);
        }
    }
}

std::vector<Signer>
LedgerState::loadSignersFromDatabase(LedgerKey const& key)
{
    auto& db = mRoot->getDatabase();
    std::vector<Signer> res;

    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    std::string pubKey;
    Signer signer;

    auto prep = db.getPreparedStatement("SELECT publickey, weight FROM signers "
                                        "WHERE accountid =:id");
    auto& st = prep.statement();
    st.exchange(soci::use(actIDStrKey));
    st.exchange(soci::into(pubKey));
    st.exchange(soci::into(signer.weight));
    st.define_and_bind();
    {
        auto timer = db.getSelectTimer("signer");
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

void
LedgerState::storeSignersInDatabase(StateEntry const& state)
{
    auto& db = mRoot->getDatabase();

    auto const& entry = state->ignoreInvalid().entry();
    auto const& account = entry->data.account();
    std::string actIDStrKey = KeyUtils::toStrKey(account.accountID);
    assert(std::adjacent_find(account.signers.begin(), account.signers.end(),
                              [](Signer const& lhs, Signer const& rhs) {
                                  return !(lhs.key < rhs.key);
                              }) == account.signers.end());

    auto const& previousEntry = state->ignoreInvalid().previousEntry();
    std::vector<Signer> signers;
    if (previousEntry)
    {
        auto key = LedgerEntryKey(*previousEntry);
        signers = loadSignersFromDatabase(key);
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
                auto timer = db.getUpdateTimer("signer");
                auto prep = db.getPreparedStatement(
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

            auto prep = db.getPreparedStatement("INSERT INTO signers "
                                                "(accountid,publickey,weight) "
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

            auto prep = db.getPreparedStatement("DELETE from signers WHERE "
                                                "accountid=:v2 AND "
                                                "publickey=:v3");
            auto& st = prep.statement();
            st.exchange(soci::use(actIDStrKey));
            st.exchange(soci::use(signerStrKey));
            st.define_and_bind();
            {
                auto timer = db.getDeleteTimer("signer");
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

std::vector<InflationVotes>
LedgerState::loadInflationWinnersFromDatabase(size_t maxWinners,
                                              int64_t minBalance)
{
    auto& db = mRoot->getDatabase();
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
         soci::into(v.votes), soci::into(inflationDest), soci::use(maxWinners));
    st.execute(true);

    std::vector<InflationVotes> winners;
    while (st.got_data())
    {
        v.inflationDest = KeyUtils::fromStrKey<PublicKey>(inflationDest);
        if (v.votes < minBalance)
        {
            break;
        }
        winners.push_back(v);
        st.fetch();
    }
    return winners;
}
}
