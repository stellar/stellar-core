// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "crypto/SignerKey.h"
#include "database/Database.h"
#include "database/DatabaseTypeSpecificOperation.h"
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
        if (static_cast<size_t>(st.get_affected_rows()) != 1)
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

class BulkUpsertAccountsOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    std::vector<std::string> mAccountIDs;
    std::vector<int64_t> mBalances;
    std::vector<int64_t> mSeqNums;
    std::vector<int32_t> mSubEntryNums;
    std::vector<std::string> mInflationDests;
    std::vector<soci::indicator> mInflationDestInds;
    std::vector<int32_t> mFlags;
    std::vector<std::string> mHomeDomains;
    std::vector<std::string> mThresholds;
    std::vector<std::string> mSigners;
    std::vector<soci::indicator> mSignerInds;
    std::vector<int32_t> mLastModifieds;
    std::vector<int64_t> mBuyingLiabilities;
    std::vector<int64_t> mSellingLiabilities;
    std::vector<soci::indicator> mLiabilitiesInds;

  public:
    BulkUpsertAccountsOperation(Database& DB,
                                std::vector<EntryIterator> const& entries)
        : mDB(DB)
    {
        mAccountIDs.reserve(entries.size());
        mBalances.reserve(entries.size());
        mSeqNums.reserve(entries.size());
        mSubEntryNums.reserve(entries.size());
        mInflationDests.reserve(entries.size());
        mInflationDestInds.reserve(entries.size());
        mFlags.reserve(entries.size());
        mHomeDomains.reserve(entries.size());
        mThresholds.reserve(entries.size());
        mSigners.reserve(entries.size());
        mSignerInds.reserve(entries.size());
        mLastModifieds.reserve(entries.size());
        mBuyingLiabilities.reserve(entries.size());
        mSellingLiabilities.reserve(entries.size());
        mLiabilitiesInds.reserve(entries.size());

        for (auto const& e : entries)
        {
            assert(e.entryExists());
            assert(e.entry().data.type() == ACCOUNT);
            auto const& account = e.entry().data.account();
            mAccountIDs.emplace_back(KeyUtils::toStrKey(account.accountID));
            mBalances.emplace_back(account.balance);
            mSeqNums.emplace_back(account.seqNum);
            mSubEntryNums.emplace_back(unsignedToSigned(account.numSubEntries));

            if (account.inflationDest)
            {
                mInflationDests.emplace_back(
                    KeyUtils::toStrKey(*account.inflationDest));
                mInflationDestInds.emplace_back(soci::i_ok);
            }
            else
            {
                mInflationDests.emplace_back("");
                mInflationDestInds.emplace_back(soci::i_null);
            }
            mFlags.emplace_back(unsignedToSigned(account.flags));
            mHomeDomains.emplace_back(decoder::encode_b64(account.homeDomain));
            mThresholds.emplace_back(decoder::encode_b64(account.thresholds));
            if (account.signers.empty())
            {
                mSigners.emplace_back("");
                mSignerInds.emplace_back(soci::i_null);
            }
            else
            {
                mSigners.emplace_back(
                    decoder::encode_b64(xdr::xdr_to_opaque(account.signers)));
                mSignerInds.emplace_back(soci::i_ok);
            }
            mLastModifieds.emplace_back(
                unsignedToSigned(e.entry().lastModifiedLedgerSeq));

            if (account.ext.v() >= 1)
            {
                mBuyingLiabilities.emplace_back(
                    account.ext.v1().liabilities.buying);
                mSellingLiabilities.emplace_back(
                    account.ext.v1().liabilities.selling);
                mLiabilitiesInds.emplace_back(soci::i_ok);
            }
            else
            {
                mBuyingLiabilities.emplace_back(0);
                mSellingLiabilities.emplace_back(0);
                mLiabilitiesInds.emplace_back(soci::i_null);
            }
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql =
            "INSERT INTO accounts ( "
            "accountid, balance, seqnum, numsubentries, inflationdest,"
            "homedomain, thresholds, signers, flags, lastmodified, "
            "buyingliabilities, sellingliabilities "
            ") VALUES ( "
            ":id, :v1, :v2, :v3, :v4, :v5, :v6, :v7, :v8, :v9, :v10, :v11 "
            ") ON CONFLICT (accountid) DO UPDATE SET "
            "balance = excluded.balance, "
            "seqnum = excluded.seqnum, "
            "numsubentries = excluded.numsubentries, "
            "inflationdest = excluded.inflationdest, "
            "homedomain = excluded.homedomain, "
            "thresholds = excluded.thresholds, "
            "signers = excluded.signers, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "buyingliabilities = excluded.buyingliabilities, "
            "sellingliabilities = excluded.sellingliabilities";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.exchange(soci::use(mBalances));
        st.exchange(soci::use(mSeqNums));
        st.exchange(soci::use(mSubEntryNums));
        st.exchange(soci::use(mInflationDests, mInflationDestInds));
        st.exchange(soci::use(mHomeDomains));
        st.exchange(soci::use(mThresholds));
        st.exchange(soci::use(mSigners, mSignerInds));
        st.exchange(soci::use(mFlags));
        st.exchange(soci::use(mLastModifieds));
        st.exchange(soci::use(mBuyingLiabilities, mLiabilitiesInds));
        st.exchange(soci::use(mSellingLiabilities, mLiabilitiesInds));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("account");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    void
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        doSociGenericOperation();
    }

#ifdef USE_POSTGRES
    void
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        std::string strAccountIDs, strBalances, strSeqNums, strSubEntryNums,
            strInflationDests, strFlags, strHomeDomains, strThresholds,
            strSigners, strLastModifieds, strBuyingLiabilities,
            strSellingLiabilities;

        PGconn* conn = pg->conn_;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        marshalToPGArray(conn, strBalances, mBalances);
        marshalToPGArray(conn, strSeqNums, mSeqNums);
        marshalToPGArray(conn, strSubEntryNums, mSubEntryNums);
        marshalToPGArray(conn, strInflationDests, mInflationDests,
                         &mInflationDestInds);
        marshalToPGArray(conn, strFlags, mFlags);
        marshalToPGArray(conn, strHomeDomains, mHomeDomains);
        marshalToPGArray(conn, strThresholds, mThresholds);
        marshalToPGArray(conn, strSigners, mSigners, &mSignerInds);
        marshalToPGArray(conn, strLastModifieds, mLastModifieds);
        marshalToPGArray(conn, strBuyingLiabilities, mBuyingLiabilities,
                         &mLiabilitiesInds);
        marshalToPGArray(conn, strSellingLiabilities, mSellingLiabilities,
                         &mLiabilitiesInds);

        std::string sql =
            "WITH r AS (SELECT "
            "unnest(:ids::TEXT[]), "
            "unnest(:v1::BIGINT[]), "
            "unnest(:v2::BIGINT[]), "
            "unnest(:v3::INT[]), "
            "unnest(:v4::TEXT[]), "
            "unnest(:v5::TEXT[]), "
            "unnest(:v6::TEXT[]), "
            "unnest(:v7::TEXT[]), "
            "unnest(:v8::INT[]), "
            "unnest(:v9::INT[]), "
            "unnest(:v10::BIGINT[]), "
            "unnest(:v11::BIGINT[]) "
            ")"
            "INSERT INTO accounts ( "
            "accountid, balance, seqnum, "
            "numsubentries, inflationdest, homedomain, thresholds, signers, "
            "flags, lastmodified, buyingliabilities, sellingliabilities "
            ") SELECT * FROM r "
            "ON CONFLICT (accountid) DO UPDATE SET "
            "balance = excluded.balance, "
            "seqnum = excluded.seqnum, "
            "numsubentries = excluded.numsubentries, "
            "inflationdest = excluded.inflationdest, "
            "homedomain = excluded.homedomain, "
            "thresholds = excluded.thresholds, "
            "signers = excluded.signers, "
            "flags = excluded.flags, "
            "lastmodified = excluded.lastmodified, "
            "buyingliabilities = excluded.buyingliabilities, "
            "sellingliabilities = excluded.sellingliabilities";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.exchange(soci::use(strBalances));
        st.exchange(soci::use(strSeqNums));
        st.exchange(soci::use(strSubEntryNums));
        st.exchange(soci::use(strInflationDests));
        st.exchange(soci::use(strHomeDomains));
        st.exchange(soci::use(strThresholds));
        st.exchange(soci::use(strSigners));
        st.exchange(soci::use(strFlags));
        st.exchange(soci::use(strLastModifieds));
        st.exchange(soci::use(strBuyingLiabilities));
        st.exchange(soci::use(strSellingLiabilities));
        st.define_and_bind();
        {
            auto timer = mDB.getUpsertTimer("account");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size())
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

class BulkDeleteAccountsOperation : public DatabaseTypeSpecificOperation<void>
{
    Database& mDB;
    LedgerTxnConsistency mCons;
    std::vector<std::string> mAccountIDs;

  public:
    BulkDeleteAccountsOperation(Database& DB, LedgerTxnConsistency cons,
                                std::vector<EntryIterator> const& entries)
        : mDB(DB), mCons(cons)
    {
        for (auto const& e : entries)
        {
            assert(!e.entryExists());
            assert(e.key().type() == ACCOUNT);
            auto const& account = e.key().account();
            mAccountIDs.emplace_back(KeyUtils::toStrKey(account.accountID));
        }
    }

    void
    doSociGenericOperation()
    {
        std::string sql = "DELETE FROM accounts WHERE accountid = :id";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(mAccountIDs));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("account");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    void
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        doSociGenericOperation();
    }

#ifdef USE_POSTGRES
    void
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {
        PGconn* conn = pg->conn_;
        std::string strAccountIDs;
        marshalToPGArray(conn, strAccountIDs, mAccountIDs);
        std::string sql =
            "WITH r AS (SELECT unnest(:ids::TEXT[])) "
            "DELETE FROM accounts WHERE accountid IN (SELECT * FROM r)";
        auto prep = mDB.getPreparedStatement(sql);
        soci::statement& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        st.define_and_bind();
        {
            auto timer = mDB.getDeleteTimer("account");
            st.execute(true);
        }
        if (static_cast<size_t>(st.get_affected_rows()) != mAccountIDs.size() &&
            mCons == LedgerTxnConsistency::EXACT)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }
#endif
};

