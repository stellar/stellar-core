// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdrpp/marshal.h"

namespace stellar
{

std::shared_ptr<LedgerEntry const>
LedgerTxnRoot::Impl::loadAccount(LedgerKey const& key) const
{
    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    std::string inflationDest, homeDomain, thresholds, signers;
    soci::indicator inflationDestInd, signersInd;
    Liabilities liabilities;
    soci::indicator buyingLiabilitiesInd, sellingLiabilitiesInd;

    LedgerEntry le;
    le.data.type(ACCOUNT);
    auto& account = le.data.account();

    auto prep =
        mDatabase.getPreparedStatement("SELECT balance, seqnum, numsubentries, "
                                       "inflationdest, homedomain, thresholds, "
                                       "flags, lastmodified, "
                                       "buyingliabilities, sellingliabilities, "
                                       "signers "
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
    st.exchange(soci::into(signers, signersInd));
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
    decoder::decode_b64(homeDomain, account.homeDomain);

    bn::decode_b64(thresholds.begin(), thresholds.end(),
                   account.thresholds.begin());

    if (inflationDestInd == soci::i_ok)
    {
        account.inflationDest.activate() =
            KeyUtils::fromStrKey<PublicKey>(inflationDest);
    }

    if (signersInd == soci::i_ok)
    {
        std::vector<uint8_t> signersOpaque;
        decoder::decode_b64(signers, signersOpaque);
        xdr::xdr_from_opaque(signersOpaque, account.signers);
        assert(std::adjacent_find(account.signers.begin(),
                                  account.signers.end(),
                                  [](Signer const& lhs, Signer const& rhs) {
                                      return !(lhs.key < rhs.key);
                                  }) == account.signers.end());
    }

    assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
    if (buyingLiabilitiesInd == soci::i_ok)
    {
        account.ext.v(1);
        account.ext.v1().liabilities = liabilities;
    }

    return std::make_shared<LedgerEntry const>(std::move(le));
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
LedgerTxnRoot::Impl::writeSignersTableIntoAccountsTable()
{
    throwIfChild();
    soci::transaction sqlTx(mDatabase.getSession());

    CLOG(INFO, "Ledger") << "Loading all signers from signers table";
    std::map<std::string, xdr::xvector<Signer, 20>> signersByAccount;

    {
        std::string accountIDStrKey, pubKey;
        Signer signer;

        auto prep = mDatabase.getPreparedStatement(
            "SELECT accountid, publickey, weight FROM signers");
        auto& st = prep.statement();
        st.exchange(soci::into(accountIDStrKey));
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
            signersByAccount[accountIDStrKey].emplace_back(signer);
            st.fetch();
        }
    }

    size_t numAccountsUpdated = 0;
    for (auto& kv : signersByAccount)
    {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](Signer const& lhs, Signer const& rhs) {
                      return lhs.key < rhs.key;
                  });
        std::string signers(decoder::encode_b64(xdr::xdr_to_opaque(kv.second)));

        auto prep = mDatabase.getPreparedStatement(
            "UPDATE accounts SET signers = :v1 WHERE accountID = :id");
        auto& st = prep.statement();
        st.exchange(soci::use(signers, "v1"));
        st.exchange(soci::use(kv.first, "id"));
        st.define_and_bind();
        st.execute(true);
        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }

        if ((++numAccountsUpdated & 0xfff) == 0xfff ||
            (numAccountsUpdated == signersByAccount.size()))
        {
            CLOG(INFO, "Ledger")
                << "Wrote signers for " << numAccountsUpdated << " accounts";
        }
    }

    sqlTx.commit();

    // Clearing the cache does not throw
    mEntryCache.clear();
    mBestOffersCache.clear();
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
    std::string homeDomain(decoder::encode_b64(account.homeDomain));

    soci::indicator signersInd = soci::i_null;
    std::string signers;
    if (!account.signers.empty())
    {
        signers = decoder::encode_b64(xdr::xdr_to_opaque(account.signers));
        signersInd = soci::i_ok;
    }

    std::string sql;
    if (isInsert)
    {
        sql = "INSERT INTO accounts ( accountid, balance, seqnum, "
              "numsubentries, inflationdest, homedomain, thresholds, flags, "
              "lastmodified, buyingliabilities, sellingliabilities, signers ) "
              "VALUES ( :id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, "
              ":v10, :v11 )";
    }
    else
    {
        sql = "UPDATE accounts SET balance = :v1, seqnum = :v2, "
              "numsubentries = :v3, inflationdest = :v4, homedomain = :v5, "
              "thresholds = :v6, flags = :v7, lastmodified = :v8, "
              "buyingliabilities = :v9, sellingliabilities = :v10, "
              "signers = :v11 WHERE accountid = :id";
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
    st.exchange(soci::use(signers, signersInd, "v11"));
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

static std::vector<std::pair<std::string, std::string>>
loadHomeDomainsToEncode(Database& db)
{
    std::string accountID, homeDomain;

    std::string sql = "SELECT accountid, homedomain FROM accounts "
                      "WHERE length(homedomain) > 0";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::into(accountID));
    st.exchange(soci::into(homeDomain));
    st.define_and_bind();
    st.execute(true);

    std::vector<std::pair<std::string, std::string>> res;
    while (st.got_data())
    {
        res.emplace_back(accountID, homeDomain);
        st.fetch();
    }
    return res;
}

static void
writeEncodedHomeDomain(Database& db, std::string const& accountID,
                       std::string const& homeDomain)
{
    std::string encodedHomeDomain = decoder::encode_b64(homeDomain);

    std::string sql =
        "UPDATE accounts SET homedomain = :v1 WHERE accountid = :v2";

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(soci::use(encodedHomeDomain));
    st.exchange(soci::use(accountID));
    st.define_and_bind();
    st.execute(true);
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }
}

void
LedgerTxnRoot::Impl::encodeHomeDomainsBase64()
{
    throwIfChild();
    mEntryCache.clear();
    mBestOffersCache.clear();

    CLOG(INFO, "Ledger") << "Loading all home domains from accounts table";
    auto homeDomainsToEncode = loadHomeDomainsToEncode(mDatabase);

    if (!mDatabase.isSqlite())
    {
        auto& session = mDatabase.getSession();
        session << "ALTER TABLE accounts ALTER COLUMN homedomain "
                   "SET DATA TYPE VARCHAR(44)";
    }

    size_t numUpdated = 0;
    for (auto const& kv : homeDomainsToEncode)
    {
        writeEncodedHomeDomain(mDatabase, kv.first, kv.second);

        if ((++numUpdated & 0xfff) == 0xfff ||
            (numUpdated == homeDomainsToEncode.size()))
        {
            CLOG(INFO, "Ledger")
                << "Wrote home domains for " << numUpdated << " accounts";
        }
    }
}
}
