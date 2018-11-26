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
#include "util/Decoder.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include <algorithm>

using namespace soci;
using namespace std;

namespace stellar
{
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

int64_t
getBuyingLiabilities(AccountEntry const& acc, LedgerManager const& lm)
{
    assert(lm.getCurrentLedgerVersion() >= 10);
    return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.buying;
}

int64_t
getSellingLiabilities(AccountEntry const& acc, LedgerManager const& lm)
{
    assert(lm.getCurrentLedgerVersion() >= 10);
    return (acc.ext.v() == 0) ? 0 : acc.ext.v1().liabilities.selling;
}

int64_t
AccountFrame::getBuyingLiabilities(LedgerManager const& lm) const
{
    return stellar::getBuyingLiabilities(mAccountEntry, lm);
}

int64_t
AccountFrame::getSellingLiabilities(LedgerManager const& lm) const
{
    return stellar::getSellingLiabilities(mAccountEntry, lm);
}

bool
AccountFrame::addBuyingLiabilities(int64_t delta, LedgerManager const& lm)
{
    assert(lm.getCurrentLedgerVersion() >= 10);
    assert(getBalance() >= 0);
    if (delta == 0)
    {
        return true;
    }
    int64_t buyingLiab = (mAccountEntry.ext.v() == 0)
                             ? 0
                             : mAccountEntry.ext.v1().liabilities.buying;

    int64_t maxLiabilities = INT64_MAX - getBalance();
    bool res = stellar::addBalance(buyingLiab, delta, maxLiabilities);
    if (res)
    {
        if (mAccountEntry.ext.v() == 0)
        {
            mAccountEntry.ext.v(1);
            mAccountEntry.ext.v1().liabilities = Liabilities{0, 0};
        }
        mAccountEntry.ext.v1().liabilities.buying = buyingLiab;
    }
    return res;
}

bool
AccountFrame::addSellingLiabilities(int64_t delta, LedgerManager const& lm)
{
    assert(lm.getCurrentLedgerVersion() >= 10);
    assert(getBalance() >= 0);
    if (delta == 0)
    {
        return true;
    }
    int64_t sellingLiab = (mAccountEntry.ext.v() == 0)
                              ? 0
                              : mAccountEntry.ext.v1().liabilities.selling;

    int64_t maxLiabilities = getBalance() - getMinimumBalance(lm);
    if (maxLiabilities < 0)
    {
        return false;
    }

    bool res = stellar::addBalance(sellingLiab, delta, maxLiabilities);
    if (res)
    {
        if (mAccountEntry.ext.v() == 0)
        {
            mAccountEntry.ext.v(1);
            mAccountEntry.ext.v1().liabilities = Liabilities{0, 0};
        }
        mAccountEntry.ext.v1().liabilities.selling = sellingLiab;
    }
    return res;
}

bool
AccountFrame::addBalance(int64_t delta, LedgerManager const& lm)
{
    if (delta == 0)
    {
        return true;
    }

    auto newBalance = mAccountEntry.balance;
    if (!stellar::addBalance(newBalance, delta))
    {
        return false;
    }
    if (lm.getCurrentLedgerVersion() >= 10)
    {
        if (delta < 0 &&
            newBalance - getMinimumBalance(lm) < getSellingLiabilities(lm))
        {
            return false;
        }
        if (newBalance > INT64_MAX - getBuyingLiabilities(lm))
        {
            return false;
        }
    }

    mAccountEntry.balance = newBalance;
    return true;
}

int64_t
AccountFrame::getMinimumBalance(LedgerManager const& lm) const
{
    return lm.getMinBalance(mAccountEntry.numSubEntries);
}

int64_t
AccountFrame::getAvailableBalance(LedgerManager const& lm) const
{
    int64_t avail =
        getBalance() - lm.getMinBalance(mAccountEntry.numSubEntries);
    if (lm.getCurrentLedgerVersion() >= 10)
    {
        avail -= getSellingLiabilities(lm);
    }
    return avail;
}

int64_t
AccountFrame::getMaxAmountReceive(LedgerManager const& lm) const
{
    if (lm.getCurrentLedgerVersion() >= 10)
    {
        return INT64_MAX - getBalance() - getBuyingLiabilities(lm);
    }
    else
    {
        return INT64_MAX;
    }
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

    int64_t effMinBalance = lm.getMinBalance(newEntriesCount);
    if (lm.getCurrentLedgerVersion() >= 10)
    {
        effMinBalance += getSellingLiabilities(lm);
    }

    // only check minBalance when attempting to add subEntries
    if (count > 0 && getBalance() < effMinBalance)
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
    Liabilities liabilities;
    soci::indicator inflationDestInd;
    soci::indicator buyingLiabilitiesInd, sellingLiabilitiesInd;

    AccountFrame::pointer res = make_shared<AccountFrame>(accountID);
    AccountEntry& account = res->getAccount();

    auto prep =
        db.getPreparedStatement("SELECT balance, seqnum, numsubentries, "
                                "inflationdest, homedomain, thresholds, "
                                "flags, lastmodified, buyingliabilities, "
                                "sellingliabilities "
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
    st.exchange(into(liabilities.buying, buyingLiabilitiesInd));
    st.exchange(into(liabilities.selling, sellingLiabilitiesInd));
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

    decoder::decode_b64(thresholds.begin(), thresholds.end(),
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

    assert(buyingLiabilitiesInd == sellingLiabilitiesInd);
    if (buyingLiabilitiesInd == soci::i_ok)
    {
        account.ext.v(1);
        account.ext.v1().liabilities = liabilities;
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

class accountsAccumulator : public EntryFrame::Accumulator
{
  public:
    accountsAccumulator(Database& db) : mDb(db)
    {
    }
    ~accountsAccumulator()
    {
        vector<string> insertUpdateAccountIDs;
        vector<int64> balances;
        vector<SequenceNumber> seqnums;
        vector<uint32> numsubentrieses;
        vector<string> inflationdests;
        vector<soci::indicator> inflationdestInds;
        vector<string> homedomains;
        vector<string> thresholdses;
        vector<uint32> flagses;
        vector<uint32> lastmodifieds;
        vector<int64> buyingliabilitieses;
        vector<soci::indicator> buyingliabilitiesInds;
        vector<int64> sellingliabilitieses;
        vector<soci::indicator> sellingliabilitiesInds;

        vector<string> signerReplaceAccountIDs;
        vector<string> signerAccountIDs;
        vector<string> signerPublicKeys;
        vector<int> signerWeights;

        vector<string> deleteAccountIds;

        for (auto& it : mItems)
        {
            if (!it.second)
            {
                deleteAccountIds.push_back(it.first);
                continue;
            }
            insertUpdateAccountIDs.push_back(it.first);
            balances.push_back(it.second->balance);
            seqnums.push_back(it.second->seqnum);
            numsubentrieses.push_back(it.second->numsubentries);
            inflationdests.push_back(it.second->inflationdest);
            inflationdestInds.push_back(it.second->inflationdestInd);
            homedomains.push_back(it.second->homedomain);
            thresholdses.push_back(it.second->thresholds);
            flagses.push_back(it.second->flags);
            lastmodifieds.push_back(it.second->lastmodified);
            buyingliabilitieses.push_back(it.second->buyingliabilities);
            buyingliabilitiesInds.push_back(it.second->buyingliabilitiesInd);
            sellingliabilitieses.push_back(it.second->sellingliabilities);
            sellingliabilitiesInds.push_back(it.second->sellingliabilitiesInd);

            if (it.second->signers && !it.second->signers->empty())
            {
                signerReplaceAccountIDs.push_back(it.first);
                for (auto& s : *(it.second->signers))
                {
                    signerAccountIDs.push_back(it.first);
                    signerPublicKeys.push_back(KeyUtils::toStrKey(s.key));
                    signerWeights.push_back(s.weight);
                }
            }
        }

        soci::session& session = mDb.getSession();
        auto pg = dynamic_cast<soci::postgresql_session_backend*>(session.get_backend());
        if (pg) {
          if (!insertUpdateAccountIDs.empty()) {
            static const char q[] = "WITH r AS ("
              "SELECT unnest($1::text[]) AS id, unnest($2::bigint[]) AS bal, unnest($3::bigint[]) as seq, "
              "unnest($4::integer[]) AS numsub, unnest($5::text[]) AS infl, unnest($6::text[]) AS home, unnest($7::text[]) AS thresh, "
              "unnest($8::integer[]) AS flags, unnest($9::integer[]) AS lastmod, unnest($10::integer[]) AS bl, unnest($11::integer[]) AS sl) "
              "INSERT INTO accounts "
              "(accountid, balance, seqnum, numsubentries, "
              "inflationdest, homedomain, thresholds, flags, "
              "lastmodified, buyingliabilities, sellingliabilities) "
              "SELECT id, bal, seq, numsub, infl, home, thresh, flags, lastmod, bl, sl FROM r "
              "ON CONFLICT (accountid) DO UPDATE "
              "SET balance = r.bal, seqnum = r.seq, numsubentries = r.numsub, "
              "inflationdest = r.infl, homedomain = r.home, thresholds = r.thresh, "
              "flags = r.flags, lastmodified = r.lastmod, "
              "buyingliabilities = r.buying, sellingliabilities = r.selling";
            // xxx marshal args
            PGresult* res = PQexecParams(pg->conn_, q, 11, 0, paramVals, 0, 0, 0);
            // xxx check res
          }
          if (!signerReplaceAccountIDs.empty()) {
            static const char q1[] = "DELETE FROM signers WHERE accountid = ANY($1::text[])";
            // xxx marshal args
            PGresult* res = PQexecParams(pg->conn_, q1, 1, 0, paramVals1, 0, 0, 0);
            // xxx check res

            static const char q2[] = "WITH r AS ("
              "SELECT unnest($1::text[]) AS id, unnest($2::text[]) AS pubkeys, unnest($3::integer[]) AS weights) "
              "INSERT INTO signers (accountid, publickey, weight) "
              "SELECT id, pubkeys, weights FROM r";
            // xxx marshal args
            res = PQexecParams(pg->conn_, q2, 3, 0, paramVals2, 0, 0, 0);
            // xxx check res
          }
          if (!deleteAccountIds.empty()) {
            static const char q1[] = "DELETE FROM accounts WHERE accountid = ANY($1::text[])";
            // xxx marshal args
            PGresult* res = PQexecParams(pg->conn_, q1, 1, 0, paramVals, 0, 0, 0);
            // xxx check res

            static const char q2[] = "DELETE FROM signers WHERE accountid = ANY($1::text[])";
            PGresult* res = PQexecParams(pg->conn_, q2, 1, 0, paramVals, 0, 0, 0); // sic - uses same paramVals
            // xxx check res
          }

          return;
        }

        if (!insertUpdateAccountIDs.empty())
        {
            soci::statement st =
                session.prepare
                << "INSERT INTO accounts "
                << "(accountid, balance, seqnum, numsubentries, "
                << "inflationdest, homedomain, thresholds, flags, "
                << "lastmodified, buyingliabilities, sellingliabilities) "
                << "VALUES (:id, :bal, :seq, :numsub, :infl, :home, :thresh, "
                << ":flags, :lastmod, :buying, :selling) "
                << "ON CONFLICT (accountid) DO UPDATE "
                << "SET balance = :bal, seqnum = :seq, numsubentries = "
                   ":numsub, "
                << "inflationdest = :infl, homedomain = :home, thresholds = "
                   ":thresh, "
                << "flags = :flags, lastmodified = :lastmod, "
                << "buyingliabilities = :buying, sellingliabilities = :selling";

            st.exchange(use(insertUpdateAccountIDs, "id"));
            st.exchange(use(balances, "bal"));
            st.exchange(use(seqnums, "seq"));
            st.exchange(use(numsubentrieses, "numsub"));
            st.exchange(use(inflationdests, inflationdestInds, "infl"));
            st.exchange(use(homedomains, "home"));
            st.exchange(use(thresholdses, "thresh"));
            st.exchange(use(flagses, "flags"));
            st.exchange(use(lastmodifieds, "lastmod"));
            st.exchange(
                use(buyingliabilitieses, buyingliabilitiesInds, "buying"));
            st.exchange(
                use(sellingliabilitieses, sellingliabilitiesInds, "selling"));

            st.define_and_bind();

            try
            {
                st.execute(true); // xxx timer
            }
            catch (soci::soci_error& e)
            {
                cout << "xxx inserting into accounts: " << e.what() << endl;
                throw;
            }
        }

        if (!signerReplaceAccountIDs.empty())
        {
            try
            {
                session << "DELETE FROM signers WHERE accountid = :id",
                    use(signerReplaceAccountIDs, "id");
            }
            catch (const soci::soci_error& e)
            {
                cout << "xxx deleting/inserting from signers [1]: " << e.what()
                     << endl;
                throw;
            }
            try
            {
                session << "INSERT INTO signers (accountid, publickey, weight) "
                           "VALUES (:id, :key, :weight)",
                    use(signerAccountIDs, "id"), use(signerPublicKeys, "key"),
                    use(signerWeights, "weight");
            }
            catch (const soci::soci_error& e)
            {
                cout << "xxx deleting/inserting from signers [2]: " << e.what()
                     << endl;
            }
        }

        if (!deleteAccountIds.empty())
        {
            try
            {
                session << "DELETE FROM accounts WHERE accountid = :id",
                    use(deleteAccountIds, "id");
            }
            catch (soci::soci_error& e)
            {
                cout << "xxx deleting from accounts: " << e.what() << endl;
            }
            try
            {
                session << "DELETE FROM signers WHERE accountid = :id",
                    use(deleteAccountIds, "id");
            }
            catch (soci::soci_error& e)
            {
                cout << "xxx deleting from signers: " << e.what() << endl;
            }
        }
    }

  protected:
    friend AccountFrame;

    Database& mDb;
    struct valType
    {
        int64 balance;
        SequenceNumber seqnum;
        uint32 numsubentries;
        string inflationdest;
        soci::indicator inflationdestInd;
        string homedomain;
        string thresholds;
        uint32 flags;
        uint32 lastmodified;
        int64 buyingliabilities;
        soci::indicator buyingliabilitiesInd;
        int64 sellingliabilities;
        soci::indicator sellingliabilitiesInd;
        unique_ptr<xdr::xvector<Signer, 20>> signers;
    };
    map<string, unique_ptr<valType>> mItems;
};

unique_ptr<EntryFrame::Accumulator>
AccountFrame::createAccumulator(Database& db)
{
    return unique_ptr<EntryFrame::Accumulator>(new accountsAccumulator(db));
}

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db,
                          EntryFrame::AccumulatorGroup* accums) const
{
    storeDelete(delta, db, getKey(), accums);
}

void
AccountFrame::storeDelete(LedgerDelta& delta, Database& db,
                          LedgerKey const& key,
                          EntryFrame::AccumulatorGroup* accums)
{
    LedgerDelta::EntryDeleter entryDeleter(delta, key);

    flushCachedEntry(key, db);

    std::string actIDStrKey = KeyUtils::toStrKey(key.account().accountID);

    if (accums)
    {
        accountsAccumulator* accountsAccum =
            dynamic_cast<accountsAccumulator*>(accums->accountsAccum());
        accountsAccum->mItems[actIDStrKey] =
            unique_ptr<accountsAccumulator::valType>();
        return;
    }

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

void
AccountFrame::storeAddOrChange(LedgerDelta& delta, Database& db,
                               EntryFrame::AccumulatorGroup* accums)
{
    LedgerDelta::EntryModder(delta, *this);

    touch(delta);

    flushCachedEntry(db);

    std::string actIDStrKey = KeyUtils::toStrKey(mAccountEntry.accountID);

    soci::indicator inflation_ind = soci::i_null;
    string inflationDestStrKey;

    if (mAccountEntry.inflationDest)
    {
        inflationDestStrKey = KeyUtils::toStrKey(*mAccountEntry.inflationDest);
        inflation_ind = soci::i_ok;
    }

    string thresholds(decoder::encode_b64(mAccountEntry.thresholds));

    Liabilities liabilities;
    soci::indicator liabilitiesInd = soci::i_null;
    if (mAccountEntry.ext.v() == 1)
    {
        liabilities = mAccountEntry.ext.v1().liabilities;
        liabilitiesInd = soci::i_ok;
    }

    if (accums)
    {
        auto val = make_unique<accountsAccumulator::valType>();
        val->balance = mAccountEntry.balance;
        val->seqnum = mAccountEntry.seqNum;
        val->numsubentries = mAccountEntry.numSubEntries;
        val->inflationdest = inflationDestStrKey;
        val->inflationdestInd = inflation_ind;
        val->homedomain = mAccountEntry.homeDomain;
        val->thresholds = thresholds;
        val->flags = mAccountEntry.flags;
        val->lastmodified = getLastModified();
        val->buyingliabilities = liabilities.buying;
        val->buyingliabilitiesInd = liabilitiesInd;
        val->sellingliabilities = liabilities.selling;
        val->sellingliabilitiesInd = liabilitiesInd;
        if (mUpdateSigners)
        {
            val->signers = make_unique<xdr::xvector<Signer, 20>>();
            for (auto& s : mAccountEntry.signers)
            {
                val->signers->push_back(s);
            }
        }

        accountsAccumulator* accountsAccum =
            dynamic_cast<accountsAccumulator*>(accums->accountsAccum());
        accountsAccum->mItems[actIDStrKey] = move(val);
        return;
    }

    string sql =
        "INSERT INTO accounts (accountid, balance, seqnum, "
        "numsubentries, inflationdest, homedomain, thresholds, flags, "
        "lastmodified, buyingliabilities, sellingliabilities ) "
        "VALUES (:id, :bal, :seq, :numsub, :infl, :home, :thresh, :flags, "
        ":lastmod, :bl, :sl) "
        "ON CONFLICT (accountid) DO UPDATE "
        "SET balance = :bal, seqnum = :seq, "
        "numsubentries = :numsub, inflationdest = :infl, homedomain = :home, "
        "thresholds = :thresh, flags = :flags, "
        "lastmodified = :lastmod, buyingliabilities = :bl, sellingliabilities "
        "= :sl";
    auto prep = db.getPreparedStatement(sql);

    {
        soci::statement& st = prep.statement();
        st.exchange(use(actIDStrKey, "id"));
        st.exchange(use(mAccountEntry.balance, "bal"));
        st.exchange(use(mAccountEntry.seqNum, "seq"));
        st.exchange(use(mAccountEntry.numSubEntries, "numsub"));
        st.exchange(use(inflationDestStrKey, inflation_ind, "infl"));
        string homeDomain(mAccountEntry.homeDomain);
        st.exchange(use(homeDomain, "home"));
        st.exchange(use(thresholds, "thresh"));
        st.exchange(use(mAccountEntry.flags, "flags"));
        st.exchange(use(getLastModified(), "lastmod"));
        st.exchange(use(liabilities.buying, liabilitiesInd, "bl"));
        st.exchange(use(liabilities.selling, liabilitiesInd, "sl"));
        st.define_and_bind();
        {
            auto timer = db.getInsertTimer("account"); // xxx update timer?
            st.execute(true);
        }

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }
    }

    if (mUpdateSigners)
    {
        applySigners(db);
    }
}

void
AccountFrame::applySigners(Database& db)
{
    std::string actIDStrKey = KeyUtils::toStrKey(mAccountEntry.accountID);

    // generates a diff with the signers stored in the database

    // first, load the signers stored in the database for this account (if any)
    std::vector<Signer> signers = loadSigners(db, actIDStrKey);

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
