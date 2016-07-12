// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "herder/LedgerCloseData.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManagerImpl.h"
#include "TrustFrame.h"
#include "OfferFrame.h"
#include "DataFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "util/format.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "medida/counter.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"

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
LedgerManager::ledgerAbbrev(LedgerHeaderFrame::pointer p)
{
    if (!p)
    {
        return "[empty]";
    }
    return ledgerAbbrev(p->mHeader, p->getHash());
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
        CLOG(INFO, "Ledger") << "Changing state " << oldState << " -> "
                             << getStateHuman();
        if (mState != LM_CATCHING_UP_STATE)
        {
            mApp.getHistoryManager().logAndUpdateStatus(true);
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

void
LedgerManagerImpl::startNewLedger()
{
    DBTimeExcluder qtExclude(mApp);
    auto ledgerTime = mLedgerClose.TimeScope();
    SecretKey skey = SecretKey::fromSeed(mApp.getNetworkID());

    AccountFrame masterAccount(skey.getPublicKey());
    masterAccount.getAccount().balance = 1000000000000000000;
    LedgerHeader genesisHeader;

    // all fields are initialized by default to 0
    // set the ones that are not 0
    genesisHeader.baseFee = 100;
    genesisHeader.baseReserve = 100000000;
    genesisHeader.maxTxSetSize = 100; // 100 tx/ledger max
    genesisHeader.totalCoins = masterAccount.getAccount().balance;
    genesisHeader.ledgerSeq = 1;

    LedgerDelta delta(genesisHeader, getDatabase());
    masterAccount.storeAdd(delta, this->getDatabase());
    delta.commit();

    mCurrentLedger = make_shared<LedgerHeaderFrame>(genesisHeader);
    CLOG(INFO, "Ledger") << "Established genesis ledger, closing";
    CLOG(INFO, "Ledger") << "Root account seed: " << skey.getStrKeySeed();
    closeLedgerHelper(delta);
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

        mCurrentLedger =
            LedgerHeaderFrame::loadByHash(lastLedgerHash, getDatabase());
        if (!mCurrentLedger)
        {
            throw std::runtime_error("Could not load ledger from database");
        }

        if (handler)
        {
            string hasString = mApp.getPersistentState().getState(
                PersistentState::kHistoryArchiveState);
            HistoryArchiveState has;
            has.fromString(hasString);

            auto continuation = [this, handler, has](asio::error_code const& ec)
            {
                if (ec)
                {
                    handler(ec);
                }
                else
                {
                    mApp.getBucketManager().assumeState(has);

                    CLOG(INFO, "Ledger") << "Loaded last known ledger: "
                                         << ledgerAbbrev(mCurrentLedger);

                    advanceLedgerPointers();
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
            advanceLedgerPointers();
        }
    }
}

Database&
LedgerManagerImpl::getDatabase()
{
    return mApp.getDatabase();
}

int64_t
LedgerManagerImpl::getTxFee() const
{
    return mCurrentLedger->mHeader.baseFee;
}

uint32_t
LedgerManagerImpl::getMaxTxSetSize() const
{
    return mCurrentLedger->mHeader.maxTxSetSize;
}

int64_t
LedgerManagerImpl::getMinBalance(uint32_t ownerCount) const
{
    return (2 + ownerCount) * mCurrentLedger->mHeader.baseReserve;
}

uint32_t
LedgerManagerImpl::getLedgerNum() const
{
    return mCurrentLedger->mHeader.ledgerSeq;
}

uint64_t
LedgerManagerImpl::getCloseTime() const
{
    return mCurrentLedger->mHeader.scpValue.closeTime;
}

LedgerHeader const&
LedgerManagerImpl::getCurrentLedgerHeader() const
{
    return mCurrentLedger->mHeader;
}

LedgerHeader&
LedgerManagerImpl::getCurrentLedgerHeader()
{
    return mCurrentLedger->mHeader;
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

HistoryManager::CatchupMode
getCatchupMode(Application& app)
{
    return app.getConfig().CATCHUP_COMPLETE
               ? HistoryManager::CATCHUP_COMPLETE
               : (app.getConfig().CATCHUP_RECENT == 0
                      ? HistoryManager::CATCHUP_MINIMAL
                      : HistoryManager::CATCHUP_RECENT);
}

// called by txherder
void
LedgerManagerImpl::externalizeValue(LedgerCloseData const& ledgerData)
{
    CLOG(INFO, "Ledger") << "Got consensus: "
                         << "[seq=" << ledgerData.mLedgerSeq << ", prev="
                         << hexAbbrev(ledgerData.mTxSet->previousLedgerHash())
                         << ", tx_count=" << ledgerData.mTxSet->size()
                         << ", sv: " << stellarValueToString(ledgerData.mValue)
                         << "]";

    auto st = getState();
    switch (st)
    {
    case LedgerManager::LM_BOOTING_STATE:
    case LedgerManager::LM_SYNCED_STATE:
        if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.mLedgerSeq)
        {
            if (mLastClosedLedger.hash ==
                ledgerData.mTxSet->previousLedgerHash())
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
        else if (ledgerData.mLedgerSeq <= mLastClosedLedger.header.ledgerSeq)
        {
            CLOG(INFO, "Ledger") << "Skipping close ledger: local state is "
                                 << mLastClosedLedger.header.ledgerSeq
                                 << ", more recent than "
                                 << ledgerData.mLedgerSeq;
        }
        else
        {
            // Out of sync, buffer what we just heard and start catchup.
            CLOG(INFO, "Ledger") << "Lost sync, local LCL is "
                                 << mLastClosedLedger.header.ledgerSeq
                                 << ", network closed ledger "
                                 << ledgerData.mLedgerSeq;

            assert(mSyncingLedgers.size() == 0);
            mSyncingLedgers.push_back(ledgerData);
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());
            CLOG(INFO, "Ledger") << "Close of ledger " << ledgerData.mLedgerSeq
                                 << " buffered, starting catchup";
            startCatchUp(ledgerData.mLedgerSeq, getCatchupMode(mApp));
        }
        break;

    case LedgerManager::LM_CATCHING_UP_STATE:
    {
        bool contiguous =
            (mSyncingLedgers.empty() ||
             mSyncingLedgers.back().mLedgerSeq + 1 == ledgerData.mLedgerSeq);

        bool skipLogging = false;

        if (contiguous)
        {
            // Normal close while catching up
            mSyncingLedgers.push_back(ledgerData);
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());
        }
        else if (ledgerData.mLedgerSeq <= mSyncingLedgers.back().mLedgerSeq)
        {
            CLOG(INFO, "Ledger") << "Skipping close ledger: latest known is "
                                 << mSyncingLedgers.back().mLedgerSeq
                                 << ", more recent than "
                                 << ledgerData.mLedgerSeq;
            skipLogging = true;
        }
        else
        {
            // Out-of-order close while catching up; timeout / network failure?
            CLOG(WARNING, "Ledger")
                << "Out-of-order close during catchup, buffered to "
                << mSyncingLedgers.back().mLedgerSeq << " but network closed "
                << ledgerData.mLedgerSeq;

            CLOG(WARNING, "Ledger")
                << "this round of catchup will fail and restart.";
        }

        if (!skipLogging)
        {
            mApp.getHistoryManager().logAndUpdateStatus(contiguous);
        }
    }
    break;

    default:
        assert(false);
    }
}

void
LedgerManagerImpl::startCatchUp(uint32_t initLedger,
                                HistoryManager::CatchupMode resume,
                                bool manualCatchup)
{
    setState(LM_CATCHING_UP_STATE);
    mApp.getHistoryManager().catchupHistory(
        initLedger, resume,
        std::bind(&LedgerManagerImpl::historyCaughtup, this, _1, _2, _3),
        manualCatchup);
}

HistoryManager::VerifyHashStatus
LedgerManagerImpl::verifyCatchupCandidate(
    LedgerHeaderHistoryEntry const& candidate) const
{
// This is a callback from CatchupStateMachine when it's considering whether
// to treat a retrieved history block as legitimate. It asks LedgerManagerImpl
// if it's seen (in its previous, current, or buffer of ledgers-to-close that
// have queued up since catchup began) whether it believes the candidate is a
// legitimate part of history. LedgerManagerImpl is allowed to answer "unknown"
// here, which causes CatchupStateMachine to pause and retry later.

#define CHECK_PAIR(aseq, bseq, ahash, bhash)                                   \
    if ((aseq) == (bseq))                                                      \
    {                                                                          \
        if ((ahash) == (bhash))                                                \
        {                                                                      \
            return HistoryManager::VERIFY_HASH_OK;                             \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            return HistoryManager::VERIFY_HASH_BAD;                            \
        }                                                                      \
    }

    CHECK_PAIR(mLastClosedLedger.header.ledgerSeq, candidate.header.ledgerSeq,
               mLastClosedLedger.hash, candidate.hash);

    CHECK_PAIR(mLastClosedLedger.header.ledgerSeq,
               candidate.header.ledgerSeq + 1,
               mLastClosedLedger.header.previousLedgerHash, candidate.hash);

    CHECK_PAIR(mCurrentLedger->mHeader.ledgerSeq,
               candidate.header.ledgerSeq + 1,
               mCurrentLedger->mHeader.previousLedgerHash, candidate.hash);

    for (auto const& ld : mSyncingLedgers)
    {
        CHECK_PAIR(ld.mLedgerSeq, candidate.header.ledgerSeq + 1,
                   ld.mTxSet->previousLedgerHash(), candidate.hash);
    }

#undef CHECK_PAIR
    return HistoryManager::VERIFY_HASH_UNKNOWN;
}

void
LedgerManagerImpl::historyCaughtup(asio::error_code const& ec,
                                   HistoryManager::CatchupMode mode,
                                   LedgerHeaderHistoryEntry const& lastClosed)
{
    if (ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up: " << ec.message();
        CLOG(ERROR, "Ledger") << "Catchup will restart at next close.";
        setState(LM_BOOTING_STATE);
    }
    else
    {
        // If we're in CATCHUP_RECENT mode, we actually only got part way
        // through catchup -- we did the minimal prefix part -- and will
        // get another callback as CATCHUP_COMPLETE when recent-replay is
        // done. So for now just deposit LCL and prepare for replay.
        if (mode == HistoryManager::CATCHUP_RECENT)
        {
            mLastClosedLedger = lastClosed;
            CLOG(INFO, "Ledger") << "First phase of CATCHUP_RECENT done: "
                                 << ledgerAbbrev(mLastClosedLedger);
            mCurrentLedger = make_shared<LedgerHeaderFrame>(lastClosed);
            return;
        }

        // If we were in CATCHUP_MINIMAL mode, LCL has not been updated
        // and we need to pick it up here.
        if (mode == HistoryManager::CATCHUP_MINIMAL)
        {
            mLastClosedLedger = lastClosed;
            mCurrentLedger = make_shared<LedgerHeaderFrame>(lastClosed);
        }
        else
        {
            // In this case we should actually have been caught-up during the
            // replay process and, if judged successful, our LCL should be the
            // one provided as well.
            assert(mode == HistoryManager::CATCHUP_COMPLETE);
            assert(lastClosed.hash == mLastClosedLedger.hash);
            assert(lastClosed.header == mLastClosedLedger.header);
        }

        CLOG(INFO, "Ledger") << "Caught up to LCL from history: "
                             << ledgerAbbrev(mLastClosedLedger);

        // Now replay remaining txs from buffered local network history.
        bool applied = false;
        for (auto const& lcd : mSyncingLedgers)
        {
            if (lcd.mLedgerSeq < mLastClosedLedger.header.ledgerSeq + 1)
            {
                // We may have buffered lots of stuff between the consensus
                // ledger when we started catchup and the final ledger applied
                // during catchup replay. We can just drop these, they're
                // redundant with what catchup did.

                if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq)
                {
                    // At the knit-up point between history-replay and
                    // buffer-replay, we should have identity between the
                    // contents of the consensus LCD and the last ledger catchup
                    // closed (which was proposed as a candidate, and we
                    // approved in verifyCatchupCandidate).
                    assert(lcd.mTxSet->getContentsHash() ==
                           lcd.mValue.txSetHash);
                    assert(lcd.mValue == mLastClosedLedger.header.scpValue);
                }

                continue;
            }
            else if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq + 1)
            {
                CLOG(INFO, "Ledger")
                    << "Replaying buffered ledger-close: "
                    << "[seq=" << lcd.mLedgerSeq
                    << ", prev=" << hexAbbrev(lcd.mTxSet->previousLedgerHash())
                    << ", tx_count=" << lcd.mTxSet->size()
                    << ", sv: " << stellarValueToString(lcd.mValue) << "]";
                closeLedger(lcd);
                applied = true;
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

                assert(lcd.mLedgerSeq > mLastClosedLedger.header.ledgerSeq + 1);
                auto const& lastBuffered = mSyncingLedgers.back();
                CLOG(ERROR, "Ledger")
                    << "Catchup failed to buffer contiguous ledger chain";
                CLOG(ERROR, "Ledger")
                    << "LCL is " << ledgerAbbrev(mLastClosedLedger)
                    << ", trying to apply buffered close " << lcd.mLedgerSeq
                    << " with txhash "
                    << hexAbbrev(lcd.mTxSet->getContentsHash());
                CLOG(ERROR, "Ledger")
                    << "Flushing buffer and restarting at ledger "
                    << lastBuffered.mLedgerSeq;
                mSyncingLedgers.clear();
                mSyncingLedgersSize.set_count(mSyncingLedgers.size());
                startCatchUp(lastBuffered.mLedgerSeq, getCatchupMode(mApp));
                return;
            }
        }

        CLOG(INFO, "Ledger")
            << "Caught up to LCL including recent network activity: "
            << ledgerAbbrev(mLastClosedLedger);
        setState(LM_SYNCED_STATE);
    }

    // Either way, we're done processing the ledgers backlog
    mSyncingLedgers.clear();
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
    CLOG(DEBUG, "Ledger") << "starting closeLedger() on ledgerSeq="
                          << mCurrentLedger->mHeader.ledgerSeq;

    auto now = mApp.getClock().now();
    mLedgerAgeClosed.Update(now - mLastClose);
    mLastClose = now;
    mLedgerAge.set_count(0);

    if (ledgerData.mTxSet->previousLedgerHash() !=
        getLastClosedLedgerHeader().hash)
    {
        CLOG(ERROR, "Ledger")
            << "TxSet mismatch: LCD wants "
            << ledgerAbbrev(ledgerData.mLedgerSeq - 1,
                            ledgerData.mTxSet->previousLedgerHash())
            << ", LCL is " << ledgerAbbrev(getLastClosedLedgerHeader());

        CLOG(ERROR, "Ledger")
            << "Full LCL: " << xdr::xdr_to_string(getLastClosedLedgerHeader());

        throw std::runtime_error("txset mismatch");
    }

    if (ledgerData.mTxSet->getContentsHash() != ledgerData.mValue.txSetHash)
    {
        throw std::runtime_error("corrupt transaction set");
    }

    soci::transaction txscope(getDatabase().getSession());

    auto ledgerTime = mLedgerClose.TimeScope();

    auto const& sv = ledgerData.mValue;
    mCurrentLedger->mHeader.scpValue = sv;

    LedgerDelta ledgerDelta(mCurrentLedger->mHeader, getDatabase());

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    vector<TransactionFramePtr> txs = ledgerData.mTxSet->sortForApply();

    // first, charge fees
    processFeesSeqNums(txs, ledgerDelta);

    TransactionResultSet txResultSet;
    txResultSet.results.reserve(txs.size());

    applyTransactions(txs, ledgerDelta, txResultSet);

    ledgerDelta.getHeader().txSetResultHash =
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
        switch (lupgrade.type())
        {
        case LEDGER_UPGRADE_VERSION:
            ledgerDelta.getHeader().ledgerVersion = lupgrade.newLedgerVersion();
            break;
        case LEDGER_UPGRADE_BASE_FEE:
            ledgerDelta.getHeader().baseFee = lupgrade.newBaseFee();
            break;
        case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
            ledgerDelta.getHeader().maxTxSetSize = lupgrade.newMaxTxSetSize();
            break;
        default:
        {
            string s;
            s = "Unknown upgrade type: ";
            s += std::to_string(lupgrade.type());
            throw std::runtime_error(s);
        }
        }
    }

    ledgerDelta.checkAgainstDatabase(mApp);

    ledgerDelta.commit();
    closeLedgerHelper(ledgerDelta);

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
    mApp.getDatabase().clearPreparedStatementCache();
    txscope.commit();

    // step 3
    hm.publishQueuedHistory();
    hm.logAndUpdateStatus(true);

    // step 4
    if (getState() != LM_CATCHING_UP_STATE) {
        mApp.getBucketManager().forgetUnreferencedBuckets();
    }
}

