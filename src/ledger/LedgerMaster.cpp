// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include "clf/CLFMaster.h"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "ledger/LedgerDelta.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "herder/HerderGateway.h"
#include "herder/TxSetFrame.h"
#include "overlay/OverlayGateway.h"
#include "history/HistoryMaster.h"
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
    // TODO.1 wire up catch up. turn off ledger close when catching up.
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
LedgerMaster::ledgerAbbrev(LedgerHeader const& header, uint256 const& hash)
{
    std::ostringstream oss;
    oss << "[seq=" << header.ledgerSeq << ", hash=" << hexAbbrev(hash) << "]";
    return oss.str();
}

std::string
LedgerMaster::ledgerAbbrev(LedgerHeaderFrame::pointer p)
{
    if (!p)
    {
        return "[empty]";
    }
    return ledgerAbbrev(p->mHeader, p->getHash());
}

std::string
LedgerMaster::ledgerAbbrev(LedgerHeaderHistoryEntry he)
{
    return ledgerAbbrev(he.header, he.hash);
}

LedgerMaster::LedgerMaster(Application& app)
    : mApp(app)
    , mTransactionApply(
          app.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
{
    mLastCloseTime = mApp.timeNow(); // this is 0 at this point
}

void
LedgerMaster::startNewLedger()
{
    auto ledgerTime = mLedgerClose.TimeScope();
    ByteSlice bytes("masterpassphrasemasterpassphrase");
    std::string b58SeedStr = toBase58Check(VER_SEED, bytes);
    SecretKey skey = SecretKey::fromBase58Seed(b58SeedStr);
    AccountFrame masterAccount(skey.getPublicKey());
    masterAccount.getAccount().balance = 100000000000000000;
    LedgerHeader genesisHeader;

    LedgerDelta delta(genesisHeader);
    masterAccount.storeAdd(delta, this->getDatabase());

    genesisHeader.baseFee = mApp.getConfig().DESIRED_BASE_FEE;
    genesisHeader.baseReserve = mApp.getConfig().DESIRED_BASE_RESERVE;
    genesisHeader.totalCoins = masterAccount.getAccount().balance;
    genesisHeader.closeTime = 0; // the genesis ledger has close time of 0 so it
                                 // always has the same hash
    genesisHeader.ledgerSeq = 1;
    delta.commit();

    mCurrentLedger = make_shared<LedgerHeaderFrame>(genesisHeader);
    CLOG(INFO, "Ledger") << "Established genesis ledger, closing";
    closeLedgerHelper(delta);
}

void
LedgerMaster::loadLastKnownLedger()
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
        CLOG(INFO, "Ledger")
            << "Loaded last known ledger: " << ledgerAbbrev(mCurrentLedger);

        if (!mCurrentLedger)
        {
            throw std::runtime_error("Could not load ledger from database");
        }

        advanceLedgerPointers();
    }
}

Database&
LedgerMaster::getDatabase()
{
    return mApp.getDatabase();
}

int64_t
LedgerMaster::getTxFee() const
{
    return mCurrentLedger->mHeader.baseFee;
}

int64_t
LedgerMaster::getMinBalance(uint32_t ownerCount) const
{
    return (2 + ownerCount) * mCurrentLedger->mHeader.baseReserve;
}

uint32_t
LedgerMaster::getLedgerNum() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.ledgerSeq;
}

uint64_t
LedgerMaster::getCloseTime() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.closeTime;
}

LedgerHeader const&
LedgerMaster::getCurrentLedgerHeader() const
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader;
}

LedgerHeader&
LedgerMaster::getCurrentLedgerHeader()
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader;
}

LedgerHeaderFrame const&
LedgerMaster::getCurrentLedgerHeaderFrame() const
{
    assert(mCurrentLedger);
    return *mCurrentLedger;
}

LedgerHeaderHistoryEntry const&
LedgerMaster::getLastClosedLedgerHeader() const
{
    return mLastClosedLedger;
}

// called by txherder
void
LedgerMaster::externalizeValue(LedgerCloseData ledgerData)
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
            startCatchUp(mLastClosedLedger.header.ledgerSeq,
                         ledgerData.mLedgerSeq, HistoryMaster::RESUME_AT_LAST);
        }
    }
}

