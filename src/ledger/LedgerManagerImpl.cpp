// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/HerderPersistence.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayManager.h"
#include "transactions/OperationFrame.h"
#include "transactions/TransactionSQL.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include <fmt/format.h>

#include "medida/buckets.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"
#include <Tracy.hpp>

#include <chrono>
#include <numeric>
#include <regex>
#include <sstream>
#include <thread>

/*
The ledger module:
    1) gets the externalized tx set
    2) applies this set to the last closed ledger
    3) sends the changed entries to the BucketList
    4) saves the changed entries to SQL
    5) saves the ledger hash and header to SQL
    6) sends the new ledger hash and the tx set to the history
    7) sends the new ledger hash and header to the Herder


catching up to network:
    1) Wait for SCP to tell us what the network is on now
    2) Pull history log or static deltas from history archive
    3) Replay or force-apply deltas, depending on catchup mode

*/
using namespace std;

namespace stellar
{

const uint32_t LedgerManager::GENESIS_LEDGER_SEQ = 1;
const uint32_t LedgerManager::GENESIS_LEDGER_VERSION = 0;
const uint32_t LedgerManager::GENESIS_LEDGER_BASE_FEE = 100;
const uint32_t LedgerManager::GENESIS_LEDGER_BASE_RESERVE = 100000000;
const uint32_t LedgerManager::GENESIS_LEDGER_MAX_TX_SIZE = 100;
const int64_t LedgerManager::GENESIS_LEDGER_TOTAL_COINS = 1000000000000000000;

std::unique_ptr<LedgerManager>
LedgerManager::create(Application& app)
{
    return std::make_unique<LedgerManagerImpl>(app);
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeader const& header)
{
    return ledgerAbbrev(header, xdrSha256(header));
}

std::string
LedgerManager::ledgerAbbrev(uint32_t seq, uint256 const& hash)
{
    std::ostringstream oss;
    oss << "[seq=" << seq << ", hash=" << hexAbbrev(hash) << "]";
    return oss.str();
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeader const& header, uint256 const& hash)
{
    return ledgerAbbrev(header.ledgerSeq, hash);
}

std::string
LedgerManager::ledgerAbbrev(LedgerHeaderHistoryEntry const& he)
{
    return ledgerAbbrev(he.header, he.hash);
}

LedgerManagerImpl::LedgerManagerImpl(Application& app)
    : mApp(app)
    , mTransactionApply(
          app.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mTransactionCount(
          app.getMetrics().NewHistogram({"ledger", "transaction", "count"}))
    , mOperationCount(
          app.getMetrics().NewHistogram({"ledger", "operation", "count"}))
    , mPrefetchHitRate(
          app.getMetrics().NewHistogram({"ledger", "prefetch", "hit-rate"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
    , mLedgerAgeClosed(app.getMetrics().NewBuckets(
          {"ledger", "age", "closed"}, {5000.0, 7000.0, 10000.0, 20000.0}))
    , mLedgerAge(
          app.getMetrics().NewCounter({"ledger", "age", "current-seconds"}))
    , mLastClose(mApp.getClock().now())
    , mCatchupDuration(
          app.getMetrics().NewTimer({"ledger", "catchup", "duration"}))
    , mMetaStreamWriteTime(
          app.getMetrics().NewTimer({"ledger", "metastream", "write"}))
    , mState(LM_BOOTING_STATE)

{
    setupLedgerCloseMetaStream();
}

void
LedgerManagerImpl::moveToSynced()
{
    setState(LM_SYNCED_STATE);
}

void
LedgerManagerImpl::setState(State s)
{
    if (s != getState())
    {
        std::string oldState = getStateHuman();
        mState = s;
        mApp.syncOwnMetrics();
        CLOG_INFO(Ledger, "Changing state {} -> {}", oldState, getStateHuman());
        if (mState != LM_CATCHING_UP_STATE)
        {
            mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
        }

        if (mState == LM_CATCHING_UP_STATE && !mStartCatchup)
        {
            mStartCatchup = std::make_unique<VirtualClock::time_point>(
                mApp.getClock().now());
        }
        else if (mState == LM_SYNCED_STATE && mStartCatchup)
        {
            std::chrono::nanoseconds duration =
                mApp.getClock().now() - *mStartCatchup;
            mCatchupDuration.Update(duration);
            CLOG_DEBUG(Perf, "Caught up to the network in {} seconds",
                       std::chrono::duration<double>(duration).count());
        }
    }
}

LedgerManager::State
LedgerManagerImpl::getState() const
{
    return mState;
}

std::string
LedgerManagerImpl::getStateHuman() const
{
    static const char* stateStrings[LM_NUM_STATE] = {
        "LM_BOOTING_STATE", "LM_SYNCED_STATE", "LM_CATCHING_UP_STATE"};
    return std::string(stateStrings[getState()]);
}

LedgerHeader
LedgerManager::genesisLedger()
{
    LedgerHeader result;
    // all fields are initialized by default to 0
    // set the ones that are not 0
    result.ledgerVersion = GENESIS_LEDGER_VERSION;
    result.baseFee = GENESIS_LEDGER_BASE_FEE;
    result.baseReserve = GENESIS_LEDGER_BASE_RESERVE;
    result.maxTxSetSize = GENESIS_LEDGER_MAX_TX_SIZE;
    result.totalCoins = GENESIS_LEDGER_TOTAL_COINS;
    result.ledgerSeq = GENESIS_LEDGER_SEQ;
    return result;
}

void
LedgerManagerImpl::startNewLedger(LedgerHeader const& genesisLedger)
{
    auto ledgerTime = mLedgerClose.TimeScope();
    SecretKey skey = SecretKey::fromSeed(mApp.getNetworkID());

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
    ltx.loadHeader().current() = genesisLedger;

    LedgerEntry rootEntry;
    rootEntry.lastModifiedLedgerSeq = 1;
    rootEntry.data.type(ACCOUNT);
    auto& rootAccount = rootEntry.data.account();
    rootAccount.accountID = skey.getPublicKey();
    rootAccount.thresholds[0] = 1;
    rootAccount.balance = genesisLedger.totalCoins;
    ltx.create(rootEntry);

    CLOG_INFO(Ledger, "Established genesis ledger, closing");
    CLOG_INFO(Ledger, "Root account: {}", skey.getStrKeyPublic());
    CLOG_INFO(Ledger, "Root account seed: {}", skey.getStrKeySeed().value);
    ledgerClosed(ltx);
    ltx.commit();
}

void
LedgerManagerImpl::startNewLedger()
{
    auto ledger = genesisLedger();
    auto const& cfg = mApp.getConfig();
    if (cfg.USE_CONFIG_FOR_GENESIS)
    {
        ledger.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
        ledger.baseFee = cfg.TESTING_UPGRADE_DESIRED_FEE;
        ledger.baseReserve = cfg.TESTING_UPGRADE_RESERVE;
        ledger.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE;
    }

    startNewLedger(ledger);
}

void
LedgerManagerImpl::loadLastKnownLedger(
    function<void(asio::error_code const& ec)> handler)
{
    ZoneScoped;
    auto ledgerTime = mLedgerClose.TimeScope();

    string lastLedger =
        mApp.getPersistentState().getState(PersistentState::kLastClosedLedger);

    if (lastLedger.empty())
    {
        throw std::runtime_error(
            "No reference in DB to any last closed ledger");
    }
    else
    {
        CLOG_INFO(Ledger, "Last closed ledger (LCL) hash is {}", lastLedger);
        Hash lastLedgerHash = hexToBin256(lastLedger);

        if (mApp.getConfig().MODE_STORES_HISTORY_LEDGERHEADERS)
        {
            auto currentLedger =
                LedgerHeaderUtils::loadByHash(getDatabase(), lastLedgerHash);
            if (!currentLedger)
            {
                throw std::runtime_error("Could not load ledger from database");
            }
            CLOG_INFO(Ledger, "Loaded LCL header from database: {}",
                      ledgerAbbrev(*currentLedger));
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            ltx.loadHeader().current() = *currentLedger;
            ltx.commit();
        }
        else
        {
            // In no-history mode, this method should only be called when
            // the LCL is genesis.
            releaseAssertOrThrow(mLastClosedLedger.hash == lastLedgerHash);
            releaseAssertOrThrow(mLastClosedLedger.header.ledgerSeq ==
                                 GENESIS_LEDGER_SEQ);
            CLOG_INFO(Ledger, "LCL is genesis: {}",
                      ledgerAbbrev(mLastClosedLedger));
        }

        if (handler)
        {
            HistoryArchiveState has = getLastClosedLedgerHAS();

            auto continuation = [this, handler,
                                 has](asio::error_code const& ec) {
                if (ec)
                {
                    handler(ec);
                }
                else
                {
                    {
                        LedgerTxn ltx(mApp.getLedgerTxnRoot());
                        auto header = ltx.loadHeader();
                        if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
                        {
                            mApp.getBucketManager().assumeState(
                                has, header.current().ledgerVersion);
                            CLOG_INFO(Ledger,
                                      "Assumed bucket-state for LCL: {}",
                                      ledgerAbbrev(header.current()));
                        }
                        advanceLedgerPointers(header.current());
                    }
                    handler(ec);
                }
            };

            auto missing =
                mApp.getBucketManager().checkForMissingBucketsFiles(has);
            auto pubmissing = mApp.getHistoryManager()
                                  .getMissingBucketsReferencedByPublishQueue();
            missing.insert(missing.end(), pubmissing.begin(), pubmissing.end());
            if (!missing.empty())
            {
                CLOG_ERROR(
                    Ledger, "{} buckets are missing from bucket directory '{}'",
                    missing.size(), mApp.getBucketManager().getBucketDir());
                throw std::runtime_error("Bucket directory is corrupt");
            }
            else
            {
                continuation(asio::error_code());
            }
        }
        else
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            advanceLedgerPointers(ltx.loadHeader().current());
        }
    }
}

Database&
LedgerManagerImpl::getDatabase()
{
    return mApp.getDatabase();
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSize() const
{
    return mLastClosedLedger.header.maxTxSetSize;
}

uint32_t
LedgerManagerImpl::getLastMaxTxSetSizeOps() const
{
    auto n = mLastClosedLedger.header.maxTxSetSize;
    return mLastClosedLedger.header.ledgerVersion >= 11 ? n
                                                        : (n * MAX_OPS_PER_TX);
}

int64_t
LedgerManagerImpl::getLastMinBalance(uint32_t ownerCount) const
{
    auto& lh = mLastClosedLedger.header;
    if (lh.ledgerVersion <= 8)
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2LL + ownerCount) * int64_t(lh.baseReserve);
}

uint32_t
LedgerManagerImpl::getLastReserve() const
{
    return mLastClosedLedger.header.baseReserve;
}

uint32_t
LedgerManagerImpl::getLastTxFee() const
{
    return mLastClosedLedger.header.baseFee;
}

LedgerHeaderHistoryEntry const&
LedgerManagerImpl::getLastClosedLedgerHeader() const
{
    return mLastClosedLedger;
}

HistoryArchiveState
LedgerManagerImpl::getLastClosedLedgerHAS()
{
    string hasString = mApp.getPersistentState().getState(
        PersistentState::kHistoryArchiveState);
    HistoryArchiveState has;
    has.fromString(hasString);
    return has;
}

uint32_t
LedgerManagerImpl::getLastClosedLedgerNum() const
{
    return mLastClosedLedger.header.ledgerSeq;
}

// called by txherder
void
LedgerManagerImpl::valueExternalized(LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    CLOG_INFO(Ledger,
              "Got consensus: [seq={}, prev={}, txs={}, ops={}, sv: {}]",
              ledgerData.getLedgerSeq(),
              hexAbbrev(ledgerData.getTxSet()->previousLedgerHash()),
              ledgerData.getTxSet()->sizeTx(), ledgerData.getTxSet()->sizeOp(),
              stellarValueToString(mApp.getConfig(), ledgerData.getValue()));

    auto st = getState();
    if (st != LedgerManager::LM_BOOTING_STATE &&
        st != LedgerManager::LM_CATCHING_UP_STATE &&
        st != LedgerManager::LM_SYNCED_STATE)
    {
        assert(false);
    }

    closeLedgerIf(ledgerData);

    auto& cm = mApp.getCatchupManager();

    cm.processLedger(ledgerData);

    // Invariant: if catchup is running or waiting to run, buffered ledgers are
    // never empty
    if (!cm.hasBufferedLedger())
    {
        setState(LM_SYNCED_STATE);
    }

    FrameMark;
}

void
LedgerManagerImpl::closeLedgerIf(LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.getLedgerSeq())
    {
        auto& cm = mApp.getCatchupManager();
        // if catchup work is running, we don't want ledger manager to close
        // this ledger and potentially cause issues.
        if (cm.isCatchupInitialized() && !cm.catchupWorkIsDone())
        {
            CLOG_INFO(
                Ledger,
                "Can't close ledger: {}  in LM because catchup is running",
                ledgerAbbrev(mLastClosedLedger));
            return;
        }

        closeLedger(ledgerData);
        CLOG_INFO(Ledger, "Closed ledger: {}", ledgerAbbrev(mLastClosedLedger));
    }
    else if (ledgerData.getLedgerSeq() <= mLastClosedLedger.header.ledgerSeq)
    {
        CLOG_INFO(
            Ledger,
            "Skipping close ledger: local state is {}, more recent than {}",
            mLastClosedLedger.header.ledgerSeq, ledgerData.getLedgerSeq());
    }
    else
    {
        if (mState != LM_CATCHING_UP_STATE)
        {
            // Out of sync, buffer what we just heard and start catchup.
            CLOG_INFO(
                Ledger, "Lost sync, local LCL is {}, network closed ledger {}",
                mLastClosedLedger.header.ledgerSeq, ledgerData.getLedgerSeq());
        }

        setState(LM_CATCHING_UP_STATE);
    }
}

void
LedgerManagerImpl::startCatchup(CatchupConfiguration configuration,
                                std::shared_ptr<HistoryArchive> archive)
{
    ZoneScoped;
    setState(LM_CATCHING_UP_STATE);
    mApp.getCatchupManager().startCatchup(configuration, archive);
}

uint64_t
LedgerManagerImpl::secondsSinceLastLedgerClose() const
{
    uint64_t ct = getLastClosedLedgerHeader().header.scpValue.closeTime;
    uint64_t now = mApp.timeNow();
    return (now > ct) ? (now - ct) : 0;
}

void
LedgerManagerImpl::syncMetrics()
{
    mLedgerAge.set_count(secondsSinceLastLedgerClose());
    mApp.syncOwnMetrics();
}

void
LedgerManagerImpl::emitNextMeta()
{
    releaseAssert(mNextMetaToEmit);
    releaseAssert(mMetaStream);
    auto streamWrite = mMetaStreamWriteTime.TimeScope();
    mMetaStream->writeOne(*mNextMetaToEmit);
    mMetaStream->flush();
    mNextMetaToEmit.reset();
}

/*
    This is the main method that closes the current ledger based on
the close context that was computed by SCP or by the historical module
during replays.

*/
void
LedgerManagerImpl::closeLedger(LedgerCloseData const& ledgerData)
{
    ZoneScoped;
    auto ledgerTime = mLedgerClose.TimeScope();
    LogSlowExecution closeLedgerTime{"closeLedger",
                                     LogSlowExecution::Mode::MANUAL, "",
                                     std::chrono::milliseconds::max()};

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    ++header.current().ledgerSeq;
    header.current().previousLedgerHash = mLastClosedLedger.hash;
    CLOG_DEBUG(Ledger, "starting closeLedger() on ledgerSeq={}",
               header.current().ledgerSeq);

    ZoneValue(static_cast<int64_t>(header.current().ledgerSeq));

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
    mLastClose = now;
    mLedgerAge.set_count(0);

    std::shared_ptr<AbstractTxSetFrameForApply> txSet = ledgerData.getTxSet();

    // If we do not support ledger version, we can't apply that ledger, fail!
    if (header.current().ledgerVersion >
        mApp.getConfig().LEDGER_PROTOCOL_VERSION)
    {
        CLOG_ERROR(Ledger, "Unknown ledger version: {}",
                   header.current().ledgerVersion);
        CLOG_ERROR(Ledger, "{}", UPGRADE_STELLAR_CORE);
        throw std::runtime_error(
            fmt::format("cannot apply ledger with not supported version: {}",
                        header.current().ledgerVersion));
    }

    if (txSet->previousLedgerHash() != getLastClosedLedgerHeader().hash)
    {
        CLOG_ERROR(Ledger, "TxSet mismatch: LCD wants {}, LCL is {}",
                   ledgerAbbrev(ledgerData.getLedgerSeq() - 1,
                                txSet->previousLedgerHash()),
                   ledgerAbbrev(getLastClosedLedgerHeader()));

        CLOG_ERROR(Ledger, "{}",
                   xdr_to_string(getLastClosedLedgerHeader(), "Full LCL"));
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_LOCAL_DATA);

        throw std::runtime_error("txset mismatch");
    }

    if (txSet->getContentsHash() != ledgerData.getValue().txSetHash)
    {
        CLOG_ERROR(
            Ledger,
            "Corrupt transaction set: TxSet hash is {}, SCP value reports {}",
            txSet->getContentsHash(), ledgerData.getValue().txSetHash);
        CLOG_ERROR(Ledger, "{}", POSSIBLY_CORRUPTED_QUORUM_SET);

        throw std::runtime_error("corrupt transaction set");
    }

    auto const& sv = ledgerData.getValue();
    header.current().scpValue = sv;

    // In addition to the _canonical_ LedgerResultSet hashed into the
    // LedgerHeader, we optionally collect an even-more-fine-grained record of
    // the ledger entries modified by each tx during tx processing in a
    // LedgerCloseMeta, for streaming to attached clients (typically: horizon).
    std::unique_ptr<LedgerCloseMeta> ledgerCloseMeta;
    if (mMetaStream)
    {
        if (mNextMetaToEmit)
        {
            releaseAssert(mNextMetaToEmit->v0().ledgerHeader.hash ==
                          getLastClosedLedgerHeader().hash);
            emitNextMeta();
        }
        releaseAssert(!mNextMetaToEmit);
        // Write to a local variable rather than a member variable first: this
        // enables us to discard incomplete meta and retry, should anything in
        // this method throw.
        ledgerCloseMeta = std::make_unique<LedgerCloseMeta>();
        ledgerCloseMeta->v0().txProcessing.reserve(txSet->sizeTx());
        txSet->toXDR(ledgerCloseMeta->v0().txSet);
    }

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    vector<TransactionFrameBasePtr> txs = ledgerData.getTxSet()->sortForApply();

    // first, prefetch source accounts for txset, then charge fees
    prefetchTxSourceIds(txs);
    processFeesSeqNums(txs, ltx, txSet->getBaseFee(header.current()),
                       ledgerCloseMeta);

    TransactionResultSet txResultSet;
    txResultSet.results.reserve(txs.size());
    applyTransactions(txs, ltx, txResultSet, ledgerCloseMeta);

    ltx.loadHeader().current().txSetResultHash = xdrSha256(txResultSet);

    // apply any upgrades that were decided during consensus
    // this must be done after applying transactions as the txset
    // was validated before upgrades
    for (size_t i = 0; i < sv.upgrades.size(); i++)
    {
        LedgerUpgrade lupgrade;
        auto valid = Upgrades::isValidForApply(
            sv.upgrades[i], lupgrade, ltx.loadHeader().current(),
            mApp.getConfig().LEDGER_PROTOCOL_VERSION);
        switch (valid)
        {
        case Upgrades::UpgradeValidity::VALID:
            break;
        case Upgrades::UpgradeValidity::XDR_INVALID:
            throw std::runtime_error(
                fmt::format(FMT_STRING("Unknown upgrade at index {}"), i));
        case Upgrades::UpgradeValidity::INVALID:
            throw std::runtime_error(
                fmt::format(FMT_STRING("Invalid upgrade at index {}: {}"), i,
                            xdr_to_string(lupgrade, "LedgerUpgrade")));
        }

        try
        {
            LedgerTxn ltxUpgrade(ltx);
            Upgrades::applyTo(lupgrade, ltxUpgrade);

            auto ledgerSeq = ltxUpgrade.loadHeader().current().ledgerSeq;
            LedgerEntryChanges changes = ltxUpgrade.getChanges();
            if (ledgerCloseMeta)
            {
                auto& up = ledgerCloseMeta->v0().upgradesProcessing;
                up.emplace_back();
                UpgradeEntryMeta& uem = up.back();
                uem.upgrade = lupgrade;
                uem.changes = changes;
            }
            // Note: Index from 1 rather than 0 to match the behavior of
            // storeTransaction and storeTransactionFee.
            if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
            {
                Upgrades::storeUpgradeHistory(getDatabase(), ledgerSeq,
                                              lupgrade, changes,
                                              static_cast<int>(i + 1));
            }
            ltxUpgrade.commit();
        }
        catch (std::runtime_error& e)
        {
            CLOG_ERROR(Ledger, "Exception during upgrade: {}", e.what());
        }
        catch (...)
        {
            CLOG_ERROR(Ledger, "Unknown exception during upgrade");
        }
    }

    ledgerClosed(ltx);

    if (ledgerData.getExpectedHash() &&
        *ledgerData.getExpectedHash() != mLastClosedLedger.hash)
    {
        throw std::runtime_error("Local node's ledger corrupted during close");
    }

    if (mMetaStream)
    {
        releaseAssert(ledgerCloseMeta);
        ledgerCloseMeta->v0().ledgerHeader = mLastClosedLedger;

        // At this point we've got a complete meta and we can store it to the
        // member variable: if we throw while committing below, we will at worst
        // emit duplicate meta, when retrying.
        mNextMetaToEmit = std::move(ledgerCloseMeta);

        // If the LedgerCloseData provided an expected hash, then we validated
        // it above.
        if (!mApp.getConfig().EXPERIMENTAL_PRECAUTION_DELAY_META ||
            ledgerData.getExpectedHash())
        {
            emitNextMeta();
        }
    }

    // The next 4 steps happen in a relatively non-obvious, subtle order.
    // This is unfortunate and it would be nice if we could make it not
    // be so subtle, but for the time being this is where we are.
    //
    // 1. Queue any history-checkpoint to the database, _within_ the current
    //    transaction. This way if there's a crash after commit and before
    //    we've published successfully, we'll re-publish on restart.
    //
    // 2. Commit the current transaction.
    //
    // 3. Start any queued checkpoint publishing, _after_ the commit so that
    //    it takes its snapshot of history-rows from the committed state, but
    //    _before_ we GC any buckets (because this is the step where the
    //    bucket refcounts are incremented for the duration of the publish).
    //
    // 4. GC unreferenced buckets. Only do this once publishes are in progress.

    // step 1
    auto& hm = mApp.getHistoryManager();
    hm.maybeQueueHistoryCheckpoint();

    // step 2
    ltx.commit();

    // step 3
    hm.publishQueuedHistory();
    hm.logAndUpdatePublishStatus();

    // step 4
    mApp.getBucketManager().forgetUnreferencedBuckets();

    if (!mApp.getConfig().getOpApplySleepTimeForTesting().empty())
    {
        // Sleep for a parameterized amount of time in simulation mode
        std::chrono::microseconds sleepFor{0};
        for (int i = 0; i < txSet->sizeOp(); i++)
        {
            sleepFor +=
                rand_element(mApp.getConfig().getOpApplySleepTimeForTesting());
        }
        std::chrono::microseconds applicationTime =
            closeLedgerTime.checkElapsedTime();
        if (applicationTime < sleepFor)
        {
            sleepFor -= applicationTime;
            CLOG_DEBUG(Perf, "Simulate application: sleep for {} microseconds",
                       sleepFor.count());
            std::this_thread::sleep_for(sleepFor);
        }
    }

    std::chrono::duration<double> ledgerTimeSeconds = ledgerTime.Stop();
    CLOG_DEBUG(Perf, "Applied ledger in {} seconds", ledgerTimeSeconds.count());
}

void
LedgerManagerImpl::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    ZoneScoped;
    soci::transaction txscope(db.getSession());
    db.clearPreparedStatementCache();
    LedgerHeaderUtils::deleteOldEntries(db, ledgerSeq, count);
    deleteOldTransactionHistoryEntries(db, ledgerSeq, count);
    HerderPersistence::deleteOldEntries(db, ledgerSeq, count);
    Upgrades::deleteOldEntries(db, ledgerSeq, count);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::deleteNewerEntries(Database& db, uint32_t ledgerSeq)
{
    ZoneScoped;
    soci::transaction txscope(db.getSession());
    db.clearPreparedStatementCache();

    // as we use this method only when we apply buckets, we have to preserve
    // data for everything but ledger header
    LedgerHeaderUtils::deleteNewerEntries(db, ledgerSeq);
    // for other data we delete data *after*
    ++ledgerSeq;
    deleteNewerTransactionHistoryEntries(db, ledgerSeq);
    HerderPersistence::deleteNewerEntries(db, ledgerSeq);
    Upgrades::deleteNewerEntries(db, ledgerSeq);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::setLastClosedLedger(
    LedgerHeaderHistoryEntry const& lastClosed)
{
    ZoneScoped;
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    header.current() = lastClosed.header;
    storeCurrentLedger(header.current());
    ltx.commit();

    advanceLedgerPointers(lastClosed.header);
}

void
LedgerManagerImpl::manuallyAdvanceLedgerHeader(LedgerHeader const& header)
{
    if (!mApp.getConfig().MANUAL_CLOSE || !mApp.getConfig().RUN_STANDALONE)
    {
        throw std::logic_error(
            "May only manually advance ledger header sequence number with "
            "MANUAL_CLOSE and RUN_STANDALONE");
    }
    advanceLedgerPointers(header, false);
}

void
LedgerManagerImpl::setupLedgerCloseMetaStream()
{
    if (mMetaStream)
    {
        throw std::runtime_error("LedgerManagerImpl already streaming");
    }
    auto& cfg = mApp.getConfig();
    if (cfg.METADATA_OUTPUT_STREAM != "")
    {
        // We can't be sure we're writing to a stream that supports fsync;
        // pipes typically error when you try. So we don't do it.
        mMetaStream = std::make_unique<XDROutputFileStream>(
            mApp.getClock().getIOContext(),
            /*fsyncOnClose=*/false);
        std::regex fdrx("^fd:([0-9]+)$");
        std::smatch sm;
        if (std::regex_match(cfg.METADATA_OUTPUT_STREAM, sm, fdrx))
        {
            int fd = std::stoi(sm[1]);
            CLOG_INFO(Ledger, "Streaming metadata to file descriptor {}", fd);
            mMetaStream->fdopen(fd);
        }
        else
        {
            CLOG_INFO(Ledger, "Streaming metadata to '{}'",
                      cfg.METADATA_OUTPUT_STREAM);
            mMetaStream->open(cfg.METADATA_OUTPUT_STREAM);
        }
    }
}

void
LedgerManagerImpl::advanceLedgerPointers(LedgerHeader const& header,
                                         bool debugLog)
{
    auto ledgerHash = xdrSha256(header);

    if (debugLog)
    {
        CLOG_DEBUG(Ledger, "Advancing LCL: {} -> {}",
                   ledgerAbbrev(mLastClosedLedger),
                   ledgerAbbrev(header, ledgerHash));
    }

    mLastClosedLedger.hash = ledgerHash;
    mLastClosedLedger.header = header;
}

void
LedgerManagerImpl::processFeesSeqNums(
    std::vector<TransactionFrameBasePtr>& txs, AbstractLedgerTxn& ltxOuter,
    int64_t baseFee, std::unique_ptr<LedgerCloseMeta> const& ledgerCloseMeta)
{
    ZoneScoped;
    CLOG_DEBUG(Ledger, "processing fees and sequence numbers with base fee {}",
               baseFee);
    int index = 0;
    try
    {
        LedgerTxn ltx(ltxOuter);
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        for (auto tx : txs)
        {
            LedgerTxn ltxTx(ltx);
            tx->processFeeSeqNum(ltxTx, baseFee);
            LedgerEntryChanges changes = ltxTx.getChanges();
            if (ledgerCloseMeta)
            {
                auto& tp = ledgerCloseMeta->v0().txProcessing;
                tp.emplace_back();
                tp.back().feeProcessing = changes;
            }
            // Note to future: when we eliminate the txhistory and txfeehistory
            // tables, the following step can be removed.
            //
            // Also note: for historical reasons the history tables number
            // txs counting from 1, not 0. We preserve this for the time being
            // in case anyone depends on it.
            ++index;
            if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
            {
                storeTransactionFee(mApp.getDatabase(), ledgerSeq, tx, changes,
                                    index);
            }
            ltxTx.commit();
        }
        ltx.commit();
    }
    catch (std::exception& e)
    {
        CLOG_FATAL(Ledger, "processFeesSeqNums error @ {} : {}", index,
                   e.what());
        CLOG_FATAL(Ledger, "{}", REPORT_INTERNAL_BUG);
        throw;
    }
}

void
LedgerManagerImpl::prefetchTxSourceIds(
    std::vector<TransactionFrameBasePtr>& txs)
{
    ZoneScoped;
    if (mApp.getConfig().PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& tx : txs)
        {
            tx->insertKeysForFeeProcessing(keys);
        }
        mApp.getLedgerTxnRoot().prefetch(keys);
    }
}

void
LedgerManagerImpl::prefetchTransactionData(
    std::vector<TransactionFrameBasePtr>& txs)
{
    ZoneScoped;
    if (mApp.getConfig().PREFETCH_BATCH_SIZE > 0)
    {
        UnorderedSet<LedgerKey> keys;
        for (auto const& tx : txs)
        {
            tx->insertKeysForTxApply(keys);
        }
        mApp.getLedgerTxnRoot().prefetch(keys);
    }
}

void
LedgerManagerImpl::applyTransactions(
    std::vector<TransactionFrameBasePtr>& txs, AbstractLedgerTxn& ltx,
    TransactionResultSet& txResultSet,
    std::unique_ptr<LedgerCloseMeta> const& ledgerCloseMeta)
{
    ZoneNamedN(txsZone, "applyTransactions", true);
    int index = 0;

    // Record counts
    auto numTxs = txs.size();
    size_t numOps = 0;
    if (numTxs > 0)
    {
        mTransactionCount.Update(static_cast<int64_t>(numTxs));
        TracyPlot("ledger.transaction.count", static_cast<int64_t>(numTxs));
        numOps =
            std::accumulate(txs.begin(), txs.end(), size_t(0),
                            [](size_t s, TransactionFrameBasePtr const& v) {
                                return s + v->getNumOperations();
                            });
        mOperationCount.Update(static_cast<int64_t>(numOps));
        TracyPlot("ledger.operation.count", static_cast<int64_t>(numOps));
        CLOG_INFO(Tx, "applying ledger {} (txs:{}, ops:{})",
                  ltx.loadHeader().current().ledgerSeq, numTxs, numOps);
    }

    prefetchTransactionData(txs);

    for (auto tx : txs)
    {
        ZoneNamedN(txZone, "applyTransaction", true);
        auto txTime = mTransactionApply.TimeScope();
        TransactionMeta tm(2);
        CLOG_DEBUG(Tx, " tx#{} = {} ops={} txseq={} (@ {})", index,
                   hexAbbrev(tx->getContentsHash()), tx->getNumOperations(),
                   tx->getSeqNum(),
                   mApp.getConfig().toShortString(tx->getSourceID()));
        tx->apply(mApp, ltx, tm);

        TransactionResultPair results;
        results.transactionHash = tx->getContentsHash();
        results.result = tx->getResult();

        // First gather the TransactionResultPair into the TxResultSet for
        // hashing into the ledger header.
        txResultSet.results.emplace_back(results);

        // Then potentially add that TRP and its associated TransactionMeta
        // into the associated slot of any LedgerCloseMeta we're collecting.
        if (ledgerCloseMeta)
        {
            TransactionResultMeta& trm =
                ledgerCloseMeta->v0().txProcessing.at(index);
            trm.txApplyProcessing = tm;
            trm.result = results;
        }

        // Then finally store the results and meta into the txhistory table.
        // if we're running in a mode that has one.
        //
        // Note to future: when we eliminate the txhistory and txfeehistory
        // tables, the following step can be removed.
        //
        // Also note: for historical reasons the history tables number
        // txs counting from 1, not 0. We preserve this for the time being
        // in case anyone depends on it.
        ++index;
        if (mApp.getConfig().MODE_STORES_HISTORY_MISC)
        {
            auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
            storeTransaction(mApp.getDatabase(), ledgerSeq, tx, tm,
                             txResultSet);
        }
    }

    logTxApplyMetrics(ltx, numTxs, numOps);
}

void
LedgerManagerImpl::logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                                     size_t numOps)
{
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto hitRate = mApp.getLedgerTxnRoot().getPrefetchHitRate() * 100;

    CLOG_DEBUG(Ledger, "Ledger: {} txs: {}, ops: {}, prefetch hit rate (%): {}",
               ledgerSeq, numTxs, numOps, hitRate);

    // We lose a bit of precision here, as medida only accepts int64_t
    mPrefetchHitRate.Update(std::llround(hitRate));
    TracyPlot("ledger.prefetch.hit-rate", hitRate);
}

void
LedgerManagerImpl::storeCurrentLedger(LedgerHeader const& header)
{
    ZoneScoped;
    if (mApp.getConfig().MODE_STORES_HISTORY_LEDGERHEADERS)
    {
        LedgerHeaderUtils::storeInDatabase(mApp.getDatabase(), header);
    }

    Hash hash = xdrSha256(header);
    assert(!isZero(hash));
    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(hash));