void
LedgerManagerImpl::deleteOldEntries(Database& db, uint32_t ledgerSeq)
{
    LedgerHeaderFrame::deleteOldEntries(db, ledgerSeq);
    TransactionFrame::deleteOldEntries(db, ledgerSeq);
    Herder::deleteOldEntries(db, ledgerSeq);
}

void
LedgerManagerImpl::checkDbState()
{
    std::unordered_map<AccountID, AccountFrame::pointer> aData =
        AccountFrame::checkDB(getDatabase());
    std::unordered_map<AccountID, std::vector<TrustFrame::pointer>> trustLines;
    trustLines = TrustFrame::loadAllLines(getDatabase());
    std::unordered_map<AccountID, std::vector<OfferFrame::pointer>> offers;
    offers = OfferFrame::loadAllOffers(getDatabase());
    std::unordered_map<AccountID, std::vector<DataFrame::pointer>> datas;
    datas = DataFrame::loadAllData(getDatabase());


    for (auto& i : aData)
    {
        auto const& a = i.second->getAccount();

        // checks the number of sub entries found in the database
        size_t actualSubEntries = a.signers.size();
        auto itTL = trustLines.find(i.first);
        if (itTL != trustLines.end())
        {
            actualSubEntries += itTL->second.size();
        }
        auto itOffers = offers.find(i.first);
        if (itOffers != offers.end())
        {
            actualSubEntries += itOffers->second.size();
        }

        auto itDatas = datas.find(i.first);
        if(itDatas != datas.end())
        {
            actualSubEntries += itDatas->second.size();
        }

        if (a.numSubEntries != (uint32)actualSubEntries)
        {
            throw std::runtime_error(
                fmt::format("Mismatch in number of subentries for account {}: "
                            "account says {} but found {}",
                            PubKeyUtils::toStrKey(i.first), a.numSubEntries,
                            actualSubEntries));
        }
    }
    for (auto& tl : trustLines)
    {
        if (aData.find(tl.first) == aData.end())
        {
            throw std::runtime_error(
                fmt::format("Unexpected trust line found for account {}",
                            PubKeyUtils::toStrKey(tl.first)));
        }
    }
    for (auto& of : offers)
    {
        if (aData.find(of.first) == aData.end())
        {
            throw std::runtime_error(
                fmt::format("Unexpected offer found for account {}",
                            PubKeyUtils::toStrKey(of.first)));
        }
    }
}

