#pragma once

#include "bucket/BucketUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"

namespace stellar
{
class OverlayManager;
class LedgerManager;
class Herder;
class BanManager;
struct OverlayMetrics;
class SorobanNetworkConfig;
class SorobanMetrics;
class SearchableHotArchiveBucketListSnapshot;
struct LedgerTxnDelta;
class CapacityTrackedMessage;

// Helper class to isolate access to Application; all function helpers must
// either be called from main or be thread-sade
class AppConnector
{
    Application& mApp;
    // Copy config for threads to use, and avoid warnings from thread sanitizer
    // about accessing mApp
    std::shared_ptr<const Config> const mConfig;

  public:
    explicit AppConnector(Application& app);

    // Methods that can only be called from main thread
    Herder& getHerder() const;
    LedgerManager& getLedgerManager() const;
    OverlayManager& getOverlayManager() const;
    BanManager& getBanManager() const;
    bool shouldYield() const;
    SorobanMetrics& getSorobanMetrics() const;
    void checkOnOperationApply(Operation const& operation,
                               OperationResult const& opres,
                               LedgerTxnDelta const& ltxDelta);
    Hash const& getNetworkID() const;

    // Thread-safe methods
    void postOnMainThread(
        std::function<void()>&& f, std::string&& message,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION);
    void postOnOverlayThread(std::function<void()>&& f,
                             std::string const& message);
    void postOnTxValidationThread(std::function<void()>&& f,
                                  std::string const& message);
    VirtualClock::time_point now() const;
    VirtualClock::system_time_point system_now() const;
    Config const& getConfig() const;
    std::shared_ptr<Config const> getConfigPtr() const;
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() const;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() const;

    // These `maybeGet*` methods have two main differences from their `get*`
    // counterparts:
    // 1. They return `nullopt` instead of throwing an assertion error if
    //    the value is not set.
    // 2. They make copies instead of returning references. This allows other
    //    threads to maintain their own snapshots of Soroban configs.
    std::optional<SorobanNetworkConfig>
    maybeGetLastClosedSorobanNetworkConfig() const;
    std::optional<SorobanNetworkConfig>
    maybeGetSorobanNetworkConfigForApply() const;

    bool threadIsType(Application::ThreadType type) const;

    medida::MetricsRegistry& getMetrics() const;
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot();
};
}