#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerManagerImpl.h"
#include "main/ApplicationImpl.h"
#include <type_traits>

namespace stellar
{

class LoopbackPeerConnection;

namespace testutil
{
void crankSome(VirtualClock& clock);
void crankFor(VirtualClock& clock, VirtualClock::duration duration);
void injectSendPeersAndReschedule(VirtualClock::time_point& end,
                                  VirtualClock& clock, VirtualTimer& timer,
                                  LoopbackPeerConnection& connection);

void shutdownWorkScheduler(Application& app);

std::vector<Asset> getInvalidAssets(SecretKey const& issuer);

class BucketListDepthModifier
{
    uint32_t const mPrevDepth;

  public:
    BucketListDepthModifier(uint32_t newDepth);

    ~BucketListDepthModifier();
};

inline BucketMetadata
testBucketMetadata(uint32_t protocolVersion)
{
    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;
    return meta;
}
}

class TestInvariantManager : public InvariantManagerImpl
{
  public:
    TestInvariantManager(medida::MetricsRegistry& registry);

  private:
    virtual void
    handleInvariantFailure(std::shared_ptr<Invariant> invariant,
                           std::string const& message) const override;
};

class TestApplication : public ApplicationImpl
{
  public:
    TestApplication(VirtualClock& clock, Config const& cfg);

  private:
    std::unique_ptr<InvariantManager> createInvariantManager() override;
};

template <typename T = TestApplication, typename... Args,
          typename = typename std::enable_if<
              std::is_base_of<TestApplication, T>::value>::type>
std::shared_ptr<T>
createTestApplication(VirtualClock& clock, Config const& cfg, Args&&... args,
                      bool newDB = true)
{
    Config c2(cfg);
    c2.adjust();
    auto app = Application::create<T, Args...>(
        clock, c2, std::forward<Args>(args)..., newDB);
    return app;
}

time_t getTestDate(int day, int month, int year);
std::tm getTestDateTime(int day, int month, int year, int hour, int minute,
                        int second);

VirtualClock::system_time_point genesis(int minute, int second);
}
