#pragma once

#include "main/Config.h"
#include "util/GlobalChecks.h"

namespace stellar
{
class Application;
class OverlayManager;
class LedgerManager;
class Herder;
class BanManager;

// Helper class to isolate access to Application; all function helpers must
// either be called from main or be thread-sade
class OverlayAppConnector
{
    Application& mApp;

  public:
    OverlayAppConnector(Application& app) : mApp(app)
    {
    }

    /* Methods that can only be called from main thread */
    Herder& getHerder();
    LedgerManager& getLedgerManager();
    OverlayManager& getOverlayManager();
    BanManager& getBanManager();

    void postOnMainThread(
        std::function<void()>&& f, std::string&& message,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION);

    VirtualClock::time_point now() const;
    Config const& getConfig() const;
    bool overlayShuttingDown() const;
    bool shouldYield() const;
};
}