void
LedgerMaster::startCatchUp(uint32_t lastLedger, uint32_t initLedger,
                           HistoryMaster::ResumeMode resume)
{
    mApp.setState(Application::CATCHING_UP_STATE);
    mApp.getHistoryMaster().catchupHistory(
        lastLedger, initLedger, resume,
        std::bind(&LedgerMaster::historyCaughtup, this, _1, _2, _3));
}

void
LedgerMaster::historyCaughtup(asio::error_code const& ec,
                              HistoryMaster::ResumeMode mode,
                              LedgerHeaderHistoryEntry const& lastClosed)
{
    if (ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up: " << ec;
    }
    else
    {
        uint32_t nextLedger = lastClosed.header.ledgerSeq + 1;

        // If we were in RESUME_AT_NEXT mode, LCL has not been updated
        // and we need to pick it up here.
        if (mode == HistoryMaster::RESUME_AT_NEXT)
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
            assert(mode == HistoryMaster::RESUME_AT_LAST);
            assert(lastClosed.hash == mLastClosedLedger.hash);
            assert(lastClosed.header == mLastClosedLedger.header);
        }

        CLOG(INFO, "Ledger") << "Caught up to LCL from history: "
                             << ledgerAbbrev(mLastClosedLedger);

        // Now replay remaining txs from buffered local network history.
        bool applied = false;
        for (auto lcd : mSyncingLedgers)
        {
            if (lcd.mLedgerSeq <= nextLedger)
            {
                assert(lcd.mLedgerSeq !=
                       mLastClosedLedger.header.ledgerSeq + 1);
                continue;
            }

            if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq + 1)
            {
                closeLedger(lcd);
                applied = true;
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
LedgerMaster::secondsSinceLastLedgerClose() const
{
    return mApp.timeNow() - mLastCloseTime;
}

// called by txherder
void
LedgerMaster::closeLedger(LedgerCloseData ledgerData)
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

    SHA256 txResultHasher;
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
                // need delta.rollback

                CLOG(ERROR, "Tx") << "invalid tx. This should never happen";
                CLOG(ERROR, "Tx")
                    << "Transaction: " << xdr::xdr_to_string(tx->getEnvelope());
                CLOG(ERROR, "Tx")
                    << "Result: " << xdr::xdr_to_string(tx->getResult());
            }
            tx->storeTransaction(*this, delta, ++index, txResultHasher);
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
    mCurrentLedger->mHeader.txSetResultHash = txResultHasher.finish();
    closeLedgerHelper(ledgerDelta);
    txscope.commit();

    // Notify ledger close to other components.
    mApp.getHerderGateway().ledgerClosed(mLastClosedLedger);
    mApp.getOverlayGateway().ledgerClosed(mLastClosedLedger);
    mApp.getHistoryMaster().maybePublishHistory([](asio::error_code const&)
                                                {
                                                });
}

void
LedgerMaster::advanceLedgerPointers()
{
    CLOG(INFO, "Ledger") << "Advancing LCL: " << ledgerAbbrev(mLastClosedLedger)
                         << " -> " << ledgerAbbrev(mCurrentLedger);

    mLastClosedLedger.hash = mCurrentLedger->getHash();
    mLastClosedLedger.header = mCurrentLedger->mHeader;
    mCurrentLedger = make_shared<LedgerHeaderFrame>(mLastClosedLedger);
    CLOG(DEBUG, "Ledger") << "New current ledger: seq="
                          << mCurrentLedger->mHeader.ledgerSeq;
}

void
LedgerMaster::closeLedgerHelper(LedgerDelta const& delta)
{
    mLastCloseTime = mApp.timeNow();
    delta.markMeters(mApp);
    mApp.getCLFMaster().addBatch(mApp, mCurrentLedger->mHeader.ledgerSeq,
                                 delta.getLiveEntries(),
                                 delta.getDeadEntries());

    mApp.getCLFMaster().snapshotLedger(mCurrentLedger->mHeader);

    mCurrentLedger->storeInsert(*this);

    mApp.getPersistentState().setState(PersistentState::kLastClosedLedger,
                                       binToHex(mCurrentLedger->getHash()));
    advanceLedgerPointers();
}
}
