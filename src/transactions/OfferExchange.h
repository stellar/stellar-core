#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/OfferFrame.h"
#include "ledger/TrustFrame.h"
#include "transactions/OperationFrame.h"
#include <functional>
#include <vector>

namespace stellar
{

enum class ExchangeResultType
{
    NORMAL,
    REDUCED_TO_ZERO,
    BOGUS
};

struct ExchangeResult
{
    int64_t numWheatReceived;
    int64_t numSheepSend;
    bool reduced;

    ExchangeResultType
    type() const
    {
        if (numWheatReceived != 0 && numSheepSend != 0)
            return ExchangeResultType::NORMAL;
        else
            return reduced ? ExchangeResultType::REDUCED_TO_ZERO
                           : ExchangeResultType::BOGUS;
    }
};

struct ExchangeResultV10
{
    int64_t numWheatReceived;
    int64_t numSheepSend;
    bool wheatStays;
};

int64_t canSellAtMostBasedOnSheep(Asset const& sheep,
                                  TrustFrame::pointer sheepLine,
                                  Price const& wheatPrice);

int64_t canSellAtMost(AccountFrame::pointer account, Asset const& asset,
                      TrustFrame::pointer trustLine,
                      LedgerManager& ledgerManager);
int64_t canBuyAtMost(Asset const& asset, TrustFrame::pointer trustLine);

ExchangeResult exchangeV2(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResult exchangeV3(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResultV10 exchangeV10(Price price, int64_t maxWheatSend,
                              int64_t maxWheatReceive, int64_t maxSheepSend,
                              int64_t maxSheepReceive, bool isPathPayment);

void adjustOffer(OfferFrame& offer, LedgerManager& lm,
                 AccountFrame::pointer account, Asset const& wheat,
                 TrustFrame::pointer wheatLine, Asset const& sheep,
                 TrustFrame::pointer sheepLine);

int64_t adjustOffer(Price const& price, int64_t maxWheatSend,
                    int64_t maxSheepReceive);

bool checkPriceErrorBound(Price price, int64_t wheatReceive, int64_t sheepSend,
                          bool canFavorWheat);

class LoadBestOfferContext
{
    Asset const mSelling;
    Asset const mBuying;

    Database& mDb;

    std::vector<OfferFrame::pointer> mBatch;
    std::vector<OfferFrame::pointer>::iterator mBatchIterator;

    void loadBatchIfNecessary();

  public:
    LoadBestOfferContext(Database& db, Asset const& selling,
                         Asset const& buying);

    OfferFrame::pointer loadBestOffer();

    void eraseAndUpdate();
};

class OfferExchange
{

    LedgerDelta& mDelta;
    LedgerManager& mLedgerManager;

    std::vector<ClaimOfferAtom> mOfferTrail;

  public:
    OfferExchange(LedgerDelta& delta, LedgerManager& ledgerManager);

    // buys wheat with sheep from a single offer
    enum CrossOfferResult
    {
        eOfferPartial,
        eOfferTaken,
        eOfferCantConvert
    };
    CrossOfferResult crossOffer(OfferFrame& sellingWheatOffer,
                                int64_t maxWheatReceived,
                                int64_t& numWheatReceived, int64_t maxSheepSend,
                                int64_t& numSheepSent);

    CrossOfferResult crossOfferV10(OfferFrame& sellingWheatOffer,
                                   int64_t maxWheatReceived,
                                   int64_t& numWheatReceived,
                                   int64_t maxSheepSend, int64_t& numSheepSent,
                                   bool& wheatStays, bool isPathPayment);

    enum OfferFilterResult
    {
        eKeep,
        eStop
    };

    enum ConvertResult
    {
        eOK,
        ePartial,
        eFilterStop
    };
    // buys wheat with sheep, crossing as many offers as necessary
    ConvertResult convertWithOffers(
        Asset const& sheep, int64_t maxSheepSent, int64_t& sheepSend,
        Asset const& wheat, int64_t maxWheatReceive, int64_t& wheatReceived,
        bool isPathPayment,
        std::function<OfferFilterResult(OfferFrame const&)> filter);

    std::vector<ClaimOfferAtom> const&
    getOfferTrail() const
    {
        return mOfferTrail;
    }
};
}
