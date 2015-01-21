// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "herder/Herder.h"
#include "fba/FBA.h"
#include "overlay/ItemFetcher.h"
#include "main/Application.h"
#include "main/Config.h"

#include <cassert>
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"


using namespace stellar;

using xdr::operator<;
using xdr::operator==;

typedef std::unique_ptr<Application> appPtr;

// see if we flood at the right times
//  invalid tx
//  normal tx
//  tx with bad seq num
//  account can't pay for all the tx
//  account has just enough for all the tx
//  tx from account not in the DB
TEST_CASE("recvTx", "[hrd]")
{
   

}


// sortForApply 
// sortForHash
// checkValid
//   not sorted correctly
//   tx with bad seq num
//   account can't pay for all the tx
//   account has just enough for all the tx
//   tx from account not in the DB 
TEST_CASE("txset", "[hrd]")
{

}

class OverlayGatewayMock;

class TxSetFetcherMock : public TxSetFetcher
{
  public:
    TxSetFetcherMock(Application& app,
                     std::function<void(uint256)> const& fetch)
        : TxSetFetcher(app)
        , mFetch(fetch)
    {
    }
    void stopFetchingAll()
    {
    }
    void clear()
    {
        mTxSetFrames.clear();
    }
    TxSetFramePtr fetchItem(uint256 const& itemID, bool askNetwork)
    {
        LOG(INFO) << "TxSetFetcherMock::fetchItem"
                  << " " << binToHex(itemID).substr(0,6);
        if (mTxSetFrames.find(itemID) != mTxSetFrames.end())
        {
            return mTxSetFrames[itemID];
        }
        mFetch(itemID);
        return (TxSetFramePtr());
    }
    void doesntHave(uint256 const& itemID, Peer::pointer peer)
    {
        //mTxSetFrames.erase(itemID);
    }
    bool recvItem(TxSetFramePtr txSet)
    {
        LOG(INFO) << "TxSetFetcherMock::recvItem"
                  << " " << binToHex(txSet->getContentsHash()).substr(0,6);

        mTxSetFrames[txSet->getContentsHash()] = txSet;
        return true;
    }

    std::map<uint256, TxSetFramePtr>    mTxSetFrames;
    std::function<void(uint256)>        mFetch;
};

class FBAQSetFetcherMock : public FBAQSetFetcher
{
  public:
    FBAQSetFetcherMock(Application& app,
                       std::function<void(uint256)> const& fetch)
        : FBAQSetFetcher(app)
        , mFetch(fetch)
    {
    }
    void stopFetchingAll()
    {
    }
    void clear()
    {
        mFBAQuorumSets.clear();
    }
    FBAQuorumSetPtr fetchItem(uint256 const& itemID, bool askNetwork)
    {
        LOG(INFO) << "FBAQSetFetcherMock::fetchItem"
                  << " " << binToHex(itemID).substr(0,6);
        if (mFBAQuorumSets.find(itemID) != mFBAQuorumSets.end())
        {
            return mFBAQuorumSets[itemID];
        }
        mFetch(itemID);
        return (FBAQuorumSetPtr());
    }
    void doesntHave(uint256 const& itemID, Peer::pointer peer)
    {
        //mFBAQuorumSets.erase(itemID);
    }
    bool recvItem(FBAQuorumSetPtr qSet)
    {
        uint256 qSetHash = sha512_256(xdr::xdr_to_msg(*qSet));

        LOG(INFO) << "FBAQSetFetcherMock::recvItem"
                  << " " << binToHex(qSetHash).substr(0,6);

        mFBAQuorumSets[qSetHash] = qSet;
        return true;
    }

    std::map<uint256, FBAQuorumSetPtr>  mFBAQuorumSets;
    std::function<void(uint256)>        mFetch;
};

class OverlayGatewayMock : public OverlayGateway
{
  public:
    OverlayGatewayMock(Application& app)
        : mApp(app)
    {
    }

    void ledgerClosed(LedgerHeader& ledger)
    {
        assert(false); // not used
    }

    void broadcastMessage(StellarMessage const& msg)
    {
        mMsgs.push_back(msg);
    }

    TxSetFetcherPtr getNewTxSetFetcher()
    {
        return std::make_shared<TxSetFetcherMock>(
            TxSetFetcherMock(
                mApp, 
                [this] (const uint256& itemID)
                {
                    mFetches.push_back(itemID);
                }));
    }

    FBAQSetFetcherPtr getNewFBAQSetFetcher()
    {
        return std::make_shared<FBAQSetFetcherMock>(
            FBAQSetFetcherMock(
                mApp, 
                [this] (const uint256& itemID)
                {
                    mFetches.push_back(itemID);
                }));
    }

    void recvFloodedMsg(StellarMessage const& msg,
                        Peer::pointer peer)
    {
        assert(false); // not used
    }

    Peer::pointer getRandomPeer()
    {
        assert(false); // not used
        return Peer::pointer();
    }
    Peer::pointer getNextPeer(Peer::pointer peer)
    {
        assert(false); // not used
        return Peer::pointer();
    }

    std::vector<StellarMessage>    mMsgs;
    std::vector<uint256>           mFetches;
    Application&                   mApp;
};

