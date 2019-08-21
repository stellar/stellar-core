#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "util/NonCopyable.h"

namespace stellar
{

class DatabaseImpl : public Database, NonMovableOrCopyable
{
    Application& mApp;
    medida::Meter& mQueryMeter;
    bool mIsOpen{false};
    soci::session mSession;
    std::unique_ptr<soci::connection_pool> mPool;

    std::map<std::string, std::shared_ptr<soci::statement>> mStatements;
    medida::Counter& mStatementsSize;

    // Helpers for maintaining the total query time and calculating
    // idle percentage.
    std::set<std::string> mEntityTypes;
    std::chrono::nanoseconds mExcludedQueryTime;
    std::chrono::nanoseconds mExcludedTotalTime;
    std::chrono::nanoseconds mLastIdleQueryTime;
    VirtualClock::time_point mLastIdleTotalTime;

    static bool gDriversRegistered;
    static void registerDrivers();
    void applySchemaUpgrade(unsigned long vers);

  public:
    DatabaseImpl(Application& app);

    void ensureOpen() override;

    medida::Meter& getQueryMeter() override;
    std::chrono::nanoseconds totalQueryTime() const override;
    void excludeTime(std::chrono::nanoseconds const& queryTime,
                     std::chrono::nanoseconds const& totalTime) override;
    uint32_t recentIdleDbPercent() override;
    std::shared_ptr<SQLLogContext>
    captureAndLogSQL(std::string contextName) override;
    StatementContext getPreparedStatement(std::string const& query) override;
    void clearPreparedStatementCache() override;

    medida::TimerContext getInsertTimer(std::string const& entityName) override;
    medida::TimerContext getSelectTimer(std::string const& entityName) override;
    medida::TimerContext getDeleteTimer(std::string const& entityName) override;
    medida::TimerContext getUpdateTimer(std::string const& entityName) override;
    medida::TimerContext getUpsertTimer(std::string const& entityName) override;

    void setCurrentTransactionReadOnly() override;
    bool isSqlite() const override;
    bool canUsePool() const override;
    void initialize() override;
    void putSchemaVersion(unsigned long vers) override;
    unsigned long getDBSchemaVersion() override;
    unsigned long getAppSchemaVersion() override;
    void upgradeToCurrentSchema() override;
    soci::session& getSession() override;
    soci::connection_pool& getPool() override;
};
}
