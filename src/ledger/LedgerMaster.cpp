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
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"
#include "herder/HerderGateway.h"
#include "herder/TxSetFrame.h"
#include "overlay/OverlayGateway.h"
#include "history/HistoryMaster.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include <chrono>

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
    2) Ask network for the the delta between what it has now and our ledger last ledger  

    // TODO.3 we need to store some validation history?
    // TODO.1 wire up catch up. turn off ledger close when catching up. 
    // TODO.3 better way to handle quorums when you are booting a network for instance if all nodes fail.

*/
using std::placeholders::_1;
using std::placeholders::_2;
using namespace std;

namespace stellar
{

LedgerMaster::LedgerMaster(Application& app)
    : mApp(app)
    , mTransactionApply(app.getMetrics().NewTimer({"ledger", "transaction", "apply"}))
    , mLedgerClose(app.getMetrics().NewTimer({"ledger", "ledger", "close"}))
{
    mLastCloseTime = mApp.timeNow(); // this is 0 at this point
}



void LedgerMaster::startNewLedger()
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
    genesisHeader.closeTime = 0; // the genesis ledger has close time of 0 so it always has the same hash
    genesisHeader.ledgerSeq = 1;
    delta.commit();

    mCurrentLedger = make_shared<LedgerHeaderFrame>(genesisHeader);

    closeLedgerHelper(true, delta);

}

void LedgerMaster::loadLastKnownLedger()
{
    auto ledgerTime = mLedgerClose.TimeScope();

    string lastLedger = mApp.getPersistentState().getState(PersistentState::kLastClosedLedger);

    if (lastLedger.empty())
    {  
        LOG(INFO) << "No ledger in the DB. Storing ledger 0.";
        startNewLedger();
    } else 
    {
        LOG(INFO) << "Loading last known ledger";
        Hash lastLedgerHash = hexToBin256(lastLedger);

        mCurrentLedger = LedgerHeaderFrame::loadByHash(lastLedgerHash, *this);

        if (!mCurrentLedger)
        {
            throw std::runtime_error("Could not load ledger from database");
        }

        LedgerDelta delta(mCurrentLedger->mHeader);

        closeLedgerHelper(false, delta);
    }
}

Database &LedgerMaster::getDatabase()
{
    return mApp.getDatabase();
}

int64_t LedgerMaster::getTxFee()
{
    return mCurrentLedger->mHeader.baseFee; 
}

int64_t LedgerMaster::getMinBalance(uint32_t ownerCount)
{
    return (2 + ownerCount) * mCurrentLedger->mHeader.baseReserve;
}

uint32_t LedgerMaster::getLedgerNum()
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.ledgerSeq;
}

uint64_t LedgerMaster::getCloseTime()
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader.closeTime;
}

LedgerHeader& LedgerMaster::getCurrentLedgerHeader()
{
    assert(mCurrentLedger);
    return mCurrentLedger->mHeader;
}

LedgerHeaderFrame& LedgerMaster::getCurrentLedgerHeaderFrame()
{
    assert(mCurrentLedger);
    return *mCurrentLedger;
}

LedgerHeaderHistoryEntry& LedgerMaster::getLastClosedLedgerHeader()
{
    return mLastClosedLedger;
}


// called by txherder
void LedgerMaster::externalizeValue(LedgerCloseData ledgerData)
{
    if(mLastClosedLedger.hash == ledgerData.mTxSet->getPreviousLedgerHash())
    {
        closeLedger(ledgerData);
    }
    else
    { // we need to catch up
        mSyncingLedgers.push_back(ledgerData);
        if(mApp.getState() == Application::CATCHING_UP_STATE)
        {  // we are already trying to catch up
            CLOG(DEBUG, "Ledger") << "Missed a ledger while trying to catch up.";
        } else
        {  // start trying to catchup
            startCatchUp(ledgerData.mLedgerSeq);
        }
    }
}


void LedgerMaster::startCatchUp(uint64_t initLedger)
{
    mApp.setState(Application::CATCHING_UP_STATE);
    mApp.getHistoryMaster().catchupHistory(
        mLastClosedLedger.header.ledgerSeq,
        initLedger,
        HistoryMaster::RESUME_AT_LAST,
        std::bind(&LedgerMaster::historyCaughtup, this, _1, _2));

}