void
LedgerTxnRoot::Impl::bulkUpsertAccounts(
    std::vector<EntryIterator> const& entries)
{
    BulkUpsertAccountsOperation op(mDatabase, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
}

void
LedgerTxnRoot::Impl::bulkDeleteAccounts(
    std::vector<EntryIterator> const& entries, LedgerTxnConsistency cons)
{
    BulkDeleteAccountsOperation op(mDatabase, cons, entries);
    mDatabase.doDatabaseTypeSpecificOperation(op);
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
    if (static_cast<size_t>(st.get_affected_rows()) != 1)
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

class BulkLoadAccountsOperation
    : public DatabaseTypeSpecificOperation<std::vector<LedgerEntry>>
{
    Database& mDb;
    std::vector<std::string> mAccountIDs;

    std::vector<LedgerEntry>
    executeAndFetch(soci::statement& st)
    {
        std::string accountID, inflationDest, homeDomain, thresholds, signers;
        int64_t balance;
        uint64_t seqNum;
        uint32_t numSubEntries, flags, lastModified;
        Liabilities liabilities;
        soci::indicator inflationDestInd, signersInd, buyingLiabilitiesInd,
            sellingLiabilitiesInd;

        st.exchange(soci::into(accountID));
        st.exchange(soci::into(balance));
        st.exchange(soci::into(seqNum));
        st.exchange(soci::into(numSubEntries));
        st.exchange(soci::into(inflationDest, inflationDestInd));
        st.exchange(soci::into(homeDomain));
        st.exchange(soci::into(thresholds));
        st.exchange(soci::into(flags));
        st.exchange(soci::into(lastModified));
        st.exchange(soci::into(liabilities.buying, buyingLiabilitiesInd));
        st.exchange(soci::into(liabilities.selling, sellingLiabilitiesInd));
        st.exchange(soci::into(signers, signersInd));
        st.define_and_bind();
        {
            auto timer = mDb.getSelectTimer("account");
            st.execute(true);
        }

        std::vector<LedgerEntry> res;
        while (st.got_data())
        {
            res.emplace_back();
            auto& le = res.back();
            le.data.type(ACCOUNT);
            auto& ae = le.data.account();

            ae.accountID = KeyUtils::fromStrKey<PublicKey>(accountID);
            ae.balance = balance;
            ae.seqNum = seqNum;
            ae.numSubEntries = numSubEntries;

            if (inflationDestInd == soci::i_ok)
            {
                ae.inflationDest.activate() =
                    KeyUtils::fromStrKey<PublicKey>(inflationDest);
            }

            decoder::decode_b64(homeDomain, ae.homeDomain);

            bn::decode_b64(thresholds.begin(), thresholds.end(),
                           ae.thresholds.begin());

            if (inflationDestInd == soci::i_ok)
            {
                ae.inflationDest.activate() =
                    KeyUtils::fromStrKey<PublicKey>(inflationDest);
            }

            ae.flags = flags;
            le.lastModifiedLedgerSeq = lastModified;

            assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
            if (buyingLiabilitiesInd == soci::i_ok)
            {
                ae.ext.v(1);
                ae.ext.v1().liabilities = liabilities;
            }

            if (signersInd == soci::i_ok)
            {
                std::vector<uint8_t> signersOpaque;
                decoder::decode_b64(signers, signersOpaque);
                xdr::xdr_from_opaque(signersOpaque, ae.signers);
                assert(std::adjacent_find(
                           ae.signers.begin(), ae.signers.end(),
                           [](Signer const& lhs, Signer const& rhs) {
                               return !(lhs.key < rhs.key);
                           }) == ae.signers.end());
            }

            st.fetch();
        }
        return res;
    }

  public:
    BulkLoadAccountsOperation(Database& db,
                              std::unordered_set<LedgerKey> const& keys)
        : mDb(db)
    {
        mAccountIDs.reserve(keys.size());
        for (auto const& k : keys)
        {
            assert(k.type() == ACCOUNT);
            mAccountIDs.emplace_back(KeyUtils::toStrKey(k.account().accountID));
        }
    }

    virtual std::vector<LedgerEntry>
    doSqliteSpecificOperation(soci::sqlite3_session_backend* sq) override
    {
        std::vector<char const*> accountIDcstrs;
        accountIDcstrs.reserve(mAccountIDs.size());
        for (auto const& acc : mAccountIDs)
        {
            accountIDcstrs.emplace_back(acc.c_str());
        }

        std::string sql =
            "SELECT accountid, balance, seqnum, numsubentries, "
            "inflationdest, homedomain, thresholds, flags, lastmodified, "
            "buyingliabilities, sellingliabilities, signers FROM accounts "
            "WHERE accountid IN carray(?, ?, 'char*')";

        auto prep = mDb.getPreparedStatement(sql);
        auto sqliteStatement = dynamic_cast<soci::sqlite3_statement_backend*>(
            prep.statement().get_backend());
        auto st = sqliteStatement->stmt_;

        sqlite3_reset(st);
        sqlite3_bind_pointer(st, 1, accountIDcstrs.data(), "carray", 0);
        sqlite3_bind_int(st, 2, accountIDcstrs.size());
        return executeAndFetch(prep.statement());
    }

#ifdef USE_POSTGRES
    virtual std::vector<LedgerEntry>
    doPostgresSpecificOperation(soci::postgresql_session_backend* pg) override
    {

        std::string strAccountIDs;
        marshalToPGArray(pg->conn_, strAccountIDs, mAccountIDs);

        std::string sql =
            "WITH r AS (SELECT unnest(:v1::TEXT[])) "
            "SELECT accountid, balance, seqnum, numsubentries, "
            "inflationdest, homedomain, thresholds, flags, lastmodified, "
            "buyingliabilities, sellingliabilities, signers FROM accounts "
            "WHERE accountid IN (SELECT * FROM r)";

        auto prep = mDb.getPreparedStatement(sql);
        auto& st = prep.statement();
        st.exchange(soci::use(strAccountIDs));
        return executeAndFetch(st);
    }
#endif
};

std::unordered_map<LedgerKey, std::shared_ptr<LedgerEntry const>>
LedgerTxnRoot::Impl::bulkLoadAccounts(
    std::unordered_set<LedgerKey> const& keys) const
{
    BulkLoadAccountsOperation op(mDatabase, keys);
    return populateLoadedEntries(keys,
                                 mDatabase.doDatabaseTypeSpecificOperation(op));
}
}
