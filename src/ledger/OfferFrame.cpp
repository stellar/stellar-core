// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "ledger/LedgerMaster.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "LedgerDelta.h"

using namespace std;
using namespace soci;

namespace stellar
{
    const char *OfferFrame::kSQLCreateStatement = 
            "CREATE TABLE IF NOT EXISTS Offers (	\
			offerIndex CHARACTER(35) PRIMARY KEY,   \
            accountID		CHARACTER(35),		    \
			sequence		INT UNSIGNED,		    \
			paysIsoCurrency CHARACTER(4),		    \
			paysIssuer CHARACTER(35),		        \
			getsIsoCurrency CHARACTER(4),			\
			getsIssuer CHARACTER(35),		        \
			amount BIGINT UNSIGNED,             	\
			price BIGINT UNSIGNED,	                \
			flags INT UNSIGNED					    \
	);";

    OfferFrame::OfferFrame()
    {

    }
    OfferFrame::OfferFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    void OfferFrame::from(const Transaction& tx) 
    {
        mEntry.type(OFFER);
        mEntry.offer().accountID = tx.account;
        mEntry.offer().amount = tx.body.createOfferTx().amount;
        mEntry.offer().price = tx.body.createOfferTx().price;
        mEntry.offer().sequence = tx.body.createOfferTx().sequence;
        mEntry.offer().takerGets = tx.body.createOfferTx().takerGets;
        mEntry.offer().takerPays = tx.body.createOfferTx().takerPays;
        mEntry.offer().flags = tx.body.createOfferTx().flags;
    }

    void OfferFrame::calculateIndex()
    {
        // hash of accountID+offerSeq
        SHA512_256 hasher;
        hasher.add(mEntry.offer().accountID);
        // TODO.2 hasher.add(mEntry.offer().sequence);
        mIndex = hasher.finish();
    }

    int64_t OfferFrame::getPrice()
    {
        return mEntry.offer().price;
    }
    int64_t OfferFrame::getAmount()
    {
        return mEntry.offer().amount;
    }
    Currency& OfferFrame::getTakerPays()
    {
        return mEntry.offer().takerPays;
    }
    Currency& OfferFrame::getTakerGets()
    {
        return mEntry.offer().takerGets;
    }

    

    void OfferFrame::storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        ledgerMaster.getDatabase().getSession() <<
            "DELETE from Offers where offerIndex= :v1", soci::use(base58ID);

        delta.deleteEntry(*this);
    }

    void OfferFrame::storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string base58ID = toBase58Check(VER_ACCOUNT_ID, getIndex());

        std::stringstream sql;
        sql << "UPDATE Offers set ";

        soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
            "UPDATE Offers set amount=:a, price=:p where offerIndex=:i",
            use(mEntry.offer().amount), use(mEntry.offer().price), use(base58ID));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.modEntry(*this);
    }

    void OfferFrame::storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string b58Index = toBase58Check(VER_ACCOUNT_ID, getIndex());
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        soci::statement st(ledgerMaster.getDatabase().getSession().prepare << "DUMMY");

        if(mEntry.offer().takerGets.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode, currencyCode);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (offerIndex,accountID,sequence,paysCurrency,paysIssuer,amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
                use(b58Index), use(b58AccountID), use(mEntry.offer().sequence), 
                use(b58issuer),use(currencyCode),use(mEntry.offer().amount),
                use(mEntry.offer().price),use(mEntry.offer().flags));
        }
        else if(mEntry.offer().takerPays.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode, currencyCode);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (offerIndex,accountID,sequence,getsCurrency,getsIssuer,amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8)",
                use(b58Index), use(b58AccountID), use(mEntry.offer().sequence),
                use(b58issuer), use(currencyCode), use(mEntry.offer().amount),
                use(mEntry.offer().price), use(mEntry.offer().flags));
        }
        else
        {
            std::string b58PaysIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string paysCurrency, getsCurrency;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode,paysCurrency);
            std::string b58GetsIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode,getsCurrency);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (offerIndex,accountID,sequence,paysCurrency,paysIssuer,getsCurrency,getsIssuer,amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10)",
                use(b58Index), use(b58AccountID), use(mEntry.offer().sequence),
                use(b58PaysIssuer), use(paysCurrency), use(b58GetsIssuer), use(getsCurrency),
                use(mEntry.offer().amount),
                use(mEntry.offer().price), use(mEntry.offer().flags));
        }

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.addEntry(*this);
    }

    void OfferFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS Offers;";
        db.getSession() << kSQLCreateStatement;
    }
}
