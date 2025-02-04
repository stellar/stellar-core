#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/LedgerManagerImpl.h"
#include "main/ApplicationImpl.h"
#include "util/ProtocolVersion.h"
#include <type_traits>

namespace stellar
{

class LoopbackPeerConnection;
class Simulation;

namespace testutil
{
void crankSome(VirtualClock& clock);
void crankFor(VirtualClock& clock, VirtualClock::duration duration);
void crankUntil(Application::pointer app,
                std::function<bool()> const& predicate,
                VirtualClock::duration timeout);
void crankUntil(Application& app, std::function<bool()> const& predicate,
                VirtualClock::duration timeout);
void shutdownWorkScheduler(Application& app);

std::vector<Asset> getInvalidAssets(SecretKey const& issuer);

int32_t computeMultiplier(LedgerEntry const& le);

template <class BucketT> class BucketListDepthModifier
{
    BUCKET_TYPE_ASSERT(BucketT);

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
    if (protocolVersionStartsFrom(
            protocolVersion,
            HotArchiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        meta.ext.v(1);
        meta.ext.bucketListType() = BucketListType::LIVE;
    }

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
                      bool newDB = true, bool startApp = true)
{
    Config c2(cfg);
    c2.adjust();
    auto app = Application::create<T, Args...>(
        clock, c2, std::forward<Args>(args)..., newDB);
    if (startApp)
    {
        app->start();
    }
    return app;
}

TimePoint getTestDate(int day, int month, int year);
std::tm getTestDateTime(int day, int month, int year, int hour, int minute,
                        int second);

VirtualClock::system_time_point genesis(int minute, int second);

// Assigns values to the SorobanNetworkConfig fields that are suitable for
// most of the unit tests.
void setSorobanNetworkConfigForTest(SorobanNetworkConfig& cfg);

// Override Soroban network config defaults with generous settings suitable
// for most of the unit tests (unless the test is meant to exercise the
// configuration limits).
void overrideSorobanNetworkConfigForTest(Application& app);

// Runs loadgen to arm all nodes in simulation for the given upgrade. If
// applyUpgrade == true, close ledgers until the upgrade has been applied.
// Otherwise just arm the nodes without closing the ledger containing the
// upgrade.
void
upgradeSorobanNetworkConfig(std::function<void(SorobanNetworkConfig&)> modifyFn,
                            std::shared_ptr<Simulation> simulation,
                            bool applyUpgrade = true);
void
modifySorobanNetworkConfig(Application& app,
                           std::function<void(SorobanNetworkConfig&)> modifyFn);

bool appProtocolVersionStartsFrom(Application& app,
                                  ProtocolVersion fromVersion);

// Large enough fee to cover most of the Soroban transactions.
constexpr uint32_t DEFAULT_TEST_RESOURCE_FEE = 1'000'000;
}
