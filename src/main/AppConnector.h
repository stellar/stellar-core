#pragma once

#include "bucket/BucketUtils.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"

namespace stellar
{
class Application;
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
    Config const mConfig;

  public:
    AppConnector(Application& app);

    // Methods that can only be called from main thread
    Herder& getHerder();
    LedgerManager& getLedgerManager();
    OverlayManager& getOverlayManager();
    BanManager& getBanManager();
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
    VirtualClock::time_point now() const;
    Config const& getConfig() const;
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getSorobanNetworkConfigReadOnly() const;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() const;

    medida::MetricsRegistry& getMetrics() const;
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot();
};
}