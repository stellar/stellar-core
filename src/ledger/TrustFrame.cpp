// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerMaster.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "database/Database.h"
#include "LedgerDelta.h"

using namespace std;

namespace stellar {
    const char *TrustFrame::kSQLCreateStatement = "CREATE TABLE IF NOT EXISTS TrustLines (					\
		trustIndex CHARACTER(35) PRIMARY KEY,				\
		accountID	CHARACTER(35),			\
		issuer CHARACTER(35),				\
		isoCurrency CHARACTER(4),    		\
		tlimit BIGINT UNSIGNED default 0,		   		\
		balance BIGINT UNSIGNED default 0,			\
		authorized BOOL						\
	); ";

    TrustFrame::TrustFrame()
    {

    }
    TrustFrame::TrustFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    
    void TrustFrame::calculateIndex()
    {
        // hash of accountID+issuer+currency
        SHA512_256 hasher;
        hasher.add(mEntry.trustLine().accountID);
        hasher.add(mEntry.trustLine().currency.isoCI().issuer);
        hasher.add(mEntry.trustLine().currency.isoCI().currencyCode);
        mIndex = hasher.finish();
    }

    int64_t TrustFrame::getBalance()
    {
        return mEntry.trustLine().balance;
    }

    void TrustFrame::storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        ledgerMaster.getDatabase().getSession() <<
            "DELETE from TrustLines where trustIndex= :v1", soci::use(base58ID);

        delta.deleteEntry(*this);
    }

    void TrustFrame::storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
            "UPDATE TrustLines set balance=:b, tlimit=:tl, "\
            "authorized=:a WHERE trustIndex=:in",
            soci::use(mEntry.trustLine().balance), soci::use(mEntry.trustLine().limit),
            soci::use((int)mEntry.trustLine().authorized), soci::use(base58ID));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }

        delta.modEntry(*this);
    }

    void TrustFrame::storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string base58Index = toBase58Check(VER_ACCOUNT_ID, getIndex());
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().accountID);
        std::string b58Issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.trustLine().currency.isoCI().issuer);
        std::string currencyCode;
        currencyCodeToStr(mEntry.trustLine().currency.isoCI().currencyCode,currencyCode);

        soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
            "INSERT into TrustLines (trustIndex,accountID,issuer,isoCurrency,tlimit,authorized) values (:v1,:v2,:v3,:v4,:v5,:v6)",
            soci::use(base58Index), soci::use(b58AccountID), soci::use(b58Issuer),
            soci::use(currencyCode), soci::use(mEntry.trustLine().limit),
            soci::use((int)mEntry.trustLine().authorized));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("Could not update data in SQL");
        }

        delta.addEntry(*this);
    }

    void TrustFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS TrustLines;";
        db.getSession() << kSQLCreateStatement;
    }
}
