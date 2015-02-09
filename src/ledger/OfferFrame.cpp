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
            "CREATE TABLE IF NOT EXISTS Offers (    \
            accountID       CHARACTER(64) NOT NULL, \
            sequence        INT NOT NULL \
                            CHECK (sequence >= 0),  \
            sellIsoCurrency CHARACTER(4),           \
            sellIssuer CHARACTER(64),               \
            amountSell BIGINT NOT NULL,             \
            buyIsoCurrency CHARACTER(4),            \
            buyIssuer CHARACTER(64),                \
            amountBuy BIGINT NOT NULL,              \
            price BIGINT NOT NULL,                  \
            PRIMARY KEY (accountID, sequence)       \
    );";

    OfferFrame::OfferFrame()
    {
        mEntry.type(OFFER);
    }

    OfferFrame::OfferFrame(const LedgerEntry& from) : EntryFrame(from)
    {
        updatePrice();
    }

    void OfferFrame::updatePrice()
    {
        if (getSellAmount())
        {
            mPrice = bigDivide(getBuyAmount(), OFFER_PRICE_DIVISOR, getSellAmount());
        }
        else
        {
            mPrice = INT64_MAX;
        }
    }

    void OfferFrame::from(const Transaction& tx) 
    {
        mEntry.type(OFFER);
        mEntry.offer().accountID = tx.account;

        mEntry.offer().sell = tx.body.createOfferTx().sell;
        mEntry.offer().amountSell = tx.body.createOfferTx().amountSell;
        
        mEntry.offer().sequence = tx.body.createOfferTx().sequence;
        
        mEntry.offer().buy = tx.body.createOfferTx().buy;
        mEntry.offer().amountBuy = tx.body.createOfferTx().amountBuy;

        updatePrice();
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
        return mPrice;
    }

        Currency& OfferFrame::getSell()
    {
        return mEntry.offer().sell;
    }

    int64_t OfferFrame::getSellAmount() const
    {
        return mEntry.offer().amountSell;
    }

    Currency& OfferFrame::getBuy()
    {
        return mEntry.offer().buy;
    }

    int64_t OfferFrame::getBuyAmount() const
    {
        return mEntry.offer().amountBuy;
    }

    void OfferFrame::adjustSellAmount(int64_t newSellAmount)
    {
        mEntry.offer().amountBuy = bigDivide(
            newSellAmount, mEntry.offer().amountBuy,
            mEntry.offer().amountSell);
        mEntry.offer().amountSell = newSellAmount;
        updatePrice();
    }

    void OfferFrame::adjustBuyAmount(int64_t newBuyAmount)
    {
        mEntry.offer().amountSell = bigDivide(
            newBuyAmount, mEntry.offer().amountSell,
            mEntry.offer().amountBuy);
        mEntry.offer().amountBuy = newBuyAmount;
        updatePrice();
    }

    uint256 const& OfferFrame::getAccountID() const
    {
        return mEntry.offer().accountID;
    }

    uint32 OfferFrame::getSequence()
    {
        return mEntry.offer().sequence;
    }
    

    // TODO: move this and related SQL code to OfferFrame
    static const char *offerColumnSelector =
        "SELECT accountID,sequence,"\
        "sellIsoCurrency,sellIssuer,amountSell,"\
        "buyIsoCurrency,buyIssuer,amountBuy FROM Offers";

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
        std::string sellIsoCurrency, sellIssuer, buyIsoCurrency, buyIssuer;

        soci::indicator sellIsoIndicator, buyIsoIndicator;

        OfferFrame offerFrame;

        OfferEntry &oe = offerFrame.mEntry.offer();

        statement st = (prep,
            into(accountID), into(oe.sequence),
            into(sellIsoCurrency, sellIsoIndicator), into(sellIssuer), into(oe.amountSell),
            into(buyIsoCurrency, buyIsoIndicator), into(buyIssuer), into(oe.amountBuy)
            );

        st.execute(true);
        while (st.got_data())
        {
            oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
            if (sellIsoIndicator == soci::i_ok)
            {
                oe.sell.type(ISO4217);
                strToCurrencyCode(oe.sell.isoCI().currencyCode, sellIsoCurrency);
                oe.sell.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, sellIssuer);
            }
            else
            {
                oe.sell.type(NATIVE);
            }
            if (buyIsoIndicator == soci::i_ok)
            {
                oe.buy.type(ISO4217);
                strToCurrencyCode(oe.buy.isoCI().currencyCode, buyIsoCurrency);
                oe.buy.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, buyIssuer);
            }
            else
            {
                oe.buy.type(NATIVE);
            }
            offerFrame.updatePrice();
            offerProcessor(offerFrame);
            st.fetch();
        }
    }

    void OfferFrame::loadBestOffers(size_t numOffers, size_t offset, const Currency & buy,
        const Currency & sell, vector<OfferFrame>& retOffers, Database& db)
    {
        soci::session &session = db.getSession();

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector);

        std::string sellCurrencyCode, b58SIssuer;
        std::string buyCurrencyCode, b58BIssuer;

        if (sell.type() == NATIVE)
        {
            sql << " WHERE sellIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(sell.isoCI().currencyCode, sellCurrencyCode);
            b58SIssuer = toBase58Check(VER_ACCOUNT_ID, sell.isoCI().issuer);
            sql << " WHERE sellIsoCurrency=:scur AND sellIssuer = :si", use(sellCurrencyCode), use(b58SIssuer);
        }

        if (buy.type() == NATIVE)
        {
            sql << " AND buyIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(buy.isoCI().currencyCode, buyCurrencyCode);
            b58BIssuer = toBase58Check(VER_ACCOUNT_ID, buy.isoCI().issuer);

            sql << " AND buyIsoCurrency=:bcur AND buyIssuer = :bi", use(buyCurrencyCode), use(b58BIssuer);
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

    void OfferFrame::storeDelete(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        db.getSession() <<
            "DELETE FROM Offers WHERE accountID=:id AND sequence=:s",
            use(b58AccountID), use(mEntry.offer().sequence);

        delta.deleteEntry(*this);
    }

    void OfferFrame::storeChange(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        updatePrice();

        soci::statement st = (db.getSession().prepare <<
            "UPDATE Offers SET amountSell=:as, amountBuy=:ab, price=:p "\
            "WHERE accountID=:id AND sequence=:s",
            use(mEntry.offer().amountSell), use(mEntry.offer().amountBuy), use(mPrice),
            use(b58AccountID), use(mEntry.offer().sequence));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.modEntry(*this);
    }

    void OfferFrame::storeAdd(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        soci::statement st(db.getSession().prepare << "select 1");

        if(mEntry.offer().buy.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().sell.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().sell.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "sellIsoCurrency,sellIssuer,amountSell,"\
                "amountBuy,price) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(b58issuer), use(currencyCode), use(mEntry.offer().amountSell),
                use(mEntry.offer().amountBuy), use(getPrice()));
            st.execute(true);
        }
        else if(mEntry.offer().sell.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().sell.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().buy.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "buyIsoCurrency,buyIssuer,amountBuy,"\
                "amountSell,price) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(b58issuer), use(currencyCode), use(mEntry.offer().amountBuy),
                use(mEntry.offer().amountSell), use(getPrice()));
            st.execute(true);
        }
        else
        {
            std::string b58BuyIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().buy.isoCI().issuer);
            std::string buyIsoCurrency;
            currencyCodeToStr(mEntry.offer().buy.isoCI().currencyCode, buyIsoCurrency);

            std::string b58SellIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().sell.isoCI().issuer);
            std::string sellIsoCurrency;
            currencyCodeToStr(mEntry.offer().sell.isoCI().currencyCode, sellIsoCurrency);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "sellIsoCurrency,sellIssuer,amountSell,"\
                "buyIsoCurrency,buyIssuer,amountBuy,"\
                "price) values (:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(sellIsoCurrency), use(b58SellIssuer), use(mEntry.offer().amountSell),
                use(buyIsoCurrency), use(b58BuyIssuer), use(mEntry.offer().amountBuy),
                use(getPrice()));
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
