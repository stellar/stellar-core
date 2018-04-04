#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include <functional>
#include <memory>
#include <vector>

namespace stellar
{
class LedgerEntryReference;
class LedgerState;
class OfferReference;

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

ExchangeResult exchangeV2(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);
ExchangeResult exchangeV3(int64_t wheatReceived, Price price,
                          int64_t maxWheatReceive, int64_t maxSheepSend);

class OfferExchange
{
    std::vector<ClaimOfferAtom> mOfferTrail;

  public:
    // buys wheat with sheep from a single offer
    enum CrossOfferResult
    {
        eOfferPartial,
        eOfferTaken,
        eOfferCantConvert
    };
    CrossOfferResult crossOffer(LedgerState& ls, std::shared_ptr<LedgerEntryReference> sellingWheatOffer,
                                int64_t maxWheatReceived,
                                int64_t& numWheatReceived, int64_t maxSheepSend,
                                int64_t& numSheepSent);

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
        LedgerState& ls,
        Asset const& sheep, int64_t maxSheepSent, int64_t& sheepSend,
        Asset const& wheat, int64_t maxWheatReceive, int64_t& weatReceived,
        std::function<OfferFilterResult(OfferReference)> filter);

    std::vector<ClaimOfferAtom> const&
    getOfferTrail() const
    {
        return mOfferTrail;
    }
};
}
