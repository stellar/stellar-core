// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/Timer.h"
#include "xdrpp/marshal.h"

#include "main/Config.h"

using namespace stellar;
using namespace stellar::txtest;
using namespace std;

TEST_CASE("genesisledger", "[ledger]")
{
    VirtualClock clock{};
    auto cfg = getTestConfig(0);
    cfg.USE_CONFIG_FOR_GENESIS = false;
    auto app = Application::create<ApplicationImpl>(clock, cfg);
    app->start();

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
    auto const& header = lcl.header;
    REQUIRE(header.ledgerVersion == 0);
    REQUIRE(header.previousLedgerHash == Hash{});
    REQUIRE(header.scpValue.txSetHash == Hash{});
    REQUIRE(header.scpValue.closeTime == 0);
    REQUIRE(header.scpValue.upgrades.size() == 0);
    REQUIRE(header.txSetResultHash == Hash{});
    REQUIRE(binToHex(header.bucketListHash) ==
            "4e6a8404d33b17eee7031af0b3606b6af8e36fe5a3bff59e4e5e420bd0ad3bf4");
    REQUIRE(header.ledgerSeq == 1);
    REQUIRE(header.totalCoins == 1000000000000000000);
    REQUIRE(header.feePool == 0);
    REQUIRE(header.inflationSeq == 0);
    REQUIRE(header.idPool == 0);
    REQUIRE(header.baseFee == 100);
    REQUIRE(header.baseReserve == 100000000);
    REQUIRE(header.maxTxSetSize == 100);
    REQUIRE(header.skipList.size() == 4);
    REQUIRE(header.skipList[0] == Hash{});
    REQUIRE(header.skipList[1] == Hash{});
    REQUIRE(header.skipList[2] == Hash{});
    REQUIRE(header.skipList[3] == Hash{});
    REQUIRE(binToHex(lcl.hash) ==
            "caf73c70dde8134f792535756cc3212f65007883e8959adf92e48062f401e543");
}

TEST_CASE("ledgerheader", "[ledger]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT));

    Hash saved;
    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg);
        app->start();

        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = TxSetXDRFrame::makeEmpty(lcl);

        // close this ledger
        StellarValue sv = app->getHerder().makeStellarValue(
            txSet->getContentsHash(), makeConsensusTime(1), emptyUpgradeSteps,
            app->getConfig().NODE_SEED);
        LedgerCloseData ledgerData(lcl.header.ledgerSeq + 1, txSet, sv);
        app->getLedgerManager().applyLedger(ledgerData);

        saved = app->getLedgerManager().getLastClosedLedgerHeader().hash;
    }

    SECTION("load existing ledger")
    {
        Config cfg2(cfg);
        cfg2.FORCE_SCP = false;
        VirtualClock clock2;
        Application::pointer app2 = Application::create(clock2, cfg2, false);
        app2->start();

        REQUIRE(saved ==
                app2->getLedgerManager().getLastClosedLedgerHeader().hash);
    }
}

TEST_CASE_VERSIONS("base reserve", "[ledger]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
    REQUIRE(lcl.header.baseReserve == 100000000);
    uint32 const n = 20000;
    int64 expectedReserve = 2000200000000ll;

    for_versions_to(8, *app, [&]() {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(getMinBalance(ltx.loadHeader().current(), n, 0, 0) <
                expectedReserve);
    });
    for_versions_from(9, *app, [&]() {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(getMinBalance(ltx.loadHeader().current(), n, 0, 0) ==
                expectedReserve);
    });
}

