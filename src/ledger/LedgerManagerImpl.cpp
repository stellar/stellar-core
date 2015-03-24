// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerManagerImpl.h"
#include "main/Application.h"
#include "main/Config.h"
#include "clf/CLFManager.h"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "ledger/LedgerDelta.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "overlay/OverlayManager.h"
#include "history/HistoryManager.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "xdrpp/printer.h"
#include "xdrpp/types.h"
#include <chrono>
#include <sstream>

/*
The ledger module:
    1) gets the externalized tx set
    2) applies this set to the previous ledger
    3) sends the resultMeta somewhere
    4) sends the changed entries to the CLF
    5) saves the changed entries to SQL
    6) saves the ledger hash and header to SQL
    7) sends the new ledger hash and the tx set to the history
    8) sends the new ledger hash and header to the Herder


catching up to network:
    1) Wait for SCP to tell us what the network is on now
    2) Ask network for the the delta between what it has now and our ledger last
ledger

    // TODO.3 we need to store some validation history?
    // TODO.3 better way to handle quorums when you are booting a network for
instance if all nodes fail.

*/
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using namespace std;

namespace stellar
{

std::string
LedgerManagerImpl::ledgerAbbrev(LedgerHeader const& header, uint256 const& hash)
{
    std::ostringstream oss;
    oss << "[seq=" << header.ledgerSeq << ", hash=" << hexAbbrev(hash) << "]";
    return oss.str();
}

std::string
LedgerManagerImpl::ledgerAbbrev(LedgerHeaderFrame::pointer p)
{
    if (!p)
    {
        return "[empty]";
    }
    return ledgerAbbrev(p->mHeader, p->getHash());
}

std::string
LedgerManagerImpl::ledgerAbbrev(LedgerHeaderHistoryEntry he)
{
    return ledgerAbbrev(he.header, he.hash);
}

LedgerManagerImpl::LedgerManagerImpl(Application& app)
    : mApp(app)
    , mTransactionApply(
          app.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
{
    mLastCloseTime = mApp.timeNow(); // this is 0 at this point
}

void
LedgerManagerImpl::startNewLedger()
{
    auto ledgerTime = mLedgerClose.TimeScope();
    ByteSlice bytes("masterpassphrasemasterpassphrase");
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
LedgerManagerImpl::loadLastKnownLedger()
{
    auto ledgerTime = mLedgerClose.TimeScope();

    string lastLedger =
        mApp.getPersistentState().getState(PersistentState::kLastClosedLedger);

    if (lastLedger.empty())
    {
        LOG(INFO) << "No ledger in the DB. Storing ledger 0.";
        startNewLedger();
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

        string hasString =
            mApp.getPersistentState().getState(PersistentState::kHistoryArchiveState);
        HistoryArchiveState has;
        has.fromString(hasString);
        mApp.getCLFManager().assumeState(has);

        CLOG(INFO, "Ledger")
            << "Loaded last known ledger: " << ledgerAbbrev(mCurrentLedger);

        advanceLedgerPointers();
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
    if (mLastClosedLedger.hash == ledgerData.mTxSet->previousLedgerHash())
    {
        closeLedger(ledgerData);
    }
    else
    {
        // Out of sync, buffer what we just heard.
        CLOG(DEBUG, "Ledger")
            << "Out of sync with network, buffering externalized value for "
               "ledgerSeq=" << ledgerData.mLedgerSeq;

        mSyncingLedgers.push_back(ledgerData);

        if (mApp.getState() == Application::CATCHING_UP_STATE)
        {
            // We are already trying to catch up.
            CLOG(DEBUG, "Ledger") << "Catchup in progress";
        }
        else
        {
            // Start trying to catchup.
            CLOG(DEBUG, "Ledger") << "Starting catchup";
            startCatchUp(ledgerData.mLedgerSeq, HistoryManager::RESUME_AT_LAST);
        }
    }
}

void
LedgerManagerImpl::startCatchUp(uint32_t initLedger, HistoryManager::ResumeMode resume)
{
    mApp.setState(Application::CATCHING_UP_STATE);
    mApp.getHistoryManager().catchupHistory(
        initLedger, resume,
        std::bind(&LedgerManagerImpl::historyCaughtup, this, _1, _2, _3));
}

HistoryManager::VerifyHashStatus
LedgerManagerImpl::verifyCatchupCandidate(LedgerHeaderHistoryEntry const& candidate) const
{
    // This is a callback from CatchupStateMachine when it's considering whether
    // to treat a retrieved history block as legitimate. It asks LedgerManagerImpl if
    // it's seen (in its previous, current, or buffer of ledgers-to-close that
    // have queued up since catchup began) whether it believes the candidate is a
    // legitimate part of history. LedgerManagerImpl is allowed to answer "unknown"
    // here, which causes CatchupStateMachine to pause and retry later.

#define CHECK_PAIR(aseq,bseq,ahash,bhash)                               \
    if ((aseq) == (bseq))                                               \
    {                                                                   \
        if ((ahash) == (bhash))                                         \
        {                                                               \
            return HistoryManager::VERIFY_HASH_OK;                       \
        }                                                               \
        else                                                            \
        {                                                               \
            return HistoryManager::VERIFY_HASH_BAD;                      \
        }                                                               \
    }

    CHECK_PAIR(mLastClosedLedger.header.ledgerSeq, candidate.header.ledgerSeq,
               mLastClosedLedger.hash, candidate.hash);

    CHECK_PAIR(mLastClosedLedger.header.ledgerSeq, candidate.header.ledgerSeq + 1,
               mLastClosedLedger.header.previousLedgerHash, candidate.hash);

    CHECK_PAIR(mCurrentLedger->mHeader.ledgerSeq, candidate.header.ledgerSeq + 1,
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
                              HistoryManager::ResumeMode mode,
                              LedgerHeaderHistoryEntry const& lastClosed)
{
    if (ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up: " << ec;
    }
    else
    {
        // If we were in RESUME_AT_NEXT mode, LCL has not been updated
        // and we need to pick it up here.
        if (mode == HistoryManager::RESUME_AT_NEXT)
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
            assert(mode == HistoryManager::RESUME_AT_LAST);
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
                    assert(lcd.mTxSet->getContentsHash() == mLastClosedLedger.header.txSetHash);
                    assert(lcd.mBaseFee == mLastClosedLedger.header.baseFee);
                    assert(lcd.mCloseTime == mLastClosedLedger.header.closeTime);
                }

                continue;

            }
            else if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq + 1)
            {
                CLOG(INFO, "Ledger")
                    << "Replaying buffered ledger-close for " << lcd.mLedgerSeq;
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
                // So if we ever get here, something was seriously wrong and we
                // should either fail the process or, at very least, flush
                // everything we did during catchup and restart the process
                // anew.

                assert(lcd.mLedgerSeq > mLastClosedLedger.header.ledgerSeq + 1);
                auto const& lastBuffered = mSyncingLedgers.back();
                CLOG(ERROR, "Ledger")
                    << "Catchup failed to buffer contiguous ledger chain";
                CLOG(ERROR, "Ledger")
                    << "LCL is " << ledgerAbbrev(mLastClosedLedger)
                    << ", trying to apply buffered close " << lcd.mLedgerSeq
                    << " with txhash " << hexAbbrev(lcd.mTxSet->getContentsHash());
                CLOG(ERROR, "Ledger")
                    << "Flushing buffer and restarting at ledger "
                    << lastBuffered.mLedgerSeq;
                mSyncingLedgers.clear();
                startCatchUp(lastBuffered.mLedgerSeq,
                             HistoryManager::RESUME_AT_LAST);
            }
        }
        if (applied)
            mSyncingLedgers.clear();

        CLOG(INFO, "Ledger")
            << "Caught up to LCL including recent network activity: "
            << ledgerAbbrev(mLastClosedLedger);

        mApp.setState(Application::SYNCED_STATE);
    }
}

uint64_t
LedgerManagerImpl::secondsSinceLastLedgerClose() const
{
    return mApp.timeNow() - mLastCloseTime;
}

// called by txherder
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

