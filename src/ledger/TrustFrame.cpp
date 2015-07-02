// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
using xdr::operator==;

const char* TrustFrame::kSQLCreateStatement1 =
    "CREATE TABLE trustlines"
    "("
    "accountid     VARCHAR(51)     NOT NULL,"
    "issuer        VARCHAR(51)     NOT NULL,"
    "alphanumcurrency   VARCHAR(4) NOT NULL,"
    "tlimit        BIGINT          NOT NULL DEFAULT 0 CHECK (tlimit >= 0),"
    "balance       BIGINT          NOT NULL DEFAULT 0 CHECK (balance >= 0),"
    "flags         INT             NOT NULL,"
    "PRIMARY KEY (accountid, issuer, alphanumcurrency)"
    ");";

const char* TrustFrame::kSQLCreateStatement2 =
    "CREATE INDEX accountlines ON trustlines (accountid);";

TrustFrame::TrustFrame()
    : EntryFrame(TRUSTLINE), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
}

TrustFrame::TrustFrame(LedgerEntry const& from)
    : EntryFrame(from), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
    assert(isValid());
}

TrustFrame::TrustFrame(TrustFrame const& from) : TrustFrame(from.mEntry)
{
}

TrustFrame& TrustFrame::operator=(TrustFrame const& other)
{
    if (&other != this)
    {
        mTrustLine = other.mTrustLine;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
        mIsIssuer = other.mIsIssuer;
    }
    return *this;
}

void
TrustFrame::getKeyFields(LedgerKey const& key, std::string& base58AccountID,
                         std::string& base58Issuer, std::string& currencyCode)
{
    base58AccountID = PubKeyUtils::toBase58(key.trustLine().accountID);
    base58Issuer =
        PubKeyUtils::toBase58(key.trustLine().currency.alphaNum().issuer);
    if (base58AccountID == base58Issuer)
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");
    currencyCodeToStr(key.trustLine().currency.alphaNum().currencyCode,
                      currencyCode);
}

int64_t
TrustFrame::getBalance() const
{
    assert(isValid());
    return mTrustLine.balance;
}

bool
TrustFrame::isAuthorized() const
{
    return (mTrustLine.flags & AUTHORIZED_FLAG) != 0;
}

void
TrustFrame::setAuthorized(bool authorized)
{
    if (authorized)
    {
        mTrustLine.flags |= AUTHORIZED_FLAG;
    }
    else
    {
        mTrustLine.flags &= ~AUTHORIZED_FLAG;
    }
}

bool
TrustFrame::addBalance(int64_t delta)
{
    if (mIsIssuer)
    {
        return true;
    }
    if (delta == 0)
    {
        return true;
    }
    if (!isAuthorized())
    {
        return false;
    }
    if (mTrustLine.limit < delta + mTrustLine.balance)
    {
        return false;
    }
    if ((delta + mTrustLine.balance) < 0)
    {
        return false;
    }
    mTrustLine.balance += delta;
    return true;
}

int64_t
TrustFrame::getMaxAmountReceive() const
{
    int64_t amount = 0;
    if (mIsIssuer)
    {
        amount = INT64_MAX;
    }
    else if (isAuthorized())
    {
        amount = mTrustLine.limit - mTrustLine.balance;
    }
    return amount;
}

bool
TrustFrame::isValid() const
{
    TrustLineEntry const& tl = mTrustLine;
    bool res = tl.currency.type() != CURRENCY_TYPE_NATIVE;
    res = res && (tl.balance >= 0);
    res = res && (tl.balance <= tl.limit);
    return res;
}

