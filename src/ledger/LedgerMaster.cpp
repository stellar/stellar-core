// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "LedgerMaster.h"
#include <asio.hpp>
#include "main/Application.h"
#include "main/Config.h"
#include "clf/CLFMaster.h"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "ledger/LedgerDelta.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "crypto/Base58.h"
#include "herder/Herder.h"
#include "database/Database.h"
#include "ledger/LedgerHeaderFrame.h"

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
    1) Wait for FBA to tell us what the network is on now
    2) Ask network for the the delta between what it has now and our ledger last ledger  

    // TODO.1 need to make sure the CLF and the SQL Ledger are in sync on start up
    // TODO.1 make sure you validate incoming Deltas to see that it gives you the CLF you want
    // TODO.3 do we need to store some validation history?

*/
namespace stellar
{

LedgerMaster::LedgerMaster(Application& app) : mApp(app)
{
	mCaughtUp = false;
    //syncWithCLF();
}

void LedgerMaster::startNewLedger()
{
    LOG(INFO) << "Starting a new network";

    ByteSlice bytes("masterpassphrasemasterpassphrase");
    std::string b58SeedStr = toBase58Check(VER_SEED, bytes);
    SecretKey skey = SecretKey::fromBase58Seed(b58SeedStr);
    AccountFrame masterAccount(skey.getPublicKey());
    masterAccount.mEntry.account().balance = 100000000000000;
    Json::Value result;
    masterAccount.storeAdd(result, *this);

    LedgerHeader genenisHeader;
    genenisHeader.baseFee = mApp.getConfig().DESIRED_BASE_FEE;
    genenisHeader.baseReserve = mApp.getConfig().DESIRED_BASE_RESERVE;
    genenisHeader.totalCoins = masterAccount.mEntry.account().balance;
    genenisHeader.ledgerSeq = 1;

    mCurrentLedger = make_shared<LedgerHeaderFrame>(genenisHeader);

    closeLedgerHelper(true);

}

void LedgerMaster::loadLastKnownLedger()
{
    LOG(INFO) << "Loading last known ledger";

    string lastLedger = getState(StoreStateName::kLastClosedLedger);

    if (lastLedger.empty())
    {
        throw std::runtime_error("No ledger in database");
    }

    Hash lastLedgerHash = hexToBin256(lastLedger);

    mCurrentLedger = LedgerHeaderFrame::loadByHash(lastLedgerHash, *this);

    if (!mCurrentLedger)
    {
        throw std::runtime_error("Could not load ledger from database");
    }

    closeLedgerHelper(false);
}

Database &LedgerMaster::getDatabase()
{
    return mApp.getDatabase();
}

int32_t LedgerMaster::getTxFee()
{
    return mCurrentLedger->mHeader.baseFee; 
}

int64_t LedgerMaster::getMinBalance(int32_t ownerCount)
{
    return (2 + ownerCount) * mCurrentLedger->mHeader.baseReserve;
}

int64_t LedgerMaster::getLedgerNum()
{
    return mCurrentLedger->mHeader.ledgerSeq;
}

LedgerHeader& LedgerMaster::getCurrentLedgerHeader()
{
    return mCurrentLedger->mHeader;
}

LedgerHeader& LedgerMaster::getLastClosedLedgerHeader()
{
    return mLastClosedLedger->mHeader;
}

// make sure our state is consistent with the CLF
void LedgerMaster::syncWithCLF()
{
    LedgerHeader const& clfHeader = mApp.getCLFMaster().getHeader();

    if(clfHeader.hash == mCurrentLedger->mHeader.hash)
    {
        CLOG(DEBUG, "Ledger") << "CLF and SQL headers match.";
    } else
    {  // ledgers don't match
        // TODO.3 try to sync them
        CLOG(ERROR, "Ledger") << "CLF and SQL headers don't match. Aborting";
    }
}

// called by txherder
void LedgerMaster::externalizeValue(TxSetFramePtr txSet)
{
    if(mLastClosedLedger->mHeader.hash == txSet->getPreviousLedgerHash())
    {
        mCaughtUp = true;
        closeLedger(txSet);
    }
    else
    { // we need to catch up
        mCaughtUp = false;
        if(mApp.getState() == Application::CATCHING_UP_STATE)
        {  // we are already trying to catch up
            CLOG(DEBUG, "Ledger") << "Missed a ledger while trying to catch up.";
        } else
        {  // start trying to catchup
            startCatchUp();
        }
    }
}

// we have some last ledger that is in the DB
// we need to 
void LedgerMaster::startCatchUp()
{
    mApp.setState(Application::CATCHING_UP_STATE);

}

void LedgerMaster::closeLedger(TxSetFramePtr txSet)
{
    TxSetFrame successfulTX;

    LedgerDelta ledgerDelta;
    
    soci::transaction txscope(getDatabase().getSession());

    vector<TransactionFramePtr> txs;
    txSet->sortForApply(txs);
    for(auto tx : txs)
    {
        try {
            TxDelta delta;
            if(tx->apply(delta, mApp))
            {
                successfulTX.add(tx);
            }

            Json::Value txResult;
            txResult["id"] = binToHex(tx->getContentsHash());
            txResult["code"] = tx->getResultCode();
            txResult["ledger"] = (Json::UInt64)mCurrentLedger->mHeader.ledgerSeq;

            delta.commitDelta(txResult, ledgerDelta, *this );
            mCurrentLedger->mHeader.feePool += delta.getCollectedFee();
            
        }catch(...)
        {
            CLOG(ERROR, "Ledger") << "Exception during tx->apply";
        }
    }

    closeLedgerHelper(true);
    txscope.commit();
}

// helper function that updates the various hashes in the current ledger header
// and switches to a new ledger
void LedgerMaster::closeLedgerHelper(bool updateCurrent)
{
    if (updateCurrent)
    {
        // TODO: give the LedgerDelta to the Bucketlist to compute the new clfHash
        mCurrentLedger->mHeader.clfHash.fill(1);
        // TODO: compute hashes in header
        mCurrentLedger->mHeader.txSetHash.fill(1);
        mCurrentLedger->computeHash();

        mCurrentLedger->storeInsert(*this);

        setState(StoreStateName::kLastClosedLedger, binToHex(mCurrentLedger->mHeader.hash));
    }

    mLastClosedLedger = mCurrentLedger;

    mCurrentLedger = make_shared<LedgerHeaderFrame>(mLastClosedLedger);
}

const char *LedgerMaster::kSQLCreateStatement =
"CREATE TABLE IF NOT EXISTS StoreState (        \
        StateName   CHARACTER(32) PRIMARY KEY,  \
        State       BLOB                        \
);";

void LedgerMaster::dropAll(Database &db)
{
    db.getSession() << "DROP TABLE IF EXISTS StoreState;";

    db.getSession() << kSQLCreateStatement;
}

string LedgerMaster::getStoreStateName(StoreStateName n) {
    static const char *mapping[kLastEntry] = { "lastClosedLedger" };
    if (n < 0 || n >= kLastEntry) {
        throw out_of_range("unknown entry");
    }
    return mapping[n];
}

string LedgerMaster::getState(StoreStateName stateName) {
    string res;

    string sn(getStoreStateName(stateName));

    getDatabase().getSession() << "SELECT State FROM StoreState WHERE StateName = :n;",
        soci::use(sn), soci::into(res);

    if (!getDatabase().getSession().got_data())
    {
        res.clear();
    }

    return res;
}

void LedgerMaster::setState(StoreStateName stateName, const string &value) {
    string sn(getStoreStateName(stateName));

    soci::statement st = (getDatabase().getSession().prepare <<
        "UPDATE StoreState SET State = :v WHERE StateName = :n;",
        soci::use(value), soci::use(sn));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        st = (getDatabase().getSession().prepare <<
            "INSERT INTO StoreState (StateName, State) VALUES (:n, :v );",
            soci::use(sn), soci::use(value));

            st.execute(true);

            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not insert data in SQL");
            }
    }
}

}
