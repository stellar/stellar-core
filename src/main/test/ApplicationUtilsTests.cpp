// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/test/HistoryTestsUtils.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/ApplicationUtils.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include <filesystem>
#include <fstream>

using namespace stellar;
using namespace stellar::historytestutils;

class TemporaryFileDamager
{
  protected:
    std::filesystem::path mVictim;
    std::filesystem::path mVictimSaved;

  public:
    TemporaryFileDamager(std::filesystem::path victim) : mVictim(victim)
    {
        mVictimSaved = mVictim;
        mVictimSaved.replace_filename(mVictim.filename().string() + "-saved");
        std::filesystem::copy_file(mVictim, mVictimSaved);
    }
    virtual void
    damageVictim()
    {
        // Default damage just truncates the file.
        std::ofstream out(mVictim);
    }
    ~TemporaryFileDamager()
    {
        std::filesystem::remove(mVictim);
        std::filesystem::rename(mVictimSaved, mVictim);
    }
};

class TemporarySQLiteDBDamager : public TemporaryFileDamager
{
    Config mConfig;
    static std::filesystem::path
    getSQLiteDBPath(Config const& cfg)
    {
        auto str = cfg.DATABASE.value;
        std::string prefix = "sqlite3://";
        REQUIRE(str.find(prefix) == 0);
        str = str.substr(prefix.size());
        REQUIRE(!str.empty());
        std::filesystem::path path(str);
        REQUIRE(std::filesystem::exists(path));
        return path;
    }

  public:
    TemporarySQLiteDBDamager(Config const& cfg)
        : TemporaryFileDamager(getSQLiteDBPath(cfg)), mConfig(cfg)
    {
    }
    void
    damageVictim() override
    {
        // Damage a database by bumping the root account's last-modified.
        VirtualClock clock;
        auto app = createTestApplication(clock, mConfig, /*newDB=*/false);
        LedgerTxn ltx(app->getLedgerTxnRoot(),
                      /*shouldUpdateLastModified=*/false);
        {
            auto rootKey = accountKey(
                stellar::txtest::getRoot(app->getNetworkID()).getPublicKey());
            auto rootLe = ltx.load(rootKey);
            rootLe.current().lastModifiedLedgerSeq += 1;
        }
        ltx.commit();
    }
};
TEST_CASE("offline self-check works", "[applicationutils][selfcheck]")
{
    // Step 1: set up history archives and publish to them.
    Config chkConfig;
    CatchupSimulation catchupSimulation{};
    auto l1 = catchupSimulation.getLastCheckpointLedger(2);
    auto l2 = catchupSimulation.getLastCheckpointLedger(4);
    catchupSimulation.ensureOfflineCatchupPossible(l2);
    std::filesystem::path victimBucketPath;
    {
        // Step 2: make a new application and catch it up part-way to the
        // archives (but behind).
        auto app = catchupSimulation.createCatchupApplication(
            std::numeric_limits<uint32_t>::max(), Config::TESTDB_ON_DISK_SQLITE,
            "client");
        catchupSimulation.catchupOffline(app, l1);
        chkConfig = app->getConfig();
        victimBucketPath = app->getBucketManager()
                               .getBucketList()
                               .getLevel(0)
                               .getCurr()
                               ->getFilename();
    }

    // Step 3: run offline self-check on that application's state, see that it's
    // agreeable.
    REQUIRE(selfCheck(chkConfig) == 0);

    std::filesystem::path archPath =
        catchupSimulation.getHistoryConfigurator().getArchiveDirName();

    // Step 4: require various self-check failures are caught.
    {
        // Damage the well-known HAS path in the archive.
        auto path = archPath;
        path /= HistoryArchiveState::wellKnownRemoteName();
        TemporaryFileDamager damage(path);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage the target ledger in the archive.
        auto path = archPath;
        path /=
            fs::remoteName(HISTORY_FILE_TYPE_LEDGER, fs::hexStr(l1), "xdr.gz");
        TemporaryFileDamager damage(path);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage a bucket file.
        TemporaryFileDamager damage(victimBucketPath);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
    {
        // Damage the SQL ledger.
        TemporarySQLiteDBDamager damage(chkConfig);
        damage.damageVictim();
        REQUIRE(selfCheck(chkConfig) == 1);
    }
}