void LedgerMaster::historyCaughtup(asio::error_code const& ec,
                                   uint64_t nextLedger)
{
    if(ec)
    {
        CLOG(ERROR, "Ledger") << "Error catching up" << ec;
    } else
    {   
        bool applied = false;
        for(auto lcd : mSyncingLedgers)
        {
            if (lcd.mLedgerSeq <= nextLedger)
            {
                assert(lcd.mLedgerSeq != mLastClosedLedger.header.ledgerSeq + 1);
                continue;
            }

            if (lcd.mLedgerSeq == mLastClosedLedger.header.ledgerSeq + 1)
            {
                closeLedger(lcd);
                applied = true;
            }
        }
        if(applied) mSyncingLedgers.clear();

        mApp.setState(Application::SYNCED_STATE);
    }
}

uint64_t LedgerMaster::secondsSinceLastLedgerClose()
{
    return mApp.timeNow() - mLastCloseTime;
}

// called by txherder
void LedgerMaster::closeLedger(LedgerCloseData ledgerData)
{
    CLOG(INFO, "Ledger")
        << "starting closeLedger() on ledgerSeq="
        << mCurrentLedger->mHeader.ledgerSeq;

    TxSetFrame successfulTX;

    LedgerDelta ledgerDelta(mCurrentLedger->mHeader);

    soci::transaction txscope(getDatabase().getSession());

    auto ledgerTime = mLedgerClose.TimeScope();

    vector<TransactionFramePtr> txs;
    ledgerData.mTxSet->sortForApply(txs);
    for(auto tx : txs)
    {
        auto txTime = mTransactionApply.TimeScope();
        try
        {
            LedgerDelta delta(ledgerDelta);

            // note that successfulTX here just means it got processed
            // a failed transaction collecting a fee is successful at this layer
            if(tx->apply(delta, mApp))
            {
                successfulTX.add(tx);
                tx->storeTransaction(*this, delta);
                delta.commit();
            }
            else
            {
                CLOG(ERROR, "Tx") << "invalid tx. This should never happen";
            }

        }
        catch(...)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply";
        }
    }
    ledgerDelta.commit();
    mCurrentLedger->mHeader.baseFee = ledgerData.mBaseFee;
    mCurrentLedger->mHeader.closeTime = ledgerData.mCloseTime;
    closeLedgerHelper(true, ledgerDelta);
    txscope.commit();

    // Notify ledger close to other components.
    mApp.getHerderGateway().ledgerClosed(mLastClosedLedger);
    mApp.getOverlayGateway().ledgerClosed(mLastClosedLedger);
}


// LATER: maybe get rid of the updateCurrent condition unless more happens if it is false
// helper function that updates the various hashes in the current ledger header
// and switches to a new ledger
void LedgerMaster::closeLedgerHelper(bool updateCurrent, LedgerDelta const& delta)
{
    mLastCloseTime = mApp.timeNow();

    delta.markMeters(mApp);
    if (updateCurrent)
    {
        mApp.getCLFMaster().addBatch(mApp,
            mCurrentLedger->mHeader.ledgerSeq,
            delta.getLiveEntries(), delta.getDeadEntries());

        mApp.getCLFMaster().snapshotLedger(mCurrentLedger->mHeader);

        // TODO: compute hashes in header
        mCurrentLedger->mHeader.txSetHash.fill(1);
        mCurrentLedger->storeInsert(*this);

        mApp.getPersistentState().setState(PersistentState::kLastClosedLedger, binToHex(mCurrentLedger->getHash()));
    }

    CLOG(INFO, "Ledger")
        << "closeLedgerHelper() closed ledgerSeq="
        << mCurrentLedger->mHeader.ledgerSeq
        << " with hash="
        << binToHex(mCurrentLedger->getHash());
  
    mLastClosedLedger.hash = mCurrentLedger->getHash();
    mLastClosedLedger.header = mCurrentLedger->mHeader;

    mCurrentLedger = make_shared<LedgerHeaderFrame>(mLastClosedLedger);
}

 
}
