// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar {
    const char *TrustFrame::kSQLCreateStatement =
        "CREATE TABLE IF NOT EXISTS TrustLines              \
         (                                                  \
         trustIndex    CHARACTER(64)  PRIMARY KEY,          \
         accountID     CHARACTER(64)  NOT NULL,             \
         issuer        CHARACTER(64)  NOT NULL,             \
         isoCurrency   CHARACTER(4)   NOT NULL,             \
         tlimit        BIGINT         NOT NULL DEFAULT 0    \
                                      CHECK (tlimit >= 0),  \
         balance       BIGINT         NOT NULL DEFAULT 0    \
                                      CHECK (balance >= 0), \
         authorized    BOOL           NOT NULL              \
         );";

    TrustFrame::TrustFrame()
    {
        mEntry.type(TRUSTLINE);
    }

    TrustFrame::TrustFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    
    void TrustFrame::calculateIndex()
    {
        // hash of accountID+issuer+currency
        SHA256 hasher;
        hasher.add(mEntry.trustLine().accountID);
        hasher.add(mEntry.trustLine().currency.isoCI().issuer);
        hasher.add(mEntry.trustLine().currency.isoCI().currencyCode);
        mIndex = hasher.finish();
    }

    int64_t TrustFrame::getBalance()
    {
        assert(isValid());
        return mEntry.trustLine().balance;
    }

    bool TrustFrame::isValid() const
    {
        const TrustLineEntry &tl = mEntry.trustLine();
        bool res = tl.currency.type() != NATIVE;
        res = res && (tl.balance >= 0);
        res = res && (tl.balance <= tl.limit);
        return res;
    }

    void TrustFrame::storeDelete(LedgerDelta &delta, Database& db)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        db.getSession() <<
            "DELETE from TrustLines where trustIndex= :v1", use(base58ID);

        delta.deleteEntry(*this);
    }

    void TrustFrame::storeChange(LedgerDelta &delta, Database& db)
    {
        assert(isValid());

        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        statement st = (db.getSession().prepare <<
            "UPDATE TrustLines set balance=:b, tlimit=:tl, "\
            "authorized=:a WHERE trustIndex=:in",
            use(mEntry.trustLine().balance), use(mEntry.trustLine().limit),
            use((int)mEntry.trustLine().authorized), use(base58ID));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }

        delta.modEntry(*this);
    }

    void TrustFrame::storeAdd(LedgerDelta &delta, Database& db)
    {
        assert(isValid());

        std::string base58Index = toBase58Check(VER_ACCOUNT_ID, getIndex());
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().accountID);
        std::string b58Issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().currency.isoCI().issuer);
        std::string currencyCode;
        currencyCodeToStr(mEntry.trustLine().currency.isoCI().currencyCode,currencyCode);

        statement st = (db.getSession().prepare <<
            "INSERT into TrustLines (trustIndex,accountID,issuer,isoCurrency,tlimit,authorized) values (:v1,:v2,:v3,:v4,:v5,:v6)",
            use(base58Index), use(b58AccountID), use(b58Issuer),
            use(currencyCode), use(mEntry.trustLine().limit),
            use((int)mEntry.trustLine().authorized));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }

        delta.addEntry(*this);
    }

    static const char *trustLineColumnSelector = "SELECT accountID, issuer, isoCurrency, tlimit,balance,authorized FROM TrustLines";

    bool TrustFrame::loadTrustLine(const uint256& accountID,
        const Currency& currency,
        TrustFrame& retLine, Database& db)
    {
        std::string accStr, issuerStr, currencyStr;

        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
        currencyCodeToStr(currency.isoCI().currencyCode, currencyStr);
        issuerStr = toBase58Check(VER_ACCOUNT_ID, currency.isoCI().issuer);

        session &session = db.getSession();

        details::prepare_temp_type sql = (session.prepare <<
            trustLineColumnSelector << " WHERE accountID=:id AND "\
            "issuer=:issuer AND isoCurrency=:currency",
            use(accStr), use(issuerStr), use(currencyStr));

        bool res = false;

        loadLines(sql, [&retLine, &res](TrustFrame const &trust)
        {
            retLine = trust;
            res = true;
        });
        return res;
    }

    void TrustFrame::loadLines(details::prepare_temp_type &prep,
        std::function<void(const TrustFrame&)> trustProcessor)
    {
        string accountID;
        std::string issuer, currency;
        int authorized;

        TrustFrame curTrustLine;

        TrustLineEntry &tl = curTrustLine.mEntry.trustLine();

        statement st = (prep,
            into(accountID), into(issuer), into(currency), into(tl.limit),
            into(tl.balance), into(authorized)
            );

        st.execute(true);
        while (st.got_data())
        {
            tl.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
            tl.currency.type(ISO4217);
            tl.currency.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, issuer);
            strToCurrencyCode(tl.currency.isoCI().currencyCode, currency);
            tl.authorized = authorized;
            trustProcessor(curTrustLine);

            st.fetch();
        }
    }

    void TrustFrame::loadLines(const uint256& accountID,
        std::vector<TrustFrame>& retLines, Database& db)
    {
        std::string accStr;
        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

        session &session = db.getSession();

        details::prepare_temp_type sql = (session.prepare <<
            trustLineColumnSelector << " WHERE accountID=:id",
            use(accStr));

        loadLines(sql, [&retLines](TrustFrame const &cur)
        {
            retLines.push_back(cur);
        });
    }


    void TrustFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS TrustLines;";
        db.getSession() << kSQLCreateStatement;
    }
}