void
LedgerManagerImpl::advanceLedgerPointers()
{
    CLOG(DEBUG, "Ledger") << "Advancing LCL: "
                          << ledgerAbbrev(mLastClosedLedger) << " -> "
                          << ledgerAbbrev(mCurrentLedger);

    mLastClosedLedger.hash = mCurrentLedger->getHash();
    mLastClosedLedger.header = mCurrentLedger->mHeader;
    mCurrentLedger = make_shared<LedgerHeaderFrame>(mLastClosedLedger);
    CLOG(DEBUG, "Ledger") << "New current ledger: seq="
                          << mCurrentLedger->mHeader.ledgerSeq;
}

void
LedgerManagerImpl::processFeesSeqNums(std::vector<TransactionFramePtr>& txs,
                                      LedgerDelta& delta)
{
    CLOG(DEBUG, "Ledger") << "processing fees and sequence numbers";
    int index = 0;
    try
    {
        soci::transaction sqlTx(mApp.getDatabase().getSession());
        for (auto tx : txs)
        {
            LedgerDelta thisTxDelta(delta);
            tx->processFeeSeqNum(thisTxDelta, *this);
            tx->storeTransactionFee(*this, thisTxDelta.getChanges(), ++index);
            thisTxDelta.commit();
        }
        sqlTx.commit();
    }
    catch (std::exception& e)
    {
        CLOG(FATAL, "Ledger") << "processFeesSeqNums error @ " << index << " : "
                              << e.what();
        throw;
    }
}

