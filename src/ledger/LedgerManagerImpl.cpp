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
#include "ledger/LedgerState.h"
#include "ledger/LedgerHeaderReference.h"
#include "ledger/LedgerHeaderUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/format.h"
#include "util/make_unique.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"
#include <util/basen.h>

#include <chrono>
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

using xdr::operator==;

std::unique_ptr<LedgerManager>
LedgerManager::create(Application& app)
{
    return make_unique<LedgerManagerImpl>(app);
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
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
    , mLedgerAgeClosed(app.getMetrics().NewTimer({"ledger", "age", "closed"}))
    , mLedgerAge(
          app.getMetrics().NewCounter({"ledger", "age", "current-seconds"}))
    , mLedgerStateCurrent(
          app.getMetrics().NewCounter({"ledger", "state", "current"}))
    , mLedgerStateChanges(
          app.getMetrics().NewTimer({"ledger", "state", "changes"}))
    , mLastClose(mApp.getClock().now())
    , mLastStateChange(mApp.getClock().now())
    , mSyncingLedgersSize(
          app.getMetrics().NewCounter({"ledger", "memory", "syncing-ledgers"}))
    , mState(LM_BOOTING_STATE)

{
}

void
LedgerManagerImpl::setState(State s)
{
    if (s != getState())
    {
        std::string oldState = getStateHuman();
        mState = s;
        mLedgerStateCurrent.set_count(static_cast<int64_t>(s));
        auto now = mApp.getClock().now();
        mLedgerStateChanges.Update(now - mLastStateChange);
        mLastStateChange = now;
        mApp.syncOwnMetrics();
        CLOG(INFO, "Ledger")
            << "Changing state " << oldState << " -> " << getStateHuman();
        if (mState != LM_CATCHING_UP_STATE)
        {
            mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
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
LedgerManagerImpl::startNewLedger(LedgerHeader genesisLedger)
{
    DBTimeExcluder qtExclude(mApp);
    auto ledgerTime = mLedgerClose.TimeScope();
    SecretKey skey = SecretKey::fromSeed(mApp.getNetworkID());

    LedgerState ls(mApp.getLedgerStateRoot());
    auto header = ls.loadHeader();
    header->header() = genesisLedger;
    header->invalidate();

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 1;
    le.data.type(ACCOUNT);
    le.data.account().accountID = skey.getPublicKey();
    le.data.account().thresholds[0] = 1;
    le.data.account().balance = genesisLedger.totalCoins;
    ls.create(le);

    CLOG(INFO, "Ledger") << "Established genesis ledger, closing";
    CLOG(INFO, "Ledger") << "Root account seed: " << skey.getStrKeySeed().value;
    ledgerClosed(ls);
    ls.commit();
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
            loadLedgerHeaderByHash(getDatabase(), lastLedgerHash);
        if (!currentLedger)
        {
            throw std::runtime_error("Could not load ledger from database");
        }
        LedgerState ls(mApp.getLedgerStateRoot());
        ls.loadHeader()->header() = *currentLedger;
        ls.commit();

        if (handler)
        {
            string hasString = mApp.getPersistentState().getState(
                PersistentState::kHistoryArchiveState);
            HistoryArchiveState has;
            has.fromString(hasString);

            auto continuation = [this, handler,
                                 has](asio::error_code const& ec) {
                if (ec)
                {
                    handler(ec);
                }
                else
                {
                    mApp.getBucketManager().assumeState(has);

                    LedgerState ls(mApp.getLedgerStateRoot());
                    auto header = ls.loadHeader();
                    CLOG(INFO, "Ledger") << "Loaded last known ledger: "
                                         << stellar::ledgerAbbrev(header->header());
                    advanceLedgerPointers(header);
                    ls.commit();
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
            LedgerState ls(mApp.getLedgerStateRoot());
            advanceLedgerPointers(ls.loadHeader());
            ls.commit();
        }
    }
}

Database&
LedgerManagerImpl::getDatabase()
{
    return mApp.getDatabase();
}

LedgerHeaderHistoryEntry const&
LedgerManagerImpl::getLastClosedLedgerHeader() const
{
    return mLastClosedLedger;
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
        if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.getLedgerSeq())
        {
            if (mLastClosedLedger.hash ==
                ledgerData.getTxSet()->previousLedgerHash())
            {
                if (st == LM_BOOTING_STATE)
                {
                    setState(LM_SYNCED_STATE);
                }
                closeLedger(ledgerData);
                CLOG(INFO, "Ledger")
                    << "Closed ledger: " << ledgerAbbrev(mLastClosedLedger);
            }
            else
            {
                CLOG(FATAL, "Ledger") << "Network consensus for ledger "
                                      << mLastClosedLedger.header.ledgerSeq
                                      << " changed; this should never happen";
                throw std::runtime_error("Network consensus inconsistency");
            }
        }
        else if (ledgerData.getLedgerSeq() <=
                 mLastClosedLedger.header.ledgerSeq)
        {
            CLOG(INFO, "Ledger")
                << "Skipping close ledger: local state is "
                << mLastClosedLedger.header.ledgerSeq << ", more recent than "
                << ledgerData.getLedgerSeq();
        }
        else
        {
            // Out of sync, buffer what we just heard and start catchup.
            CLOG(INFO, "Ledger")
                << "Lost sync, local LCL is "
                << mLastClosedLedger.header.ledgerSeq
                << ", network closed ledger " << ledgerData.getLedgerSeq();

            assert(mSyncingLedgers.size() == 0);
            auto addResult = mSyncingLedgers.add(ledgerData);
            assert(addResult == SyncingLedgerChainAddResult::CONTIGUOUS);
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());
            CLOG(INFO, "Ledger")
                << "Close of ledger " << ledgerData.getLedgerSeq()
                << " buffered, starting catchup";

            // catchup just before first buffered ledger
            // that way we will have a way to verify history consistency -
            // compare previousLedgerHash of buffered ledger with last one
            // downloaded from history
            startCatchUp({ledgerData.getLedgerSeq() - 1, getCatchupCount(mApp)},
                         false);
        }
        break;

    case LedgerManager::LM_CATCHING_UP_STATE:
    {
        switch (mSyncingLedgers.add(ledgerData))
        {
        case SyncingLedgerChainAddResult::CONTIGUOUS:
            // Normal close while catching up
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());
            mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
            break;
        case SyncingLedgerChainAddResult::TOO_OLD:
            CLOG(INFO, "Ledger")
                << "Skipping close ledger: latest known is "
                << mSyncingLedgers.back().getLedgerSeq()
                << ", more recent than " << ledgerData.getLedgerSeq();
            break;
        case SyncingLedgerChainAddResult::TOO_NEW:
            // Out-of-order close while catching up; timeout / network failure?
            CLOG(WARNING, "Ledger")
                << "Out-of-order close during catchup, buffered to "
                << mSyncingLedgers.back().getLedgerSeq()
                << " but network closed " << ledgerData.getLedgerSeq();

            CLOG(WARNING, "Ledger")
                << "this round of catchup will fail and restart.";

            mApp.getCatchupManager().logAndUpdateCatchupStatus(false);
            break;
        }
    }
    break;

    default:
        assert(false);
    }
}

void
LedgerManagerImpl::startCatchUp(CatchupConfiguration configuration,
                                bool manualCatchup)
{
    auto lastClosedLedger = getLastClosedLedgerNum();
    if ((configuration.toLedger() != CatchupConfiguration::CURRENT) &&
        (configuration.toLedger() <= lastClosedLedger))
    {
        throw std::invalid_argument("Target ledger is not newer than LCL");
    }

    setState(LM_CATCHING_UP_STATE);

    mApp.getCatchupManager().catchupHistory(
        configuration, manualCatchup,
        std::bind(&LedgerManagerImpl::historyCaughtup, this, _1, _2, _3));
}

HistoryManager::LedgerVerificationStatus
LedgerManagerImpl::verifyCatchupCandidate(
    LedgerHeaderHistoryEntry const& candidate, bool manualCatchup) const
{
    if (manualCatchup)
    {
        assert(mSyncingLedgers.empty());
        CLOG(WARNING, "History")
            << "Accepting unknown-hash ledger due to manual catchup";
        return HistoryManager::VERIFY_STATUS_OK;
    }

    assert(!mSyncingLedgers.empty());
    assert(mSyncingLedgers.front().getLedgerSeq() ==
           candidate.header.ledgerSeq + 1);

    // asserts dont work in release builds
    if (!mSyncingLedgers.empty() &&
        mSyncingLedgers.front().getLedgerSeq() ==
            candidate.header.ledgerSeq + 1 &&
        mSyncingLedgers.front().getTxSet()->previousLedgerHash() ==
            candidate.hash)
    {
        return HistoryManager::VERIFY_STATUS_OK;
    }
    else
    {
        return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
    }
}

void
LedgerManagerImpl::historyCaughtup(asio::error_code const& ec,
                                   CatchupWork::ProgressState progressState,
                                   LedgerHeaderHistoryEntry const& lastClosed)
{
    if (ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up: " << ec.message();
        CLOG(ERROR, "Ledger") << "Catchup will restart at next close.";
        setState(LM_BOOTING_STATE);
        mApp.getCatchupManager().historyCaughtup();
    }
    else
    {
        switch (progressState)
        {
        case CatchupWork::ProgressState::APPLIED_BUCKETS:
        {
            LedgerState ls(mApp.getLedgerStateRoot());
            auto header = ls.loadHeader();
            auto ledgerHash = sha256(xdr::xdr_to_opaque(lastClosed.header));
            header->header() = lastClosed.header;
            storeCurrentLedger(header);

            ++header->header().ledgerSeq;
            header->header().previousLedgerHash = ledgerHash;
            ls.commit();

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

        // Now replay remaining txs from buffered local network history.
        for (auto const& lcd : mSyncingLedgers)
        {
            if (lcd.getLedgerSeq() < mLastClosedLedger.header.ledgerSeq + 1)
            {
                // We may have buffered lots of stuff between the consensus
                // ledger when we started catchup and the final ledger applied
                // during catchup replay. We can just drop these, they're
                // redundant with what catchup did.

                if (lcd.getLedgerSeq() == mLastClosedLedger.header.ledgerSeq)
                {
                    // At the knit-up point between history-replay and
                    // buffer-replay, we should have identity between the
                    // contents of the consensus LCD and the last ledger catchup
                    // closed (which was proposed as a candidate, and we
                    // approved in verifyCatchupCandidate).
                    assert(lcd.getTxSet()->getContentsHash() ==
                           lcd.getValue().txSetHash);
                    assert(lcd.getValue() == mLastClosedLedger.header.scpValue);
                }

                continue;
            }
            else if (lcd.getLedgerSeq() ==
                     mLastClosedLedger.header.ledgerSeq + 1)
            {
                CLOG(INFO, "Ledger")
                    << "Replaying buffered ledger-close: "
                    << "[seq=" << lcd.getLedgerSeq() << ", prev="
                    << hexAbbrev(lcd.getTxSet()->previousLedgerHash())
                    << ", tx_count=" << lcd.getTxSet()->size()
                    << ", sv: " << stellarValueToString(lcd.getValue()) << "]";
                closeLedger(lcd);
            }
            else
            {
                // We should never _overshoot_ the last ledger. The whole point
                // of rounding the initLedger value up to the next history
                // boundary is that there should always be some overlap between
                // the buffered LedgerCloseDatas and the history block we catch
                // up with.
                //
                // So if we ever get here, something was seriously wrong --
                // possibly SCP timed out / fell behind _during_ catchup -- and
                // we should flush everything we did during catchup and restart
                // the process anew.

                assert(lcd.getLedgerSeq() >
                       mLastClosedLedger.header.ledgerSeq + 1);
                CLOG(ERROR, "Ledger")
                    << "Catchup failed to buffer contiguous ledger chain";
                CLOG(ERROR, "Ledger")
                    << "LCL is " << ledgerAbbrev(mLastClosedLedger)
                    << ", trying to apply buffered close " << lcd.getLedgerSeq()
                    << " with txhash "
                    << hexAbbrev(lcd.getTxSet()->getContentsHash());
                mSyncingLedgers = {};
                mSyncingLedgersSize.set_count(mSyncingLedgers.size());
                CLOG(ERROR, "Ledger") << "Catchup will restart at next close.";
                setState(LM_BOOTING_STATE);
                return;
            }
        }

        CLOG(INFO, "Ledger")
            << "Caught up to LCL including recent network activity: "
            << ledgerAbbrev(mLastClosedLedger);
        setState(LM_SYNCED_STATE);
    }

    // Either way, we're done processing the ledgers backlog
    mSyncingLedgers = {};
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
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
    auto n = static_cast<int64_t>(getState());
    auto c = mLedgerStateCurrent.count();
    if (n != c)
    {
        mLedgerStateCurrent.set_count(n);
        auto now = mApp.getClock().now();
        mLedgerStateChanges.Update(now - mLastStateChange);
        mLastStateChange = now;
    }
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
    // TODO(jonjove): Unnecessary ledger state?
    CLOG(DEBUG, "Ledger") << "starting closeLedger() on ledgerSeq="
                          << getCurrentLedgerNum(mApp.getLedgerStateRoot());

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
    mLastClose = now;
    mLedgerAge.set_count(0);

    // If we do not support ledger version, we can't apply that ledger, fail!
    if (getCurrentLedgerVersion(mApp.getLedgerStateRoot()) >
        Config::CURRENT_LEDGER_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Ledger") << "Unknown ledger version: "
                              << getCurrentLedgerVersion(mApp.getLedgerStateRoot());
        throw std::runtime_error(
            fmt::format("cannot apply ledger with not supported version: {}",
                        getCurrentLedgerVersion(mApp.getLedgerStateRoot())));
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

    LedgerState ls(mApp.getLedgerStateRoot());

    auto ledgerTime = mLedgerClose.TimeScope();

    {
        auto header = ls.loadHeader();
        header->header().scpValue = ledgerData.getValue();
        header->invalidate();
    }

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    vector<TransactionFramePtr> txs = ledgerData.getTxSet()->sortForApply();

    // first, charge fees
    processFeesSeqNums(txs, ls);

    TransactionResultSet txResultSet;
    txResultSet.results.reserve(txs.size());
    applyTransactions(txs, ls, txResultSet);

    {
        auto header = ls.loadHeader();
        header->header().txSetResultHash =
            sha256(xdr::xdr_to_opaque(txResultSet));

        // apply any upgrades that were decided during consensus
        // this must be done after applying transactions as the txset
        // was validated before upgrades
        auto const& sv = ledgerData.getValue();
        for (size_t i = 0; i < sv.upgrades.size(); i++)
        {
            LedgerUpgrade lupgrade;
            try
            {
                xdr::xdr_from_opaque(sv.upgrades[i], lupgrade);
                Upgrades::applyTo(lupgrade, header->header());
            }
            catch (xdr::xdr_runtime_error)
            {
                CLOG(FATAL, "Ledger") << "Unknown upgrade step at index " << i;
                throw;
            }
        }
        header->invalidate();
    }

    ledgerClosed(ls);

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
    hm.maybeQueueHistoryCheckpoint(ls);

    // step 2
    mApp.getDatabase().clearPreparedStatementCache();
    ls.commit();

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
    deleteOldLedgerHeaders(db, ledgerSeq, count);
    TransactionFrame::deleteOldEntries(db, ledgerSeq, count);
    HerderPersistence::deleteOldEntries(db, ledgerSeq, count);
    db.clearPreparedStatementCache();
    txscope.commit();
}

void
LedgerManagerImpl::advanceLedgerPointers(std::shared_ptr<LedgerHeaderReference> header)
{
    auto ledgerHash = sha256(xdr::xdr_to_opaque(header->header()));
    CLOG(DEBUG, "Ledger") << "Advancing LCL: "
                          << ledgerAbbrev(mLastClosedLedger) << " -> "
                          << ledgerAbbrev(header->header(), ledgerHash);

    mLastClosedLedger.hash = ledgerHash;
    mLastClosedLedger.header = header->header();

    ++header->header().ledgerSeq;
    header->header().previousLedgerHash = ledgerHash;
    CLOG(DEBUG, "Ledger") << "New current ledger: seq="
                          << header->header().ledgerSeq;
}

void
LedgerManagerImpl::processFeesSeqNums(std::vector<TransactionFramePtr>& txs,
                                      LedgerState& ls)
{
    CLOG(DEBUG, "Ledger") << "processing fees and sequence numbers";
    int index = 0;
    try
    {
        // TODO(jonjove): Need a LedgerState here?
        for (auto tx : txs)
        {
            LedgerState lsInner(ls);
            auto header = lsInner.loadHeader();
            uint32_t ledgerSeq = header->header().ledgerSeq;
            header->invalidate();

            tx->processFeeSeqNum(lsInner);
            tx->storeTransactionFee(mApp.getDatabase(), ledgerSeq, lsInner.getChanges(), ++index);
            lsInner.updateLastModified();
            lsInner.commit();
        }
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
                                     LedgerState& ls,
                                     TransactionResultSet& txResultSet)
{
    auto lh = ls.loadHeader();
    CLOG(DEBUG, "Tx") << "applyTransactions: ledger = "
                      << getCurrentLedgerNum(lh);
    lh->invalidate();

    int index = 0;
    for (auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        LedgerState lsTx(ls);
        auto header = lsTx.loadHeader();
        uint32_t ledgerSeq = header->header().ledgerSeq;
        header->invalidate();

        TransactionMeta tm(1);
        try
        {
            CLOG(DEBUG, "Tx")
                << " tx#" << index << " = " << hexAbbrev(tx->getFullHash())
                << " txseq=" << tx->getSeqNum() << " (@ "
                << mApp.getConfig().toShortString(tx->getSourceID()) << ")";
            tx->apply(lsTx, tm.v1(), mApp);
        }
        catch (InvariantDoesNotHold& e)
        {
            throw e;
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply: " << e.what();
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        catch (...)
        {
            CLOG(ERROR, "Ledger") << "Unknown exception during tx->apply";
            tx->getResult().result.code(txINTERNAL_ERROR);
        }
        lsTx.commit();
        tx->storeTransaction(mApp.getDatabase(), ledgerSeq, tm, ++index, txResultSet);
    }
}

void
LedgerManagerImpl::storeCurrentLedger(std::shared_ptr<LedgerHeaderReference> header)
{
    storeHeaderInDatabase(header->header());

    auto ledgerHash = sha256(xdr::xdr_to_opaque(header->header()));
    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(ledgerHash));

    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(header->header().ledgerSeq,
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
LedgerManagerImpl::ledgerClosed(LedgerState& ls)
{
    auto header = ls.loadHeader();

    // TODO(jonjove): Mark LedgerState meters?
    mApp.getBucketManager().addBatch(mApp, header->header().ledgerSeq,
                                     ls.getLiveEntries(),
                                     ls.getDeadEntries());

    mApp.getBucketManager().snapshotLedger(header->header());
    storeCurrentLedger(header);
    advanceLedgerPointers(header);

    header->invalidate();
}

void
LedgerManagerImpl::storeHeaderInDatabase(LedgerHeader const& header)
{
    auto headerBytes(xdr::xdr_to_opaque(header));
    std::string hash(binToHex(sha256(headerBytes))),
        prevHash(binToHex(header.previousLedgerHash)),
        bucketListHash(binToHex(header.bucketListHash));

    std::string headerEncoded;
    headerEncoded = bn::encode_b64(headerBytes);

    auto& db = mApp.getDatabase();

    // note: columns other than "data" are there to faciliate lookup/processing
    auto prep = db.getPreparedStatement(
        "INSERT INTO ledgerheaders "
        "(ledgerhash, prevhash, bucketlisthash, ledgerseq, closetime, data) "
        "VALUES "
        "(:h,        :ph,      :blh,            :seq,     :ct,       :data)");
    auto& st = prep.statement();
    st.exchange(soci::use(hash));
    st.exchange(soci::use(prevHash));
    st.exchange(soci::use(bucketListHash));
    st.exchange(soci::use(header.ledgerSeq));
    st.exchange(soci::use(header.scpValue.closeTime));
    st.exchange(soci::use(headerEncoded));
    st.define_and_bind();
    {
        auto timer = db.getInsertTimer("ledger-header");
        st.execute(true);
    }
    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }
}
}