bool
TrustFrame::exists(Database& db, LedgerKey const& key)
{
    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(key, b58AccountID, b58Issuer, currencyCode);
    int exists = 0;
    auto timer = db.getSelectTimer("trust-exists");
    db.getSession()
        << "SELECT EXISTS (SELECT NULL FROM trustlines "
           "WHERE accountid=:v1 and issuer=:v2 and alphanumcurrency=:v3)",
        use(b58AccountID), use(b58Issuer), use(currencyCode), into(exists);
    return exists != 0;
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(key, b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getDeleteTimer("trust");
    db.getSession()
        << "DELETE FROM trustlines "
           "WHERE accountid=:v1 AND issuer=:v2 AND alphanumcurrency=:v3",
        use(b58AccountID), use(b58Issuer), use(currencyCode);

    delta.deleteEntry(key);
}

void
TrustFrame::storeChange(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    if (mIsIssuer)
        return;

    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(getKey(), b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getUpdateTimer("trust");
    statement st =
        (db.getSession().prepare
             << "UPDATE trustlines "
                "SET balance=:b, tlimit=:tl, flags=:a "
                "WHERE accountid=:v1 AND issuer=:v2 AND alphanumcurrency=:v3",
         use(mTrustLine.balance), use(mTrustLine.limit),
         use((int)mTrustLine.flags), use(b58AccountID), use(b58Issuer),
         use(currencyCode));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.modEntry(*this);
}

void
TrustFrame::storeAdd(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    if (mIsIssuer)
        return;

    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(getKey(), b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getInsertTimer("trust");
    statement st =
        (db.getSession().prepare << "INSERT INTO trustlines (accountid, "
                                    "issuer, alphanumcurrency, tlimit, flags) "
                                    "VALUES (:v1,:v2,:v3,:v4,:v5)",
         use(b58AccountID), use(b58Issuer), use(currencyCode),
         use(mTrustLine.limit), use((int)mTrustLine.flags));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.addEntry(*this);
}

static const char* trustLineColumnSelector =
    "SELECT accountid, issuer, alphanumcurrency, tlimit,balance,flags FROM "
    "trustlines";

TrustFrame::pointer
TrustFrame::createIssuerFrame(Currency const& issuer)
{
    pointer res = make_shared<TrustFrame>();
    res->mIsIssuer = true;
    TrustLineEntry& tl = res->mEntry.trustLine();
    tl.accountID = issuer.alphaNum().issuer;
    tl.flags |= AUTHORIZED_FLAG;
    tl.balance = INT64_MAX;
    tl.currency = issuer;
    tl.limit = INT64_MAX;
    return res;
}

TrustFrame::pointer
TrustFrame::loadTrustLine(AccountID const& accountID, Currency const& currency,
                          Database& db)
{
    if (accountID == currency.alphaNum().issuer)
    {
        return createIssuerFrame(currency);
    }

    std::string accStr, issuerStr, currencyStr;

    accStr = PubKeyUtils::toBase58(accountID);
    currencyCodeToStr(currency.alphaNum().currencyCode, currencyStr);
    issuerStr = PubKeyUtils::toBase58(currency.alphaNum().issuer);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << trustLineColumnSelector
                         << " WHERE accountid=:id AND "
                            "issuer=:issuer AND alphanumcurrency=:currency",
         use(accStr), use(issuerStr), use(currencyStr));

    pointer retLine;
    auto timer = db.getSelectTimer("trust");
    loadLines(sql, [&retLine](LedgerEntry const& trust)
              {
                  retLine = make_shared<TrustFrame>(trust);
              });
    return retLine;
}

bool
TrustFrame::hasIssued(AccountID const& issuerID, Database& db)
{
    std::string accStr;
    accStr = PubKeyUtils::toBase58(issuerID);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << "SELECT balance FROM trustlines WHERE issuer=:id "
                            "AND balance>0 LIMIT 1",
         use(accStr));

    auto timer = db.getSelectTimer("trust");
    int balance = 0;
    statement st = (sql, into(balance));
    st.execute(true);
    if (st.got_data())
    {
        return true;
    }
    return false;
}

void
TrustFrame::loadLines(details::prepare_temp_type& prep,
                      std::function<void(LedgerEntry const&)> trustProcessor)
{
    string accountID;
    std::string issuer, currency;

    LedgerEntry le;
    le.type(TRUSTLINE);

    TrustLineEntry& tl = le.trustLine();

    statement st = (prep, into(accountID), into(issuer), into(currency),
                    into(tl.limit), into(tl.balance), into(tl.flags));

    st.execute(true);
    while (st.got_data())
    {
        tl.accountID = PubKeyUtils::fromBase58(accountID);
        tl.currency.type(CURRENCY_TYPE_ALPHANUM);
        tl.currency.alphaNum().issuer = PubKeyUtils::fromBase58(issuer);
        strToCurrencyCode(tl.currency.alphaNum().currencyCode, currency);

        trustProcessor(le);

        st.fetch();
    }
}

void
TrustFrame::loadLines(AccountID const& accountID,
                      std::vector<TrustFrame::pointer>& retLines, Database& db)
{
    std::string accStr;
    accStr = PubKeyUtils::toBase58(accountID);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << trustLineColumnSelector << " WHERE accountid=:id",
         use(accStr));

    auto timer = db.getSelectTimer("trust");
    loadLines(sql, [&retLines](LedgerEntry const& cur)
              {
                  retLines.emplace_back(make_shared<TrustFrame>(cur));
              });
}

void
TrustFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS trustlines;";
    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
}
}