TEST_CASE("close time helpers", "[ledger][herder]")
{
    auto const msVersion =
        static_cast<uint32_t>(MS_CLOSE_TIME_PROTOCOL_VERSION);

    SECTION("protocol gate")
    {
        REQUIRE(protocolHasMsCloseTime(msVersion));
        REQUIRE(!protocolHasMsCloseTime(msVersion - 1));
    }

    SECTION("whole-second conversions")
    {
        auto const t = makeConsensusTime(123);
        REQUIRE(t.isWholeSecond());
        REQUIRE(t.toApplyTime() == ApplyTime::fromTimePoint(123));
        REQUIRE(t.toString() == "123.000");
        REQUIRE(t.toSystemTime() == VirtualClock::from_time_t(123));

        // Before the ms protocol, system time rounds down to the whole second
        // and the next close time is the next whole second
        auto const systemTime =
            VirtualClock::from_time_t(123) + std::chrono::milliseconds(456);
        REQUIRE(ConsensusTime::fromSystemTime(systemTime, msVersion - 1) == t);
        REQUIRE(t.next(msVersion - 1) == makeConsensusTime(124));

        // Seconds too large to express in ms saturate to the largest whole
        // second, so the invariant survives, and the next close time cannot
        // advance past it
        auto const top = makeConsensusTime(UINT64_MAX / 1000);
        REQUIRE(top.isWholeSecond());
        REQUIRE(makeConsensusTime(UINT64_MAX / 1000 + 1) == top);
        REQUIRE(makeConsensusTime(UINT64_MAX) == top);
        REQUIRE(top.next(msVersion - 1) == top);
    }

#ifdef MS_CLOSE_TIME
    SECTION("sub-second conversions")
    {
        auto const t = ConsensusTime::fromMilliseconds(123456);
        REQUIRE(t.milliseconds() == 123456);
        REQUIRE(!t.isWholeSecond());
        REQUIRE(t == makeConsensusTime(123, 456));
        REQUIRE(makeConsensusTime(123).milliseconds() == 123000);
        // Whole-second views round down
        REQUIRE(t.toApplyTime() == ApplyTime::fromTimePoint(123));
        REQUIRE(t.toString() == "123.456");

        // System time keeps its ms from the ms protocol on
        auto const systemTime =
            VirtualClock::from_time_t(123) + std::chrono::milliseconds(456);
        REQUIRE(ConsensusTime::fromSystemTime(systemTime, msVersion) == t);
        REQUIRE(t.toSystemTime() == systemTime);

        // The next close time advances by 1ms from the ms protocol on...
        REQUIRE(t.next(msVersion) == ConsensusTime::fromMilliseconds(123457));
        REQUIRE(ConsensusTime::fromMilliseconds(123999).next(msVersion) ==
                makeConsensusTime(124));
        REQUIRE(makeConsensusTime(123).next(msVersion) ==
                ConsensusTime::fromMilliseconds(123001));
        // ...and saturates at the top of the range
        REQUIRE(ConsensusTime::fromMilliseconds(UINT64_MAX).next(msVersion) ==
                ConsensusTime::fromMilliseconds(UINT64_MAX));
        REQUIRE(makeConsensusTime(UINT64_MAX).milliseconds() ==
                (UINT64_MAX / 1000) * 1000);
    }

    SECTION("ledger header rejects inconsistent ms close time")
    {
        // A stored header's closeTime must be its closeTimeMs rounded down to
        // the whole second. Only the close time fields matter to this check, so
        // the values carry no lcValueSignature
        TimePoint const T = 1'700'000'000;

        auto msValue = [](TimePoint closeTime,
                          TimePointMilliseconds closeTimeMs) {
            StellarValue sv;
            sv.ext.v(STELLAR_VALUE_SIGNED_MS);
            sv.closeTime = closeTime;
            sv.ext.signedMsValue().closeTimeMs = closeTimeMs;
            return sv;
        };

        LedgerHeader header;
        header.scpValue = msValue(T, T * 1000 + 250);
        REQUIRE_NOTHROW(LedgerHeaderUtils::encodeHeader(header));

        // closeTime behind closeTimeMs
        header.scpValue = msValue(T, (T + 1) * 1000);
        REQUIRE_THROWS(LedgerHeaderUtils::encodeHeader(header));

        // closeTimeMs holding only the sub-second remainder, not a full close
        // time
        header.scpValue = msValue(T, 250);
        REQUIRE_THROWS(LedgerHeaderUtils::encodeHeader(header));

        // Whole-second values have no closeTimeMs to disagree with
        header.scpValue.ext.v(STELLAR_VALUE_SIGNED);
        header.scpValue.closeTime = T;
        REQUIRE_NOTHROW(LedgerHeaderUtils::encodeHeader(header));
    }
#endif // MS_CLOSE_TIME
}
