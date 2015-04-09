// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "history/HistoryManager.h"
#include "ledger/LedgerDelta.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManagerImpl.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/make_unique.h"

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

    // TODO.3 we need to store some validation history?
*/
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using namespace std;

namespace stellar
{

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
    , mSyncingLedgersSize(
          app.getMetrics().NewCounter({"ledger", "memory", "syncing-ledgers"}))
    , mState(LM_BOOTING_STATE)

{
    mLastCloseTime = mApp.timeNow(); // this is 0 at this point
}

void
LedgerManagerImpl::setState(State s)
{
    if (s != getState())
    {
        std::string oldState = getStateHuman();
        mState = s;
        CLOG(INFO, "Ledger") << "Changing state " << oldState << " -> "
                             << getStateHuman();
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
    auto ledgerTime = mLedgerClose.TimeScope();
    ByteSlice bytes("allmylifemyhearthasbeensearching");
    std::string b58SeedStr = toBase58Check(VER_SEED, bytes);
    SecretKey skey = SecretKey::fromBase58Seed(b58SeedStr);

    AccountFrame masterAccount(skey.getPublicKey());
    masterAccount.getAccount().balance = 100000000000000000;
    LedgerHeader genesisHeader;

    genesisHeader.baseFee = mApp.getConfig().DESIRED_BASE_FEE;
    genesisHeader.baseReserve = mApp.getConfig().DESIRED_BASE_RESERVE;
    genesisHeader.totalCoins = masterAccount.getAccount().balance;
    genesisHeader.closeTime = 0; // the genesis ledger has close time of 0 so it
                                 // always has the same hash
    genesisHeader.ledgerSeq = 1;

    LedgerDelta delta(genesisHeader);
    masterAccount.storeAdd(delta, this->getDatabase());
    delta.commit();

    mCurrentLedger = make_shared<LedgerHeaderFrame>(genesisHeader);
    CLOG(INFO, "Ledger") << "Established genesis ledger, closing";
    closeLedgerHelper(delta);
}

void
LedgerManagerImpl::loadLastKnownLedger(
    function<void(asio::error_code const& ec)> handler)
{
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

        auto missing = mApp.getBucketManager().checkForMissingBucketsFiles(has);
        if (!missing.empty())
        {
            CLOG(WARNING, "Ledger") << "Some buckets are missing in '"
                                    << mApp.getBucketManager().getBucketDir()
                                    << "'.";
            CLOG(WARNING, "Ledger")
                << "Attempting to recover from the history store.";
            mApp.getHistoryManager().downloadMissingBuckets(has, continuation);
        }
        else
        {
            continuation(asio::error_code());
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

int64_t
LedgerManagerImpl::getMinBalance(uint32_t ownerCount) const
{
    return (2 + ownerCount) * mCurrentLedger->mHeader.baseReserve;
}

uint32_t
LedgerManagerImpl::getLedgerNum() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.ledgerSeq;
}

uint64_t
LedgerManagerImpl::getCloseTime() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.closeTime;
}

LedgerHeader const&
LedgerManagerImpl::getCurrentLedgerHeader() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader;
}

LedgerHeader&
LedgerManagerImpl::getCurrentLedgerHeader()
{
    assert(mCurrentLedger);
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

// called by txherder
void
LedgerManagerImpl::externalizeValue(LedgerCloseData ledgerData)
{
    CLOG(INFO, "Ledger") << "Got consensus: "
                         << "[seq=" << ledgerData.mLedgerSeq << ", prev="
                         << hexAbbrev(ledgerData.mTxSet->previousLedgerHash())
                         << ", time=" << ledgerData.mCloseTime
                         << ", txs=" << ledgerData.mTxSet->size() << ", txhash="
                         << hexAbbrev(ledgerData.mTxSet->getContentsHash())
                         << ", fee=" << ledgerData.mBaseFee << "]";

    // ledgerAbbrev(ledgerData.mLedgerSeq-1,
    //              ledgerData.mTxSet->previousLedgerHash())

    switch (getState())
    {
    case LedgerManager::LM_BOOTING_STATE:
    case LedgerManager::LM_SYNCED_STATE:
        if (mLastClosedLedger.header.ledgerSeq + 1 == ledgerData.mLedgerSeq)
        {
            if (mLastClosedLedger.hash ==
                ledgerData.mTxSet->previousLedgerHash())
            {
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
            startCatchUp(ledgerData.mLedgerSeq,
                         mApp.getConfig().CATCHUP_COMPLETE
                             ? HistoryManager::CATCHUP_COMPLETE
                             : HistoryManager::CATCHUP_MINIMAL);
        }
        break;

    case LedgerManager::LM_CATCHING_UP_STATE:
        if (mSyncingLedgers.empty() ||
            mSyncingLedgers.back().mLedgerSeq + 1 == ledgerData.mLedgerSeq)
        {
            // Normal close while catching up
            mSyncingLedgers.push_back(ledgerData);
            mSyncingLedgersSize.set_count(mSyncingLedgers.size());

            uint64_t now = mApp.timeNow();
            uint64_t eta = mSyncingLedgers.front().mCloseTime +
                           mApp.getHistoryManager().nextCheckpointCatchupProbe(
                               mSyncingLedgers.front().mLedgerSeq);

            CLOG(INFO, "Ledger") << "Catchup awaiting checkpoint"
                                 << " (ETA: " << (now > eta ? 0 : (eta - now))
                                 << " seconds), buffering close of ledger "
                                 << ledgerData.mLedgerSeq;
        }
        else
        {
            // Out-of-order close while catching up; timeout / network failure?
            CLOG(INFO, "Ledger")
                << "Out-of-order close during catchup, buffered to "
                << mSyncingLedgers.back().mLedgerSeq << " but network closed "
                << ledgerData.mLedgerSeq;
            CLOG(WARNING, "Ledger") << "this round of catchup will fail.";
            assert(!mSyncingLedgers.empty());
        }
        break;

    default:
        assert(false);
    }
}

void
LedgerManagerImpl::startCatchUp(uint32_t initLedger,
                                HistoryManager::CatchupMode resume)
{
    setState(LM_CATCHING_UP_STATE);
    mApp.getHistoryManager().catchupHistory(
        initLedger, resume,
        std::bind(&LedgerManagerImpl::historyCaughtup, this, _1, _2, _3));
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
    }
    else
    {
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
            using xdr::operator==;
            assert(mode == HistoryManager::CATCHUP_COMPLETE);
            assert(lastClosed.hash == mLastClosedLedger.hash);
            assert(lastClosed.header == mLastClosedLedger.header);
        }

        CLOG(INFO, "Ledger") << "Caught up to LCL from history: "
                             << ledgerAbbrev(mLastClosedLedger);

        // Now replay remaining txs from buffered local network history.
        bool applied = false;
        for (auto lcd : mSyncingLedgers)
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
                           mLastClosedLedger.header.txSetHash);
                    assert(lcd.mBaseFee == mLastClosedLedger.header.baseFee);
                    assert(lcd.mCloseTime ==
                           mLastClosedLedger.header.closeTime);
                }

