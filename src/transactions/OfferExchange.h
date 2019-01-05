#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include <functional>
#include <vector>

namespace stellar
{

class AbstractLedgerTxn;
class ConstLedgerTxnEntry;
class LedgerTxnEntry;
class LedgerTxnHeader;
class TrustLineWrapper;
class ConstTrustLineWrapper;

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

int64_t canSellAtMostBasedOnSheep(LedgerTxnHeader const& header,
                                  Asset const& sheep,
                                  ConstTrustLineWrapper const& sheepLine,
                                  Price const& wheatPrice);

int64_t canSellAtMost(LedgerTxnHeader const& header,
                      LedgerTxnEntry const& account, Asset const& asset,
                      TrustLineWrapper const& trustLine);
int64_t canSellAtMost(LedgerTxnHeader const& header,
                      ConstLedgerTxnEntry const& account, Asset const& asset,
                      ConstTrustLineWrapper const& trustLine);

int64_t canBuyAtMost(LedgerTxnHeader const& header,
                     LedgerTxnEntry const& account, Asset const& asset,
                     TrustLineWrapper const& trustLine);
int64_t canBuyAtMost(LedgerTxnHeader const& header,
                     ConstLedgerTxnEntry const& account, Asset const& asset,
                     ConstTrustLineWrapper const& trustLine);

ExchangeResult exchangeV2(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResult exchangeV3(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResultV10 exchangeV10(Price price, int64_t maxWheatSend,
                              int64_t maxWheatReceive, int64_t maxSheepSend,
                              int64_t maxSheepReceive, bool isPathPayment);

ExchangeResultV10 exchangeV10WithoutPriceErrorThresholds(
    Price price, int64_t maxWheatSend, int64_t maxWheatReceive,
    int64_t maxSheepSend, int64_t maxSheepReceive, bool isPathPayment);
ExchangeResultV10 applyPriceErrorThresholds(Price price, int64_t wheatReceive,
                                            int64_t sheepSend, bool wheatStays,
                                            bool isPathPayment);

int64_t adjustOffer(Price const& price, int64_t maxWheatSend,
                    int64_t maxSheepReceive);

bool checkPriceErrorBound(Price price, int64_t wheatReceive, int64_t sheepSend,
                          bool canFavorWheat);

enum class OfferFilterResult
{
    eKeep,
    eStop
};

enum class ConvertResult
{
    eOK,
    ePartial,
    eFilterStop
};

enum class CrossOfferResult
{
    eOfferPartial,
    eOfferTaken,
    eOfferCantConvert
};

// buys wheat with sheep, crossing as many offers as necessary
ConvertResult convertWithOffers(
    AbstractLedgerTxn& ltx, Asset const& sheep, int64_t maxSheepSent,
    int64_t& sheepSend, Asset const& wheat, int64_t maxWheatReceive,
    int64_t& wheatReceived, bool isPathPayment,
    std::function<OfferFilterResult(LedgerTxnEntry const&)> filter,
    std::vector<ClaimOfferAtom>& offerTrail);
}
