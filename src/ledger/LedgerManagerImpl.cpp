// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
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
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerRange.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/format.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"

#include <chrono>
#include <numeric>
#include <sstream>

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
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
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
    return ledgerAbbrev(header, sha256(xdr::xdr_to_opaque(header)));
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
LedgerManager::ledgerAbbrev(LedgerHeaderHistoryEntry he)
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
    , mInternalErrorCount(app.getMetrics().NewCounter(
          {"ledger", "transaction", "internal-error"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
    , mLedgerAgeClosed(app.getMetrics().NewTimer({"ledger", "age", "closed"}))
    , mLedgerAge(
          app.getMetrics().NewCounter({"ledger", "age", "current-seconds"}))
    , mLastClose(mApp.getClock().now())
    , mSyncingLedgersSize(
          app.getMetrics().NewCounter({"ledger", "memory", "queued-ledgers"}))
    , mState(LM_BOOTING_STATE)

{
}

void
LedgerManagerImpl::bootstrap()
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
        CLOG(INFO, "Ledger")
            << "Changing state " << oldState << " -> " << getStateHuman();
        if (mState != LM_CATCHING_UP_STATE)
        {
            mCatchupState = CatchupState::NONE;
            mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
        }
    }
}

void
LedgerManagerImpl::setCatchupState(CatchupState s)
{
    mCatchupState = s;
    setState(LM_CATCHING_UP_STATE);
}

LedgerManager::State
LedgerManagerImpl::getState() const
{
    return mState;
}

LedgerManager::CatchupState
LedgerManagerImpl::getCatchupState() const
{
    return mCatchupState;
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
LedgerManagerImpl::startNewLedger(LedgerHeader genesisLedger)
{
    DBTimeExcluder qtExclude(mApp);
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

    CLOG(INFO, "Ledger") << "Established genesis ledger, closing";
    CLOG(INFO, "Ledger") << "Root account seed: " << skey.getStrKeySeed().value;
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
        ledger.maxTxSetSize = cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER;
    }

    startNewLedger(std::move(ledger));
}

void
LedgerManagerImpl::loadLastKnownLedger(
    function<void(asio::error_code const& ec)> handler)
{
    DBTimeExcluder qtExclude(mApp);
    auto ledgerTime = mLedgerClose.TimeScope();

    string lastLedger =
        mApp.getPersistentState().getState(PersistentState::kLastClosedLedger);

    if (lastLedger.empty())
    {
        throw std::runtime_error("No ledger in the DB");
    }
    else
    {
        LOG(INFO) << "Loading last known ledger";
        Hash lastLedgerHash = hexToBin256(lastLedger);

        auto currentLedger =
            LedgerHeaderUtils::loadByHash(getDatabase(), lastLedgerHash);
        if (!currentLedger)
        {
            throw std::runtime_error("Could not load ledger from database");
        }

        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            ltx.loadHeader().current() = *currentLedger;
            ltx.commit();
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
                    mApp.getBucketManager().assumeState(has);
                    {
                        LedgerTxn ltx(mApp.getLedgerTxnRoot());
                        auto header = ltx.loadHeader();
                        CLOG(INFO, "Ledger") << "Loaded last known ledger: "
                                             << ledgerAbbrev(header.current());
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
                CLOG(WARNING, "Ledger")
                    << "Some buckets are missing in '"
                    << mApp.getBucketManager().getBucketDir() << "'.";
                CLOG(WARNING, "Ledger")
                    << "Attempting to recover from the history store.";
                mApp.getHistoryManager().downloadMissingBuckets(has,
                                                                continuation);
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

int64_t
LedgerManagerImpl::getLastMinBalance(uint32_t ownerCount) const
{
    auto& lh = mLastClosedLedger.header;
    if (lh.ledgerVersion <= 8)
        return (2 + ownerCount) * lh.baseReserve;
    else
        return (2 + ownerCount) * int64_t(lh.baseReserve);
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

uint32_t
getCatchupCount(Application& app)
{
    return app.getConfig().CATCHUP_COMPLETE
               ? std::numeric_limits<uint32_t>::max()
               : app.getConfig().CATCHUP_RECENT;
}

// called by txherder
void
LedgerManagerImpl::valueExternalized(LedgerCloseData const& ledgerData)
{
    CLOG(INFO, "Ledger")
        << "Got consensus: "
        << "[seq=" << ledgerData.getLedgerSeq()
        << ", prev=" << hexAbbrev(ledgerData.getTxSet()->previousLedgerHash())
        << ", tx_count=" << ledgerData.getTxSet()->size()
        << ", sv: " << stellarValueToString(ledgerData.getValue()) << "]";

    auto st = getState();
    switch (st)
    {
    case LedgerManager::LM_BOOTING_STATE:
    case LedgerManager::LM_SYNCED_STATE:
    {
        switch (closeLedgerIf(ledgerData))
        {
        case CloseLedgerIfResult::CLOSED:
        {
            setState(LM_SYNCED_STATE);
        }
        break;
        case CloseLedgerIfResult::TOO_OLD:
            // nothing to do
            break;
        case CloseLedgerIfResult::TOO_NEW:
        {
            initializeCatchup(ledgerData);
        }
        break;
        }
    }
    break;

    case LedgerManager::LM_CATCHING_UP_STATE:
    {
        switch (mCatchupState)
        {
        case CatchupState::WAITING_FOR_CLOSING_LEDGER:
        {
            finalizeCatchup(ledgerData);
        }
        break;

        default:
        {
            continueCatchup(ledgerData);
        }
        break;
        }
    }
    break;

    default:
        assert(false);
    }
}

LedgerManagerImpl::CloseLedgerIfResult
LedgerManagerImpl::closeLedgerIf(LedgerCloseData const& ledgerData)
{
    if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.getLedgerSeq())
    {
        if (mLastClosedLedger.hash ==
            ledgerData.getTxSet()->previousLedgerHash())
        {
            closeLedger(ledgerData);
            CLOG(INFO, "Ledger")
                << "Closed ledger: " << ledgerAbbrev(mLastClosedLedger);
            return CloseLedgerIfResult::CLOSED;
        }
        else
        {
            CLOG(FATAL, "Ledger") << "Network consensus for ledger "
                                  << mLastClosedLedger.header.ledgerSeq
                                  << " changed; this should never happen";
            throw std::runtime_error("Network consensus inconsistency");
        }
    }
    else if (ledgerData.getLedgerSeq() <= mLastClosedLedger.header.ledgerSeq)
    {
        CLOG(INFO, "Ledger")
            << "Skipping close ledger: local state is "
            << mLastClosedLedger.header.ledgerSeq << ", more recent than "
            << ledgerData.getLedgerSeq();
        return CloseLedgerIfResult::TOO_OLD;
    }
    else
    {
        // Out of sync, buffer what we just heard and start catchup.
        CLOG(INFO, "Ledger")
            << "Lost sync, local LCL is " << mLastClosedLedger.header.ledgerSeq
            << ", network closed ledger " << ledgerData.getLedgerSeq();
        return CloseLedgerIfResult::TOO_NEW;
    }
}

void
LedgerManagerImpl::initializeCatchup(LedgerCloseData const& ledgerData)
{
    assert(mState != LM_CATCHING_UP_STATE);
    assert(mCatchupState == CatchupState::NONE);
    assert(mSyncingLedgers.empty());

    setState(LM_CATCHING_UP_STATE);
    mCatchupTriggerLedger = mApp.getHistoryManager().nextCheckpointLedger(
                                ledgerData.getLedgerSeq()) +
                            1;
    setCatchupState(CatchupState::WAITING_FOR_TRIGGER_LEDGER);
    addToSyncingLedgers(ledgerData);
    startCatchupIf(ledgerData.getLedgerSeq());
}

void
LedgerManagerImpl::continueCatchup(LedgerCloseData const& ledgerData)
{
    assert(mState == LM_CATCHING_UP_STATE);

    addToSyncingLedgers(ledgerData);
    startCatchupIf(ledgerData.getLedgerSeq());
}

void
LedgerManagerImpl::finalizeCatchup(LedgerCloseData const& ledgerData)
{
    assert(mState == LM_CATCHING_UP_STATE);
    assert(mCatchupState == CatchupState::WAITING_FOR_CLOSING_LEDGER);
    assert(mSyncingLedgers.empty());

    switch (closeLedgerIf(ledgerData))
    {
    case CloseLedgerIfResult::CLOSED:
    {
        CLOG(INFO, "Ledger") << "Catchup final ledger closed: "
                             << ledgerAbbrev(mLastClosedLedger);
        setState(LM_SYNCED_STATE);
    }
    break;
    case CloseLedgerIfResult::TOO_OLD:
        // nothing to do
        break;
    case CloseLedgerIfResult::TOO_NEW:
    {
        setState(LM_BOOTING_STATE);
        initializeCatchup(ledgerData);
    }
    break;
    }
}

void
LedgerManagerImpl::addToSyncingLedgers(LedgerCloseData const& ledgerData)
{
    switch (mSyncingLedgers.push(ledgerData))
    {
    case SyncingLedgerChainAddResult::CONTIGUOUS:
        // Normal close while catching up
        CLOG(INFO, "Ledger")
            << "Close of ledger " << ledgerData.getLedgerSeq() << " buffered";
        return;
    case SyncingLedgerChainAddResult::TOO_OLD:
        CLOG(INFO, "Ledger")
            << "Skipping close ledger: latest known is "
            << mSyncingLedgers.back().getLedgerSeq() << ", more recent than "
            << ledgerData.getLedgerSeq();
        return;
    case SyncingLedgerChainAddResult::TOO_NEW:
        // Out-of-order close while catching up; timeout / network failure?
        CLOG(WARNING, "Ledger")
            << "Out-of-order close during catchup, buffered to "
            << mSyncingLedgers.back().getLedgerSeq() << " but network closed "
            << ledgerData.getLedgerSeq();

        CLOG(WARNING, "Ledger")
            << "this round of catchup will fail and restart.";
        return;
    default:
        assert(false);
    }
}

void
LedgerManagerImpl::startCatchupIf(uint32_t lastReceivedLedgerSeq)
{
    assert(!mSyncingLedgers.empty());
    assert(mCatchupState != CatchupState::NONE);

    auto contiguous =
        lastReceivedLedgerSeq == mSyncingLedgers.back().getLedgerSeq();
    if (mCatchupState != CatchupState::WAITING_FOR_TRIGGER_LEDGER)
    {
        mApp.getCatchupManager().logAndUpdateCatchupStatus(contiguous);
        return;
    }

    if (lastReceivedLedgerSeq >= mCatchupTriggerLedger)
    {
        setCatchupState(CatchupState::APPLYING_HISTORY);

        auto message = fmt::format("Starting catchup after ensuring checkpoint "
                                   "ledger {} was closed on network",
                                   mCatchupTriggerLedger);
        mApp.getCatchupManager().logAndUpdateCatchupStatus(contiguous, message);

        // catchup just before first buffered ledger that way we will have a way
        // to verify history consistency - compare previousLedgerHash of
        // buffered ledger with last one downloaded from history
        auto firstBufferedLedgerSeq = mSyncingLedgers.front().getLedgerSeq();
        auto hash = make_optional<Hash>(
            mSyncingLedgers.front().getTxSet()->previousLedgerHash());
        startCatchup({LedgerNumHashPair(firstBufferedLedgerSeq - 1, hash),
                      getCatchupCount(mApp)},
                     false);
    }
    else
    {
        auto eta = (mCatchupTriggerLedger - lastReceivedLedgerSeq) *
                   mApp.getConfig().getExpectedLedgerCloseTime();
        auto message = fmt::format(
            "Waiting for trigger ledger: {}/{}, ETA: {}s",
            lastReceivedLedgerSeq, mCatchupTriggerLedger, eta.count());
        mApp.getCatchupManager().logAndUpdateCatchupStatus(contiguous, message);
    }
}

void
LedgerManagerImpl::startCatchup(CatchupConfiguration configuration,
                                bool manualCatchup)
{
    auto lastClosedLedger = getLastClosedLedgerNum();
    if ((configuration.toLedger() != CatchupConfiguration::CURRENT) &&
        (configuration.toLedger() <= lastClosedLedger))
    {
        throw std::invalid_argument("Target ledger is not newer than LCL");
    }

    setCatchupState(CatchupState::APPLYING_HISTORY);
    assert(manualCatchup == mSyncingLedgers.empty());

    mApp.getCatchupManager().catchupHistory(
        configuration,
        std::bind(&LedgerManagerImpl::historyCaughtup, this, _1, _2, _3));
}

void
LedgerManagerImpl::historyCaughtup(asio::error_code const& ec,
                                   CatchupWork::ProgressState progressState,
                                   LedgerHeaderHistoryEntry const& lastClosed)
{
    assert(mCatchupState == CatchupState::APPLYING_HISTORY);

    if (ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up: " << ec.message();
        CLOG(ERROR, "Ledger") << "Catchup will restart at next close.";
        setState(LM_BOOTING_STATE);
        mApp.getCatchupManager().historyCaughtup();
        mSyncingLedgers = {};
        mSyncingLedgersSize.set_count(mSyncingLedgers.size());
    }
    else
    {
        switch (progressState)
        {
        case CatchupWork::ProgressState::APPLIED_BUCKETS:
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            auto header = ltx.loadHeader();
            header.current() = lastClosed.header;
            storeCurrentLedger(header.current());
            ltx.commit();

            mLastClosedLedger = lastClosed;
            return;
        }
        case CatchupWork::ProgressState::APPLIED_TRANSACTIONS:
        {
            // In this case we should actually have been caught-up during the
            // replay process and, if judged successful, our LCL should be the
            // one provided as well.
            assert(lastClosed.hash == mLastClosedLedger.hash);
            assert(lastClosed.header == mLastClosedLedger.header);
            return;
        }
        case CatchupWork::ProgressState::FINISHED:
        {
            break;
        }
        default:
        {
            assert(false);
        }
        }

        CLOG(INFO, "Ledger") << "Caught up to LCL from history: "
                             << ledgerAbbrev(mLastClosedLedger);
        mApp.getCatchupManager().historyCaughtup();

        setCatchupState(CatchupState::APPLYING_BUFFERED_LEDGERS);
        applyBufferedLedgers();
    }
}

void
LedgerManagerImpl::applyBufferedLedgers()
{
    assert(mCatchupState == CatchupState::APPLYING_BUFFERED_LEDGERS);

    mApp.postOnMainThreadWithDelay(
        [&] {
            if (mSyncingLedgers.empty())
            {
                CLOG(INFO, "Ledger")
                    << "Caught up to LCL including recent network activity: "
                    << ledgerAbbrev(mLastClosedLedger)
                    << "; waiting for closing ledger";
                setCatchupState(CatchupState::WAITING_FOR_CLOSING_LEDGER);
                return;
            }

            auto lcd = mSyncingLedgers.front();
            mSyncingLedgers.pop();
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());

            assert(lcd.getLedgerSeq() ==
                   mLastClosedLedger.header.ledgerSeq + 1);
            CLOG(INFO, "Ledger")
                << "Replaying buffered ledger-close: "
                << "[seq=" << lcd.getLedgerSeq()
                << ", prev=" << hexAbbrev(lcd.getTxSet()->previousLedgerHash())
                << ", tx_count=" << lcd.getTxSet()->size()
                << ", sv: " << stellarValueToString(lcd.getValue()) << "]";
            closeLedger(lcd);

            applyBufferedLedgers();
        },
        "LedgerManager: applyBufferedLedgers");
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
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
    mLedgerAge.set_count(secondsSinceLastLedgerClose());
    mApp.syncOwnMetrics();
}

/*
    This is the main method that closes the current ledger based on
the close context that was computed by SCP or by the historical module
during replays.

*/
void
LedgerManagerImpl::closeLedger(LedgerCloseData const& ledgerData)
{
    DBTimeExcluder qtExclude(mApp);

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto header = ltx.loadHeader();
    ++header.current().ledgerSeq;
    header.current().previousLedgerHash = mLastClosedLedger.hash;
    CLOG(DEBUG, "Ledger") << "starting closeLedger() on ledgerSeq="
                          << header.current().ledgerSeq;

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
    mLastClose = now;
    mLedgerAge.set_count(0);

    // If we do not support ledger version, we can't apply that ledger, fail!
    if (header.current().ledgerVersion >
        Config::CURRENT_LEDGER_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Ledger")
            << "Unknown ledger version: " << header.current().ledgerVersion;
        throw std::runtime_error(
            fmt::format("cannot apply ledger with not supported version: {}",
                        header.current().ledgerVersion));
    }

    if (ledgerData.getTxSet()->previousLedgerHash() !=
        getLastClosedLedgerHeader().hash)
    {
        CLOG(ERROR, "Ledger")
            << "TxSet mismatch: LCD wants "
            << ledgerAbbrev(ledgerData.getLedgerSeq() - 1,
                            ledgerData.getTxSet()->previousLedgerHash())
            << ", LCL is " << ledgerAbbrev(getLastClosedLedgerHeader());

        CLOG(ERROR, "Ledger")
            << "Full LCL: " << xdr::xdr_to_string(getLastClosedLedgerHeader());

        throw std::runtime_error("txset mismatch");
    }

    if (ledgerData.getTxSet()->getContentsHash() !=
        ledgerData.getValue().txSetHash)
    {
        throw std::runtime_error("corrupt transaction set");
    }

    auto ledgerTime = mLedgerClose.TimeScope();

    auto const& sv = ledgerData.getValue();
    header.current().scpValue = sv;

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    vector<TransactionFramePtr> txs = ledgerData.getTxSet()->sortForApply();

    // first, charge fees
    processFeesSeqNums(txs, ltx);

    TransactionResultSet txResultSet;
    txResultSet.results.reserve(txs.size());

    applyTransactions(txs, ltx, txResultSet);

    ltx.loadHeader().current().txSetResultHash =
        sha256(xdr::xdr_to_opaque(txResultSet));

    // apply any upgrades that were decided during consensus
    // this must be done after applying transactions as the txset
    // was validated before upgrades
    for (size_t i = 0; i < sv.upgrades.size(); i++)
    {
        LedgerUpgrade lupgrade;
        try
        {
            xdr::xdr_from_opaque(sv.upgrades[i], lupgrade);
        }
        catch (xdr::xdr_runtime_error)
        {
            CLOG(FATAL, "Ledger") << "Unknown upgrade step at index " << i;
            throw;
        }

        try
        {
            LedgerTxn ltxUpgrade(ltx);
            Upgrades::applyTo(lupgrade, ltxUpgrade);

            auto ledgerSeq = ltxUpgrade.loadHeader().current().ledgerSeq;
            // Note: Index from 1 rather than 0 to match the behavior of
            // storeTransaction and storeTransactionFee.
            Upgrades::storeUpgradeHistory(getDatabase(), ledgerSeq, lupgrade,
                                          ltxUpgrade.getChanges(),
                                          static_cast<int>(i + 1));
            ltxUpgrade.commit();
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Ledger") << "Exception during upgrade: " << e.what();
        }
        catch (...)
        {
            CLOG(ERROR, "Ledger") << "Unknown exception during upgrade";
        }
    }

    ledgerClosed(ltx);

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
}

void
LedgerManagerImpl::deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                    uint32_t count)
{
    soci::transaction txscope(db.getSession());
    db.clearPreparedStatementCache();
    LedgerHeaderUtils::deleteOldEntries(db, ledgerSeq, count);
    TransactionFrame::deleteOldEntries(db, ledgerSeq, count);
    HerderPersistence::deleteOldEntries(db, ledgerSeq, count);
    Upgrades::deleteOldEntries(db, ledgerSeq, count);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::advanceLedgerPointers(LedgerHeader const& header)
{
    auto ledgerHash = sha256(xdr::xdr_to_opaque(header));
    CLOG(DEBUG, "Ledger") << "Advancing LCL: "
                          << ledgerAbbrev(mLastClosedLedger) << " -> "
                          << ledgerAbbrev(header, ledgerHash);

    mLastClosedLedger.hash = ledgerHash;
    mLastClosedLedger.header = header;
}

void
LedgerManagerImpl::processFeesSeqNums(std::vector<TransactionFramePtr>& txs,
                                      AbstractLedgerTxn& ltxOuter)
{
    CLOG(DEBUG, "Ledger") << "processing fees and sequence numbers";
    int index = 0;
    try
    {
        LedgerTxn ltx(ltxOuter);
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        for (auto tx : txs)
        {
            LedgerTxn ltxTx(ltx);
            tx->processFeeSeqNum(ltxTx);
            tx->storeTransactionFee(mApp.getDatabase(), ledgerSeq,
                                    ltxTx.getChanges(), ++index);
            ltxTx.commit();
        }
        ltx.commit();
    }
    catch (std::exception& e)
    {
        CLOG(FATAL, "Ledger")
            << "processFeesSeqNums error @ " << index << " : " << e.what();
        throw;
    }
}

void
LedgerManagerImpl::applyTransactions(std::vector<TransactionFramePtr>& txs,
                                     AbstractLedgerTxn& ltx,
                                     TransactionResultSet& txResultSet)
{
    int index = 0;

    // Record counts
    auto numTxs = txs.size();
    if (numTxs > 0)
    {
        mTransactionCount.Update(static_cast<int64_t>(numTxs));
        size_t numOps =
            std::accumulate(txs.begin(), txs.end(), size_t(0),
                            [](size_t s, TransactionFramePtr const& v) {
                                return s + v->getOperations().size();
                            });
        mOperationCount.Update(static_cast<int64_t>(numOps));
        CLOG(INFO, "Tx") << fmt::format("applying ledger {} (txs:{}, ops:{})",
                                        ltx.loadHeader().current().ledgerSeq,
                                        numTxs, numOps);
    }

    for (auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        TransactionMeta tm(1);
        try
        {
            CLOG(DEBUG, "Tx")
                << " tx#" << index << " = " << hexAbbrev(tx->getFullHash())
                << " ops=" << tx->getOperations().size()
                << " txseq=" << tx->getSeqNum() << " (@ "
                << mApp.getConfig().toShortString(tx->getSourceID()) << ")";
            tx->apply(mApp, ltx, tm.v1());
        }
        catch (InvariantDoesNotHold&)
        {
            CLOG(ERROR, "Ledger")
                << "Invariant failure during tx->apply for tx "
                << tx->getFullHash();
            throw;
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply for tx "
                                  << tx->getFullHash() << " : " << e.what();
            mInternalErrorCount.inc();
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        catch (...)
        {
            CLOG(ERROR, "Ledger")
                << "Unknown exception during tx->apply for tx "
                << tx->getFullHash();
            mInternalErrorCount.inc();
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
        tx->storeTransaction(mApp.getDatabase(), ledgerSeq, tm, ++index,
                             txResultSet);
    }
}

void
LedgerManagerImpl::storeCurrentLedger(LedgerHeader const& header)
{
    LedgerHeaderUtils::storeInDatabase(mApp.getDatabase(), header);

    Hash hash = sha256(xdr::xdr_to_opaque(header));
    assert(!isZero(hash));
    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(hash));

    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(header.ledgerSeq,
                            mApp.getBucketManager().getBucketList());

    // We almost always want to try to resolve completed merges to single
    // buckets, as it makes restarts less fragile: fewer saved/restored shadows,
    // fewer buckets for the user to accidentally delete from their buckets
    // dir. But we support the option of not-doing so, only for the sake of
    // testing. Note: this is nonblocking in any case.
    if (!mApp.getConfig().ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING)
    {
        has.resolveAnyReadyFutures();
    }

    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString());
}

void
LedgerManagerImpl::ledgerClosed(AbstractLedgerTxn& ltx)
{
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    mApp.getBucketManager().addBatch(mApp, ledgerSeq, ltx.getLiveEntries(),
                                     ltx.getDeadEntries());

    ltx.unsealHeader([this](LedgerHeader& lh) {
        mApp.getBucketManager().snapshotLedger(lh);
        storeCurrentLedger(lh);
        advanceLedgerPointers(lh);
    });
}
}
