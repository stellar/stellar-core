// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
    const char *OfferFrame::kSQLCreateStatement = 
        "CREATE TABLE IF NOT EXISTS Offers                     \
         (                                                     \
         accountID       CHARACTER(64)  NOT NULL,              \
         sequence        INT            NOT NULL               \
                                        CHECK (sequence >= 0), \
         paysIsoCurrency CHARACTER(4)   NOT NULL,              \
         paysIssuer      CHARACTER(64)  NOT NULL,              \
         getsIsoCurrency CHARACTER(4)   NOT NULL,              \
         getsIssuer      CHARACTER(64)  NOT NULL,              \
         amount          BIGINT         NOT NULL,              \
         priceN          INT            NOT NULL,              \
         priceD          INT            NOT NULL,              \
         flags           INT            NOT NULL,              \
         price           BIGINT         NOT NULL,              \
         PRIMARY KEY (accountID, sequence)                     \
         );";

    OfferFrame::OfferFrame() : EntryFrame(OFFER), mOffer(mEntry.offer())
    {
    }

    OfferFrame::OfferFrame(LedgerEntry const& from) : EntryFrame(from), mOffer(mEntry.offer())
    {
    }

    OfferFrame::OfferFrame(OfferFrame const& from) : OfferFrame(from.mEntry)
    {
    }

    OfferFrame& OfferFrame::operator=(OfferFrame const& other)
    {
        if (&other != this)
        {
            mOffer = other.mOffer;
            mKey = other.mKey;
            mKeyCalculated = other.mKeyCalculated;
        }
        return *this;
    }

    void OfferFrame::from(const Transaction& tx) 
    {
        assert(mEntry.type() == OFFER);
        mOffer.accountID = tx.account;
        mOffer.amount = tx.body.createOfferTx().amount;
        mOffer.price = tx.body.createOfferTx().price;
        mOffer.sequence = tx.body.createOfferTx().sequence;
        mOffer.takerGets = tx.body.createOfferTx().takerGets;
        mOffer.takerPays = tx.body.createOfferTx().takerPays;
        mOffer.flags = tx.body.createOfferTx().flags;
        mKeyCalculated = false;
    }

    Price OfferFrame::getPrice() const
    {
        return mOffer.price;
    }
    
    int64_t OfferFrame::getAmount() const
    {
        return mOffer.amount;
    }

    uint256 const& OfferFrame::getAccountID() const
    {
        return mOffer.accountID;
    }

    Currency& OfferFrame::getTakerPays()
    {
        return mOffer.takerPays;
    }
    Currency& OfferFrame::getTakerGets()
    {
        return mOffer.takerGets;
    }

    uint32 OfferFrame::getSequence()
    {
        return mOffer.sequence;
    }
    

    // TODO: move this and related SQL code to OfferFrame
    static const char *offerColumnSelector =
        "SELECT accountID,sequence,paysIsoCurrency,paysIssuer,"\
        "getsIsoCurrency,getsIssuer,amount,priceN,priceD,flags FROM Offers";

    bool OfferFrame::loadOffer(const uint256& accountID, uint32_t seq,
        OfferFrame& retOffer, Database& db)
    {
        std::string accStr;
        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

        soci::session &session = db.getSession();

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector << " where accountID=:id and sequence=:seq",
            use(accStr), use(seq));

        bool res = false;

        loadOffers(sql, [&retOffer, &res](OfferFrame const& offer) {
            retOffer = offer;
            res = true;
        });

        return res;
    }


    void OfferFrame::loadOffers(soci::details::prepare_temp_type &prep,
        std::function<void(OfferFrame const&)> offerProcessor)
    {
        string accountID;
        std::string paysIsoCurrency, getsIsoCurrency, paysIssuer, getsIssuer;

        soci::indicator paysIsoIndicator, getsIsoIndicator;

        OfferFrame offerFrame;

        OfferEntry &oe = offerFrame.mOffer;

        statement st = (prep,
            into(accountID), into(oe.sequence),
            into(paysIsoCurrency, paysIsoIndicator), into(paysIssuer),
            into(getsIsoCurrency, getsIsoIndicator), into(getsIssuer),
            into(oe.amount), into(oe.price.n), into(oe.price.d), into(oe.flags)
            );

        st.execute(true);
        while (st.got_data())
        {
            oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
            if (paysIsoIndicator == soci::i_ok)
            {
                oe.takerPays.type(ISO4217);
                strToCurrencyCode(oe.takerPays.isoCI().currencyCode, paysIsoCurrency);
                oe.takerPays.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, paysIssuer);
            }
            else
            {
                oe.takerPays.type(NATIVE);
            }
            if (getsIsoIndicator == soci::i_ok)
            {
                oe.takerGets.type(ISO4217);
                strToCurrencyCode(oe.takerGets.isoCI().currencyCode, getsIsoCurrency);
                oe.takerGets.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, getsIssuer);
            }
            else
            {
                oe.takerGets.type(NATIVE);
            }
            offerFrame.mKeyCalculated = false;
            offerProcessor(offerFrame);
            st.fetch();
        }
    }

    void OfferFrame::loadBestOffers(size_t numOffers, size_t offset, const Currency & pays,
        const Currency & gets, vector<OfferFrame>& retOffers, Database& db)
    {
        soci::session &session = db.getSession();

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector);

        std::string getCurrencyCode, b58GIssuer;
        std::string payCurrencyCode, b58PIssuer;

        if (pays.type() == NATIVE)
        {
            sql << " WHERE paysIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(pays.isoCI().currencyCode, payCurrencyCode);
            b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
            sql << " WHERE paysIsoCurrency=:pcur AND paysIssuer = :pi", use(payCurrencyCode), use(b58PIssuer);
        }

        if (gets.type() == NATIVE)
        {
            sql << " AND getsIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(gets.isoCI().currencyCode, getCurrencyCode);
            b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);

            sql << " AND getsIsoCurrency=:gcur AND getsIssuer = :gi", use(getCurrencyCode), use(b58GIssuer);
        }
        sql << " order by price,sequence,accountID limit :o,:n", use(offset), use(numOffers);

        loadOffers(sql, [&retOffers](OfferFrame const &of)
        {
            retOffers.push_back(of);
        });
    }

    void OfferFrame::loadOffers(const uint256& accountID, std::vector<OfferFrame>& retOffers, Database& db)
    {
        soci::session &session = db.getSession();

        std::string accStr;
        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector << " WHERE accountID=:id", use(accStr));

        loadOffers(sql, [&retOffers](OfferFrame const &of)
        {
            retOffers.push_back(of);
        });
    }

    bool OfferFrame::exists(Database& db, LedgerKey const& key)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, key.offer().accountID);
        int exists = 0;
        db.getSession() <<
            "SELECT EXISTS (SELECT NULL FROM Offers \
             WHERE accountID=:id AND sequence=:s)",
            use(b58AccountID), use(key.offer().sequence),
            into(exists);
        return exists != 0;
    }

    void OfferFrame::storeDelete(LedgerDelta &delta, Database& db)
    {
        storeDelete(delta, db, getKey());
    }

    void OfferFrame::storeDelete(LedgerDelta &delta, Database& db, LedgerKey const& key)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, key.offer().accountID);

        db.getSession() <<
            "DELETE FROM Offers WHERE accountID=:id AND sequence=:s",
            use(b58AccountID), use(key.offer().sequence);

        delta.deleteEntry(key);
    }

    int64_t OfferFrame::computePrice() const
    {
        return bigDivide(mOffer.price.n, OFFER_PRICE_DIVISOR,
            mOffer.price.d);
    }

    void OfferFrame::storeChange(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mOffer.accountID);

        soci::statement st = (db.getSession().prepare <<
            "UPDATE Offers SET amount=:a, priceN=:n, priceD=:D, price=:p WHERE accountID=:id AND sequence=:s",
            use(mOffer.amount),
            use(mOffer.price.n), use(mOffer.price.d),
            use(computePrice()), use(b58AccountID), use(mOffer.sequence));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.modEntry(*this);
    }

    void OfferFrame::storeAdd(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mOffer.accountID);

        soci::statement st(db.getSession().prepare << "select 1");

        if(mOffer.takerGets.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mOffer.takerPays.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,paysIsoCurrency,paysIssuer,"\
                "amount,priceN,priceP,price,flags) values"\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mOffer.sequence),
                use(b58issuer),use(currencyCode),use(mOffer.amount),
                use(mOffer.price.n), use(mOffer.price.d),
                use(computePrice()),use(mOffer.flags));
            st.execute(true);
        }
        else if(mOffer.takerPays.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mOffer.takerGets.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,getsIsoCurrency,getsIssuer,"\
                "amount,priceN,priceD,price,flags) values"\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mOffer.sequence),
                use(b58issuer), use(currencyCode), use(mOffer.amount),
                use(mOffer.price.n), use(mOffer.price.d),
                use(computePrice()), use(mOffer.flags));
            st.execute(true);
        }
        else
        {
            std::string b58PaysIssuer = toBase58Check(VER_ACCOUNT_ID, mOffer.takerPays.isoCI().issuer);
            std::string paysIsoCurrency, getsIsoCurrency;
            currencyCodeToStr(mOffer.takerPays.isoCI().currencyCode, paysIsoCurrency);
            std::string b58GetsIssuer = toBase58Check(VER_ACCOUNT_ID, mOffer.takerGets.isoCI().issuer);
            currencyCodeToStr(mOffer.takerGets.isoCI().currencyCode, getsIsoCurrency);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "paysIsoCurrency,paysIssuer,getsIsoCurrency,getsIssuer,"\
                "amount,priceN,priceD,price,flags) values "\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10,:v11)",
                use(b58AccountID), use(mOffer.sequence),
                use(paysIsoCurrency), use(b58PaysIssuer), use(getsIsoCurrency), use(b58GetsIssuer),
                use(mOffer.amount),
                use(mOffer.price.n), use(mOffer.price.d),
                use(computePrice()), use(mOffer.flags));
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