    BucketList bl;
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        bl = mApp.getBucketManager().getBucketList();
    }
    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(header.ledgerSeq, bl,
                            mApp.getConfig().NETWORK_PASSPHRASE);

    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString());
}

// NB: This is a separate method so a testing subclass can override it.
void
LedgerManagerImpl::transferLedgerEntriesToBucketList(AbstractLedgerTxn& ltx,
                                                     uint32_t ledgerSeq,
                                                     uint32_t ledgerVers)
{
    ZoneScoped;
    std::vector<LedgerEntry> initEntries, liveEntries;
    std::vector<LedgerKey> deadEntries;
    ltx.getAllEntries(initEntries, liveEntries, deadEntries);
    if (mApp.getConfig().MODE_ENABLES_BUCKETLIST)
    {
        mApp.getBucketManager().addBatch(mApp, ledgerSeq, ledgerVers,
                                         initEntries, liveEntries, deadEntries);
    }
}

void
LedgerManagerImpl::ledgerClosed(AbstractLedgerTxn& ltx)
{
    ZoneScoped;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto ledgerVers = ltx.loadHeader().current().ledgerVersion;
    CLOG_TRACE(Ledger,
               "sealing ledger {} with version {}, sending to bucket list",
               ledgerSeq, ledgerVers);

    transferLedgerEntriesToBucketList(ltx, ledgerSeq, ledgerVers);

    ltx.unsealHeader([this](LedgerHeader& lh) {
        mApp.getBucketManager().snapshotLedger(lh);
        storeCurrentLedger(lh);
        advanceLedgerPointers(lh);
    });
}
}