                continue;
            }
            else if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq + 1)
            {
                CLOG(INFO, "Ledger") << "Replaying buffered ledger-close for "
                                     << lcd.mLedgerSeq;
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
                startCatchUp(lastBuffered.mLedgerSeq,
                             mApp.getConfig().CATCHUP_COMPLETE
                                 ? HistoryManager::CATCHUP_COMPLETE
                                 : HistoryManager::CATCHUP_MINIMAL);
                return;
            }
        }

        // we're done processing the ledgers backlog
        mSyncingLedgers.clear();

        CLOG(INFO, "Ledger")
            << "Caught up to LCL including recent network activity: "
            << ledgerAbbrev(mLastClosedLedger);

        mSyncingLedgersSize.set_count(mSyncingLedgers.size());
        setState(LM_SYNCED_STATE);
    }
}

uint64_t
LedgerManagerImpl::secondsSinceLastLedgerClose() const
{
    return mApp.timeNow() - mLastCloseTime;
}

/*
    This is the main method that closes the current ledger based on
the close context that was computed by SCP or by the historical module
during replays.

*/
void
LedgerManagerImpl::closeLedger(LedgerCloseData ledgerData)
{
    CLOG(DEBUG, "Ledger") << "starting closeLedger() on ledgerSeq="
                          << mCurrentLedger->mHeader.ledgerSeq;

    if (ledgerData.mTxSet->previousLedgerHash() !=
        getLastClosedLedgerHeader().hash)
    {
        throw std::runtime_error("txset mismatch");
    }

    LedgerDelta ledgerDelta(mCurrentLedger->mHeader);

    soci::transaction txscope(getDatabase().getSession());

    auto ledgerTime = mLedgerClose.TimeScope();

    // the transaction set that was agreed upon by consensus
    // was sorted by hash; we reorder it so that transactions are
    // sorted such that sequence numbers are respected
    vector<TransactionFramePtr> txs = ledgerData.mTxSet->sortForApply();
    int index = 0;

    auto txResultHasher = SHA256::create();
    for (auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        LedgerDelta delta(ledgerDelta);
        try
        {
            CLOG(DEBUG, "Tx") << "APPLY: ledger "
                              << mCurrentLedger->mHeader.ledgerSeq << " tx#"
                              << index << " = " << hexAbbrev(tx->getFullHash())
                              << " txseq=" << tx->getSeqNum() << " (@ "
                              << hexAbbrev(tx->getSourceID()) << ")";

            // note that success here just means it got processed
            // a failed transaction collecting a fee is successful at this layer
            if (tx->apply(delta, mApp))
            {
                delta.commit();
            }
            else
            {
                // transaction failed validation and cannot have side effects
                tx->getResult().feeCharged = 0;
            }
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply: " << e.what();
            tx->getResult().result.code(txINTERNAL_ERROR);
            tx->getResult().feeCharged = 0;
        }
        catch (...)
        {
            CLOG(ERROR, "Ledger") << "Unknown exception during tx->apply";
            tx->getResult().result.code(txINTERNAL_ERROR);
            tx->getResult().feeCharged = 0;
        }
        if (tx->getResult().feeCharged == 0)
        {
            CLOG(ERROR, "Tx") << "invalid tx";
            CLOG(ERROR, "Tx")
                << "Transaction: " << xdr::xdr_to_string(tx->getEnvelope());
            CLOG(ERROR, "Tx")
                << "Result: " << xdr::xdr_to_string(tx->getResult());
            // ensures that this transaction doesn't have any side effects
            delta.rollback();
        }
        tx->storeTransaction(*this, delta, ++index, *txResultHasher);
    }
    ledgerDelta.commit();
    mCurrentLedger->mHeader.baseFee = ledgerData.mBaseFee;
    mCurrentLedger->mHeader.closeTime = ledgerData.mCloseTime;
    mCurrentLedger->mHeader.txSetHash = ledgerData.mTxSet->getContentsHash();
    mCurrentLedger->mHeader.txSetResultHash = txResultHasher->finish();
    closeLedgerHelper(ledgerDelta);
    txscope.commit();

    // Notify ledger close to other components.
    mApp.getHistoryManager().maybePublishHistory([](asio::error_code const&)
                                                 {
                                                 });

    // Permit BucketManager to forget buckets that are no longer in use.
    mApp.getBucketManager().forgetUnreferencedBuckets();
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
LedgerManagerImpl::closeLedgerHelper(LedgerDelta const& delta)
{
    mLastCloseTime = mApp.timeNow();
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
    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString());

    advanceLedgerPointers();
}
}