    vector<TransactionFramePtr> txs = ledgerData.mTxSet->sortForApply();
    int index = 0;

    auto txResultHasher = SHA256::create();
    for (auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        try
        {
            LedgerDelta delta(ledgerDelta);

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
                tx->getResult().feeCharged = 0;
                // ensures that this transaction doesn't have any side effects
                delta.rollback();

                CLOG(ERROR, "Tx") << "invalid tx. This should never happen";
                CLOG(ERROR, "Tx")
                    << "Transaction: " << xdr::xdr_to_string(tx->getEnvelope());
                CLOG(ERROR, "Tx")
                    << "Result: " << xdr::xdr_to_string(tx->getResult());
            }
            tx->storeTransaction(*this, delta, ++index, *txResultHasher);
        }
        catch (std::runtime_error& e)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply: " << e.what();
        }
        catch (...)
        {
            CLOG(ERROR, "Ledger") << "Unknown exception during tx->apply";
        }
    }
    ledgerDelta.commit();
    mCurrentLedger->mHeader.baseFee = ledgerData.mBaseFee;
    mCurrentLedger->mHeader.closeTime = ledgerData.mCloseTime;
    mCurrentLedger->mHeader.txSetHash = ledgerData.mTxSet->getContentsHash();
    mCurrentLedger->mHeader.txSetResultHash = txResultHasher->finish();
    closeLedgerHelper(ledgerDelta);
    txscope.commit();

    // Notify ledger close to other components.
    mApp.getHerder().ledgerClosed(mLastClosedLedger);
    mApp.getOverlayManager().ledgerClosed(mLastClosedLedger);
    mApp.getHistoryManager().maybePublishHistory([](asio::error_code const&)
                                                {
                                                });

    // Permit CLF manager to forget buckets that are no longer in use.
    mApp.getCLFManager().forgetUnreferencedBuckets();
}

void
LedgerManagerImpl::advanceLedgerPointers()
{
//    CLOG(INFO, "Ledger") << "Advancing LCL: " << ledgerAbbrev(mLastClosedLedger)
    //                        << " -> " << ledgerAbbrev(mCurrentLedger);

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
    mApp.getCLFManager().addBatch(mApp, mCurrentLedger->mHeader.ledgerSeq,
                                  delta.getLiveEntries(),
                                  delta.getDeadEntries());

    mApp.getCLFManager().snapshotLedger(mCurrentLedger->mHeader);

    mCurrentLedger->storeInsert(*this);

    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(mCurrentLedger->getHash()));

    // Store the current HAS in the database; this is really just to checkpoint
    // the bucketlist so we can survive a restart and re-attach to the buckets.
    HistoryArchiveState has(mCurrentLedger->mHeader.ledgerSeq,
                            mApp.getCLFManager().getBucketList());
    mApp.getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                       has.toString());

    advanceLedgerPointers();
}
}
