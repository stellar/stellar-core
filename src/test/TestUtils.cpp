// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TestUtils.h"
#include "crypto/SHA.h"
#include "herder/LedgerCloseData.h"
#include "transport/LoopbackPeer.h"
#include "util/make_unique.h"

#include <xdrpp/marshal.h>

namespace stellar
{

namespace testutil
{

void
setCurrentLedgerVersion(LedgerManager& lm, uint32_t currentLedgerVersion)
{
    lm.getCurrentLedgerHeader().ledgerVersion = currentLedgerVersion;
}

void
crankSome(VirtualClock& clock)
{
    auto start = clock.now();
    for (size_t i = 0;
         (i < 100 && clock.now() < (start + std::chrono::seconds(1)) &&
          clock.crank(false) > 0);
         ++i)
        ;
}

void
injectSendPeersAndReschedule(VirtualClock::time_point& end, VirtualClock& clock,
                             VirtualTimer& timer,
                             LoopbackPeerConnection& connection)
{
    connection.getInitiator()->sendGetPeers();
    if (clock.now() < end && connection.getInitiator()->isConnected())
    {
        timer.expires_from_now(std::chrono::milliseconds(10));
        timer.async_wait([&](asio::error_code const& ec) {
            if (!ec)
            {
                injectSendPeersAndReschedule(end, clock, timer, connection);
            }
        });
    }
}

BucketListDepthModifier::BucketListDepthModifier(uint32_t newDepth)
    : mPrevDepth(BucketList::kNumLevels)
{
    BucketList::kNumLevels = newDepth;
}

BucketListDepthModifier::~BucketListDepthModifier()
{
    BucketList::kNumLevels = mPrevDepth;
}
}

TestInvariantManager::TestInvariantManager(medida::MetricsRegistry& registry)
    : InvariantManagerImpl(registry)
{
}

void
TestInvariantManager::handleInvariantFailure(
    std::shared_ptr<Invariant> invariant, std::string const& message) const
{
    throw InvariantDoesNotHold{message};
}

TestApplication::TestApplication(VirtualClock& clock, Config const& cfg)
    : ApplicationImpl(clock, cfg)
{
}

std::unique_ptr<InvariantManager>
TestApplication::createInvariantManager()
{
    return make_unique<TestInvariantManager>(getMetrics());
}

time_t
getTestDate(int day, int month, int year)
{
    auto tm = getTestDateTime(day, month, year, 0, 0, 0);

    VirtualClock::time_point tp = VirtualClock::tmToPoint(tm);
    time_t t = VirtualClock::to_time_t(tp);

    return t;
}

std::tm
getTestDateTime(int day, int month, int year, int hour, int minute, int second)
{
    std::tm tm = {0};
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_mday = day;
    tm.tm_mon = month - 1; // 0 based
    tm.tm_year = year - 1900;
    return tm;
}

VirtualClock::time_point
genesis(int minute, int second)
{
    return VirtualClock::tmToPoint(
        getTestDateTime(1, 7, 2014, 0, minute, second));
}

PublicKey
makePublicKey(int i)
{
    auto hash = sha256("NODE_SEED_" + std::to_string(i));
    auto secretKey = SecretKey::fromSeed(hash);
    return secretKey.getPublicKey();
};

SCPQuorumSet
makeSaneQuorumSet()
{
    auto result = SCPQuorumSet{};
    result.threshold = 1;
    result.validators.push_back(makePublicKey(0));
    return result;
}

SCPQuorumSet
makeBigQuorumSet()
{
    auto keys = std::vector<PublicKey>{};
    for (auto i = 0; i < 1001; i++)
    {
        keys.push_back(makePublicKey(i));
    }

    auto bigQSet = SCPQuorumSet{};
    bigQSet.threshold = 1;
    bigQSet.validators.push_back(keys[0]);
    for (auto i = 0; i < 10; i++)
    {
        bigQSet.innerSets.push_back({});
        bigQSet.innerSets.back().threshold = 1;
        for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
            bigQSet.innerSets.back().validators.push_back(keys[j]);
    }
    return bigQSet;
}

SCPEnvelope
makeEnvelope(Hash txHash, Hash qSetHash, uint64_t slotIndex)
{
    auto envelope = SCPEnvelope{};
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.pledges.type(SCP_ST_PREPARE);
    envelope.statement.pledges.prepare().ballot.value =
        xdr::xdr_to_opaque(StellarValue{txHash, 10, emptyUpgradeSteps, 0});
    envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
    return envelope;
};
}