void
LedgerManagerImpl::applyTransactions(std::vector<TransactionFramePtr>& txs,
                                     LedgerDelta& ledgerDelta,
                                     TransactionResultSet& txResultSet)
{
    CLOG(DEBUG, "Tx") << "applyTransactions: ledger = "
                      << mCurrentLedger->mHeader.ledgerSeq;
    int index = 0;
    for (auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        LedgerDelta delta(ledgerDelta);
        TransactionMeta tm;
        try
        {
            CLOG(DEBUG, "Tx")
                << " tx#" << index << " = " << hexAbbrev(tx->getFullHash())
                << " txseq=" << tx->getSeqNum() << " (@ "
                << mApp.getConfig().toShortString(tx->getSourceID()) << ")";

            if (tx->apply(delta, tm, mApp))
            {
                delta.commit();
            }
            else
            {
                // failure means there should be no side effects
                assert(delta.getChanges().size() == 0);
                assert(delta.getHeader() == ledgerDelta.getHeader());
            }
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
        tx->storeTransaction(*this, tm, ++index, txResultSet);
    }
}

void
LedgerManagerImpl::closeLedgerHelper(LedgerDelta const& delta)
{
    delta.markMeters(mApp);
    mApp.getBucketManager().addBatch(mApp, mCurrentLedger->mHeader.ledgerSeq,
                                     delta.getLiveEntries(),
                                     delta.getDeadEntries());

    mApp.getBucketManager().snapshotLedger(mCurrentLedger->mHeader);

    mCurrentLedger->storeInsert(*this);

    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(mCurrentLedger->getHash()));

    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(mCurrentLedger->mHeader.ledgerSeq,
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

    advanceLedgerPointers();
}
}
