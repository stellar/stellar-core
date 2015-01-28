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
            "CREATE TABLE IF NOT EXISTS Offers (    \
            accountID       CHARACTER(35) NOT NULL, \
            sequence        INT UNSIGNED NOT NULL,  \
            paysIsoCurrency CHARACTER(4),           \
            paysIssuer CHARACTER(35),               \
            getsIsoCurrency CHARACTER(4),           \
            getsIssuer CHARACTER(35),               \
            amount BIGINT NOT NULL,                 \
            price BIGINT NOT NULL,                  \
            flags INT NOT NULL,                     \
            PRIMARY KEY (accountID, sequence)       \
    );";

    OfferFrame::OfferFrame()
    {
        mEntry.type(OFFER);
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
        SHA512_256 hasher;
        hasher.add(mEntry.offer().accountID);
        // TODO: fix this (endian), or remove index altogether
        hasher.add(ByteSlice(&mEntry.offer().sequence, sizeof(mEntry.offer().sequence)));
        mIndex = hasher.finish();
    }

    int64_t OfferFrame::getPrice() const
    {
        return mEntry.offer().price;
    }
    
    int64_t OfferFrame::getAmount() const
    {
        return mEntry.offer().amount;
    }

    uint256 const& OfferFrame::getAccountID() const
    {
        return mEntry.offer().accountID;
    }

    Currency& OfferFrame::getTakerPays()
    {
        return mEntry.offer().takerPays;
    }
    Currency& OfferFrame::getTakerGets()
    {
        return mEntry.offer().takerGets;
    }

    uint32 OfferFrame::getSequence()
    {
        return mEntry.offer().sequence;
    }
    

    void OfferFrame::storeDelete(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        ledgerMaster.getDatabase().getSession() <<
            "DELETE FROM Offers WHERE accountID=:id AND sequence=:s",
            use(b58AccountID), use(mEntry.offer().sequence);

        delta.deleteEntry(*this);
    }

    void OfferFrame::storeChange(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        std::stringstream sql;
        sql << "UPDATE Offers set ";

        soci::statement st = (ledgerMaster.getDatabase().getSession().prepare <<
            "UPDATE Offers SET amount=:a, price=:p WHERE accountID=:id AND sequence=:s",
            use(mEntry.offer().amount), use(mEntry.offer().price),
            use(b58AccountID), use(mEntry.offer().sequence));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.modEntry(*this);
    }

    void OfferFrame::storeAdd(LedgerDelta &delta, LedgerMaster& ledgerMaster)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        soci::statement st(ledgerMaster.getDatabase().getSession().prepare << "select 1");

        if(mEntry.offer().takerGets.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode, currencyCode);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (accountID,sequence,paysIsoCurrency,paysIssuer,amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7)",
                use(b58AccountID), use(mEntry.offer().sequence), 
                use(b58issuer),use(currencyCode),use(mEntry.offer().amount),
                use(mEntry.offer().price),use(mEntry.offer().flags));
            st.execute(true);
        }
        else if(mEntry.offer().takerPays.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode, currencyCode);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (accountID,sequence,getsIsoCurrency,getsIssuer,amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(b58issuer), use(currencyCode), use(mEntry.offer().amount),
                use(mEntry.offer().price), use(mEntry.offer().flags));
            st.execute(true);
        }
        else
        {
            std::string b58PaysIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string paysIsoCurrency, getsIsoCurrency;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode, paysIsoCurrency);
            std::string b58GetsIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode, getsIsoCurrency);
            st = (ledgerMaster.getDatabase().getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "paysIsoCurrency,paysIssuer,getsIsoCurrency,getsIssuer,"\
                "amount,price,flags) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(paysIsoCurrency), use(b58PaysIssuer), use(getsIsoCurrency), use(b58GetsIssuer),
                use(mEntry.offer().amount), use(mEntry.offer().price), use(mEntry.offer().flags));
            st.execute(true);
        }

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