static FBAEnvelope 
makeEnvelope(const uint256& nodeID,
             const Hash& qSetHash,
             const uint64& slotIndex,
             const FBABallot& ballot,
             const FBAStatementType& type)
{
    FBAEnvelope envelope;

    envelope.nodeID = nodeID;
    envelope.statement.slotIndex = slotIndex;
    envelope.statement.ballot = ballot;
    envelope.statement.quorumSetHash = qSetHash;
    envelope.statement.body.type(type);

    return envelope;
}

#define CREATE_NODE(N) \
    const Hash v##N##VSeed = sha512_256("SEED_VALIDATION_SEED_" #N); \
    const Hash v##N##NodeID = makePublicKey(v##N##VSeed);

#define CREATE_DUMMY_VALUE(X) \
    const Hash X##ValueHash = sha512_256("SEED_VALUE_HASH_" #X); \
    const Value X##Value = xdr::xdr_to_opaque(X##ValueHash);


TEST_CASE("mock test", "[hrd]")
{
    CREATE_NODE(0);
    CREATE_NODE(1);
    CREATE_NODE(2);
    CREATE_NODE(3);

    FBAQuorumSetPtr qSet = std::make_shared<FBAQuorumSet>();
    qSet->threshold = 3;
    qSet->validators.push_back(v0NodeID);
    qSet->validators.push_back(v1NodeID);
    qSet->validators.push_back(v2NodeID);
    qSet->validators.push_back(v3NodeID);

    uint256 qSetHash = sha512_256(xdr::xdr_to_msg(*qSet));

    Config::pointer cfg = std::make_shared<Config>();
    cfg->LOG_FILE_PATH = getTestConfig().LOG_FILE_PATH;
    cfg->RUN_STANDALONE = true;

    cfg->VALIDATION_SEED = v0VSeed;
    cfg->QUORUM_THRESHOLD = qSet->threshold;
    cfg->QUORUM_SET.push_back(v0NodeID);
    cfg->QUORUM_SET.push_back(v1NodeID);
    cfg->QUORUM_SET.push_back(v2NodeID);
    cfg->QUORUM_SET.push_back(v3NodeID);

    TxSetFramePtr txSetEmpty = std::make_shared<TxSetFrame>();
    txSetEmpty->mPreviousLedgerHash = sha512_256("SEED_PREVIOUS_LEDGER_HASH");

    StellarBallot bEmpty;
    bEmpty.txSetHash = txSetEmpty->getContentsHash();

    CREATE_DUMMY_VALUE(x);

    VirtualClock clock;
    Application app(clock, *cfg);

    // We explicitely don't start the app as we'll be using the mocked
    // OverlayGateway in this test.

    OverlayGatewayMock* overlayMock = new OverlayGatewayMock(app);
    app.mockOverlayGateway(std::unique_ptr<OverlayGatewayMock>(overlayMock));
    Herder h(app);

    LOG(INFO) << "<<<< BEGIN FBA TEST >>>>";

    SECTION("before SYNCED_STATE")
    {
        SECTION("receive malformed FBA Envelope")
        {
            FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xValue),
                                                FBAStatementType::PREPARE);
            h.recvFBAEnvelope(
                prepare1, 
                [] (bool valid) 
                {
                    REQUIRE(!valid);
                });
            REQUIRE(overlayMock->mMsgs.size() == 0);
        }
        SECTION("receive correct FBA Envelope txSet,qSet not cached")
        {
            FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xdr::xdr_to_opaque(bEmpty)),
                                                FBAStatementType::PREPARE);
            h.recvFBAEnvelope(prepare1);

            REQUIRE(overlayMock->mMsgs.size() == 0);

            REQUIRE(overlayMock->mFetches.size() == 1);
            REQUIRE(overlayMock->mFetches[0] == txSetEmpty->getContentsHash());
                
        }
        SECTION("receive correct FBA Envelope qSet not cached")
        {
            FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xdr::xdr_to_opaque(bEmpty)),
                                                FBAStatementType::PREPARE);

            h.recvTxSet(txSetEmpty);
            h.recvFBAEnvelope(prepare1);

            REQUIRE(overlayMock->mMsgs.size() == 0);

            REQUIRE(overlayMock->mFetches.size() == 2);
            REQUIRE(overlayMock->mFetches[0] == qSetHash);
            REQUIRE(overlayMock->mFetches[1] == qSetHash);
        }
        SECTION("receive correct FBA Envelope qSet,txSet cached")
        {
            FBAEnvelope prepare1 = makeEnvelope(v1NodeID, qSetHash, 0,
                                                FBABallot(0, xdr::xdr_to_opaque(bEmpty)),
                                                FBAStatementType::PREPARE);

            h.recvTxSet(txSetEmpty);
            h.recvFBAQuorumSet(qSet);
            h.recvFBAEnvelope(prepare1);

            h.recvFBAEnvelope(
                prepare1, 
                [] (bool valid) 
                {
                    // The envelope should be accepted (as we're not synced up)
                    REQUIRE(valid);
                });
            // But we should not send envelopes ourselves yet.
            REQUIRE(overlayMock->mMsgs.size() == 0);

            REQUIRE(overlayMock->mFetches.size() == 0);
        }
    }

    SECTION("fully synced")
    {
        
    }
}
