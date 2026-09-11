// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Curve25519.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/FlowControl.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerDoor.h"
#include "overlay/TCPPeer.h"
#include "overlay/test/OverlayTestUtils.h"
#include "simulation/Simulation.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/Timer.h"
#include <future>
#include <thread>

using namespace stellar::overlaytestutils;

namespace stellar
{
// Drives the real TCPPeer read loop and main-thread handlers over a loopback
// socket. The fixture only controls when bytes become available to the peer.
class TCPPeerHandshakeTests
{
    VirtualClock mClock{VirtualClock::REAL_TIME};

  public:
    Application::pointer mApp;

  private:
    asio::io_context mSocketContext;
    asio::ip::tcp::socket mSender{mSocketContext};
    std::shared_ptr<TCPPeer::SocketType> mSocket;
    HmacSha256Key mMacKey;
    Hmac mSenderHmac;
    bool mSentHello{false};

  public:
    TCPPeer::pointer mPeer;
    StellarMessage mHello;
    // Frames written to the peer, and the offset at which each one ends.
    std::vector<uint8_t> mBytes;
    std::vector<size_t> mFrameEnds;

    TCPPeerHandshakeTests()
    {
        auto cfg = getTestConfig();
        cfg.BACKGROUND_OVERLAY_PROCESSING = true;
        cfg.FORCE_SCP = false;
        mApp = createTestApplication(mClock, cfg);

        asio::ip::tcp::acceptor acceptor(mSocketContext,
                                         {asio::ip::address_v4::loopback(), 0});
        mSender.connect(acceptor.local_endpoint());
        mSocket = std::make_shared<TCPPeer::SocketType>(
            mApp->getOverlayIOContext(), TCPPeer::BUFSZ);
        acceptor.accept(mSocket->next_layer());
        mPeer = std::make_shared<TCPPeer>(*mApp, Peer::REMOTE_CALLED_US,
                                          mSocket, "127.0.0.1");
        mPeer->initialize(PeerBareAddress{"127.0.0.1", 2011});
        {
            RecursiveLockGuard guard(mPeer->mStateMutex);
            mPeer->setState(guard, Peer::CONNECTED);
        }
        mApp->getOverlayManager().maybeAddInboundConnection(mPeer);
        REQUIRE(mApp->getOverlayManager().getPendingPeersCount() == 1);

        // A valid HELLO from a test identity. Derive the sender's MAC key from
        // the receiver's PeerAuth, which is the key recvHello will install.
        auto remote = SecretKey::pseudoRandomForTesting();
        mHello.type(HELLO);
        auto& hello = mHello.hello();
        hello.networkID = mApp->getNetworkID();
        hello.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
        hello.overlayVersion = cfg.OVERLAY_PROTOCOL_VERSION;
        hello.overlayMinVersion = cfg.OVERLAY_PROTOCOL_MIN_VERSION;
        hello.versionStr = "TCPPeer handshake test";
        hello.listeningPort = 2011;
        hello.peerID = remote.getPublicKey();
        hello.nonce = sha256("TCPPeer handshake test nonce");
        hello.cert.pubkey = curve25519DerivePublic(curve25519RandomSecret());
        hello.cert.expiration = mApp->timeNow() + 3600;
        hello.cert.sig = remote.sign(sha256(
            xdr::xdr_to_opaque(hello.networkID, ENVELOPE_TYPE_AUTH,
                               hello.cert.expiration, hello.cert.pubkey)));
        mMacKey = mApp->getOverlayManager().getPeerAuth().getReceivingMacKey(
            hello.cert.pubkey, mPeer->mSendNonce, hello.nonce,
            Peer::REMOTE_CALLED_US);
        REQUIRE(mSenderHmac.setSendMackey(mMacKey));
    }

    template <typename F>
    auto
    onOverlayThread(F&& work)
    {
        auto task = std::make_shared<std::packaged_task<decltype(work())()>>(
            std::forward<F>(work));
        auto result = task->get_future();
        mApp->postOnOverlayThread([task]() { (*task)(); }, "handshake test");
        REQUIRE(result.wait_for(std::chrono::seconds(5)) ==
                std::future_status::ready);
        return result.get();
    }

    AuthenticatedMessage
    authenticate(StellarMessage const& message)
    {
        AuthenticatedMessage result;
        if (message.type() == HELLO && mSentHello)
        {
            // Hmac leaves HELLO unauthenticated. Give a repeated HELLO a valid
            // sequence and MAC so it reaches the ordering check in recvHello
            // instead of failing envelope validation.
            StellarMessage placeholder;
            placeholder.type(AUTH);
            mSenderHmac.setAuthenticatedMessageBody(result, placeholder);
            result.v0().message = message;
            result.v0().mac = hmacSha256(
                mMacKey, xdr::xdr_to_opaque(result.v0().sequence, message));
        }
        else
        {
            mSenderHmac.setAuthenticatedMessageBody(result, message);
        }
        mSentHello = mSentHello || message.type() == HELLO;
        return result;
    }

    void
    append(AuthenticatedMessage const& message)
    {
        auto frame = xdr::xdr_to_msg(message);
        mBytes.insert(mBytes.end(), frame->raw_data(),
                      frame->raw_data() + frame->raw_size());
        mFrameEnds.push_back(mBytes.size());
    }

    // Make `preloaded` bytes available in the peer's buffered stream before
    // its first read, then deliver the rest. Preloading everything drives the
    // synchronous loop in startRead; preloading nothing drives the
    // asynchronous header and body handlers.
    void
    startRead(size_t preloaded)
    {
        REQUIRE(preloaded <= mBytes.size());
        asio::write(mSender, asio::buffer(mBytes.data(), preloaded));
        auto buffered = onOverlayThread([&]() {
            while (mSocket->in_avail() < preloaded)
            {
                mSocket->fill();
            }
            auto result = mSocket->in_avail();
            mPeer->startRead();
            return result;
        });
        REQUIRE(buffered == preloaded);
        if (preloaded < mBytes.size())
        {
            asio::write(mSender, asio::buffer(mBytes.data() + preloaded,
                                              mBytes.size() - preloaded));
        }
    }

    // Wait until the overlay thread has fully handled `count` reads. Observing
    // the counter from the overlay thread itself guarantees the handler that
    // incremented it, including its throttling decision, has completed.
    void
    waitForReads(uint64_t count)
    {
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (onOverlayThread([&]() {
                   return mPeer->getPeerMetrics().mMessageRead.load();
               }) < count)
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    size_t
    unreadBytes()
    {
        return onOverlayThread([&]() {
            return mSocket->in_avail() + mSocket->next_layer().available();
        });
    }
};

TEST_CASE("TCPPeer serializes handshake reads", "[overlay][connections]")
{
    auto buffered = GENERATE(true, false);
    CAPTURE(buffered);
    TCPPeerHandshakeTests test;
    auto& peer = test.mPeer;
    auto& app = test.mApp;
    auto& metrics = app->getOverlayManager().getOverlayMetrics();
    auto flowControl = peer->getFlowControl();

    StellarMessage auth;
    auth.type(AUTH);
    auth.auth().flags = AUTH_MSG_FLAG_FLOW_CONTROL_BYTES_REQUESTED;
    std::vector<StellarMessage> handshake{test.mHello, auth};
    std::string dropReason;
    bool badSequence = false;

    SECTION("HELLO then AUTH")
    {
    }
    SECTION("repeated HELLO")
    {
        handshake = {test.mHello, test.mHello, auth};
        dropReason = "received unexpected HELLO";
    }
    SECTION("repeated AUTH")
    {
        handshake.push_back(auth);
        dropReason = "out-of-order AUTH message";
    }
    SECTION("AUTH without byte flow control")
    {
        handshake.back().auth().flags = 0;
        dropReason = "flow control bytes disabled";
    }
    SECTION("AUTH with invalid sequence")
    {
        badSequence = true;
        dropReason = "unexpected auth sequence";
    }

    for (auto const& message : handshake)
    {
        auto authenticated = test.authenticate(message);
        if (badSequence && message.type() == AUTH)
        {
            ++authenticated.v0().sequence;
        }
        test.append(authenticated);
    }
    // Neither a transaction (which would construct a TransactionFrame on
    // receipt) nor an ordinary request may be read before authentication.
    test.append(test.authenticate(*makeStellarMessage(1)));
    StellarMessage request;
    request.type(GET_SCP_STATE);
    test.append(test.authenticate(request));
    test.startRead(buffered ? test.mBytes.size() : 0);

    size_t processedHello = 0;
    size_t processedAuth = 0;
    for (size_t i = 0; i < handshake.size(); ++i)
    {
        CAPTURE(i, handshake[i].type());
        // The overlay thread reads exactly one frame, then stops until the
        // main thread has processed it.
        test.waitForReads(i + 1);
        REQUIRE(peer->getPeerMetrics().mMessageRead == i + 1);
        REQUIRE(peer->getPeerMetrics().mByteRead == test.mFrameEnds[i]);
        REQUIRE(test.unreadBytes() == test.mBytes.size() - test.mFrameEnds[i]);
        REQUIRE(metrics.mRecvHelloTimer.count() == processedHello);
        REQUIRE(metrics.mRecvAuthTimer.count() == processedAuth);
        REQUIRE(metrics.mRecvTransactionTimer.count() == 0);
        REQUIRE(metrics.mRecvGetSCPStateTimer.count() == 0);

        bool invalidEnvelope = badSequence && handshake[i].type() == AUTH;
        if (!invalidEnvelope)
        {
            REQUIRE(flowControl->isThrottled());
            REQUIRE_FALSE(flowControl->canRead());
            processedHello += handshake[i].type() == HELLO;
            processedAuth += handshake[i].type() == AUTH;
        }

        // Processing the frame on main either drops the peer or releases the
        // reserved capacity, which lets the overlay thread read the next one.
        testutil::crankUntil(
            app,
            [&]() {
                return !peer->isConnectedForTesting() ||
                       peer->getPeerMetrics().mMessageRead > i + 1;
            },
            std::chrono::seconds(5));
        REQUIRE(metrics.mRecvHelloTimer.count() == processedHello);
        REQUIRE(metrics.mRecvAuthTimer.count() == processedAuth);
        if (!peer->isConnectedForTesting())
        {
            REQUIRE(peer->getDropReason() == dropReason);
            REQUIRE(test.unreadBytes() ==
                    test.mBytes.size() - test.mFrameEnds[i]);
            REQUIRE_FALSE(peer->isAuthenticatedForTesting());
            return;
        }
        REQUIRE(peer->isAuthenticatedForTesting() == (processedAuth != 0));
    }

    // Once authenticated, the remaining frames are read and processed.
    REQUIRE(dropReason.empty());
    test.waitForReads(test.mFrameEnds.size());
    REQUIRE(peer->getPeerMetrics().mByteRead == test.mBytes.size());
    testutil::crankUntil(
        app, [&]() { return metrics.mRecvGetSCPStateTimer.count() == 1; },
        std::chrono::seconds(5));
    REQUIRE(peer->isAuthenticatedForTesting());
}

TEST_CASE("TCPPeer rejects ordinary messages between HELLO and AUTH",
          "[overlay][connections]")
{
    TCPPeerHandshakeTests test;
    auto& peer = test.mPeer;
    auto& metrics = test.mApp->getOverlayManager().getOverlayMetrics();

    // A correctly authenticated request in GOT_HELLO passes envelope
    // validation, but must still be rejected on main.
    StellarMessage request;
    request.type(GET_SCP_STATE);
    test.append(test.authenticate(test.mHello));
    test.append(test.authenticate(request));
    test.startRead(test.mBytes.size());

    testutil::crankUntil(
        test.mApp, [&]() { return !peer->isConnectedForTesting(); },
        std::chrono::seconds(5));
    REQUIRE(peer->getDropReason() ==
            "received GET_SCP_STATE before completed handshake");
    REQUIRE(metrics.mRecvHelloTimer.count() == 1);
    REQUIRE(metrics.mRecvGetSCPStateTimer.count() == 0);
    REQUIRE_FALSE(peer->isAuthenticatedForTesting());
}

TEST_CASE("TCPPeer lifetime", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer s = std::make_shared<Simulation>(
        Simulation::OVER_TCP, networkID, [](int i) {
            Config cfg = getTestConfig(i);
            cfg.MAX_INBOUND_PENDING_CONNECTIONS = i % 2;
            cfg.MAX_OUTBOUND_PENDING_CONNECTIONS = i % 2;
            cfg.TARGET_PEER_CONNECTIONS = i % 2;
            cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = i % 2;
            return cfg;
        });

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);

    SCPQuorumSet n1_qset;
    n1_qset.threshold = 1;
    n1_qset.validators.push_back(v11SecretKey.getPublicKey());
    auto n1 = s->addNode(v11SecretKey, n1_qset);

    SECTION("p0 connects to p1, but p1 can't accept, destroy TCPPeer on main")
    {
        s->addPendingConnection(v10SecretKey.getPublicKey(),
                                v11SecretKey.getPublicKey());
        s->startAllNodes();
        s->stopOverlayTick();
        s->crankForAtLeast(std::chrono::seconds(5), false);

        REQUIRE(n0->getMetrics()
                    .NewMeter({"overlay", "outbound", "attempt"}, "connection")
                    .count() == 1);
        REQUIRE(n1->getMetrics()
                    .NewMeter({"overlay", "inbound", "attempt"}, "connection")
                    .count() == 1);
    }
    SECTION("p1 connects to p0, but p1 can't initiate, destroy TCPPeer on main")
    {
        s->addPendingConnection(v11SecretKey.getPublicKey(),
                                v10SecretKey.getPublicKey());
        s->startAllNodes();
        s->stopOverlayTick();
        s->crankForAtLeast(std::chrono::seconds(5), false);
        REQUIRE(n1->getMetrics()
                    .NewMeter({"overlay", "outbound", "attempt"}, "connection")
                    .count() == 0);
        REQUIRE(n0->getMetrics()
                    .NewMeter({"overlay", "inbound", "attempt"}, "connection")
                    .count() == 0);
    }

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(!p0);
    REQUIRE(!p1);
}

TEST_CASE("TCPPeer can communicate", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::ConfigGen cfgGen = [](int i) { return getTestConfig(i); };

    Simulation::pointer s =
        std::make_shared<Simulation>(Simulation::OVER_TCP, networkID, cfgGen);

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);

    SCPQuorumSet n1_qset;
    n1_qset.threshold = 1;
    n1_qset.validators.push_back(v11SecretKey.getPublicKey());
    auto n1 = s->addNode(v11SecretKey, n1_qset);

    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    s->crankForAtLeast(std::chrono::seconds(1), false);

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticatedForTesting());
    REQUIRE(p1->isAuthenticatedForTesting());
    s->stopOverlayTick();

    // Now drop peer, ensure ERROR containing "drop reason" is properly flushed
    auto& msgWrite = n0->getOverlayManager().getOverlayMetrics().mMessageWrite;
    auto prevMsgWrite = msgWrite.count();

    p0->sendGetTxSet(Hash());
    p0->sendErrorAndDrop(ERR_MISC, "test drop");
    s->crankForAtLeast(std::chrono::seconds(1), false);
    REQUIRE(!p0->isConnectedForTesting());
    REQUIRE(!p1->isConnectedForTesting());

    // p0 actually sent GET_TX_SET and ERROR
    REQUIRE(msgWrite.count() == prevMsgWrite + 2);
    s->stopAllNodes();
}

TEST_CASE("TCPPeer read malformed messages", "[overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer s = std::make_shared<Simulation>(
        Simulation::OVER_TCP, networkID, [](int i) {
            Config cfg = getTestConfig(i);
            // Slow down the main thread to delay drops
            cfg.ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING =
                std::chrono::milliseconds(300);
            // Don't run SCP: this test counts messages received by each
            // peer, and SCP traffic between the nodes would pollute the
            // counts nondeterministically
            cfg.FORCE_SCP = false;
            return cfg;
        });

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);
    auto n1 = s->addNode(v11SecretKey, n0_qset);
    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    s->stopOverlayTick();
    // Use generous timeouts: ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING
    // adds 300ms per postOnMainThread callback. With background ledger
    // apply, there are additional callbacks that each incur this sleep.
    s->crankForAtLeast(std::chrono::seconds(15), false);
    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});

    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticatedForTesting());
    REQUIRE(p1->isAuthenticatedForTesting());

    auto& p0recvError =
        n0->getOverlayManager().getOverlayMetrics().mRecvErrorTimer;
    auto p0recvErrorCount = p0recvError.count();

    auto const& recvGetTxSet =
        n1->getOverlayManager().getOverlayMetrics().mRecvGetTxSetTimer;
    auto recvGetTxSetPrev = recvGetTxSet.count();

    // Send non-flood messages: flood messages must go through FlowControl's
    // outbound queue, while the *ForTesting helpers write directly to the
    // socket
    auto msg = std::make_shared<StellarMessage>();
    msg->type(GET_TX_SET);
    msg->txSetHash() = Hash();

    // Wrap a transaction of the given size in a TX_SET (not flow-controlled)
    auto makeTxSetMessage = [](uint32_t wasmSize) {
        auto txSet = std::make_shared<StellarMessage>();
        txSet->type(TX_SET);
        txSet->txSet().previousLedgerHash = sha256("not a tx batch");
        txSet->txSet().txs.push_back(
            makeStellarMessage(wasmSize)->transaction());
        REQUIRE(!OverlayManager::isFloodMessage(*txSet));
        return txSet;
    };

    auto crankAndValidateDrop = [&](std::string const& dropReason,
                                    bool shouldSendError) {
        s->crankUntil(
            [&]() {
                // p0 should drop p1
                return !p0->isConnectedForTesting() &&
                       !p1->isConnectedForTesting();
            },
            std::chrono::seconds(30), false);
        REQUIRE(!p0->isConnectedForTesting());
        REQUIRE(!p1->isConnectedForTesting());
        REQUIRE(p1->getDropReason() == dropReason);

        if (shouldSendError)
        {
            // p0 received ERROR from p1
            REQUIRE(p0recvErrorCount + 1 == p0recvError.count());
            REQUIRE(recvGetTxSet.count() == recvGetTxSetPrev);
        }
    };

    SECTION("message size is over limit")
    {
        auto bigMessage = makeTxSetMessage(MAX_MESSAGE_SIZE * 2);
        REQUIRE(xdr::xdr_size(*bigMessage) > MAX_MESSAGE_SIZE);

        p0->sendAuthenticatedMessageForTesting(bigMessage);
        p0->sendAuthenticatedMessageForTesting(makeTxSetMessage(1000));
        crankAndValidateDrop("error during read", false);
    }
    SECTION("bad auth sequence")
    {
        n0->postOnOverlayThread(
            [p0, msg]() {
                // Send message without auth sequence
                AuthenticatedMessage amsg;
                amsg.v0().message = *msg;
                p0->sendXdrMessageForTesting(xdr::xdr_to_msg(amsg), msg);
                // Follow by a regular message so there's something in the
                // socket
                p0->sendAuthenticatedMessageForTesting(msg);
            },
            "send");

        crankAndValidateDrop("unexpected auth sequence", true);
    }
    SECTION("corrupt xdr")
    {
        n0->postOnOverlayThread(
            [p0, msg]() {
                xdr::msg_ptr corruptMsg = xdr::message_t::alloc(0xff);
                p0->sendXdrMessageForTesting(std::move(corruptMsg), msg);
                // Send a normal message to make sure there's something to read
                // in the socket
                p0->sendAuthenticatedMessageForTesting(msg);
            },
            "send");
        crankAndValidateDrop("received corrupt XDR", true);
    }
}

TEST_CASE("TCPPeer drop at capacity", "[overlay][flowcontrol]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto txMsgPtr = makeStellarMessage(1);
    uint32 txSize = static_cast<uint32>(xdr::xdr_argpack_size(*txMsgPtr));

    Simulation::pointer s = std::make_shared<Simulation>(
        Simulation::OVER_TCP, networkID, [txSize](int i) {
            Config cfg = getTestConfig(i);
            cfg.ARTIFICIALLY_SLEEP_MAIN_THREAD_FOR_TESTING =
                std::chrono::milliseconds(300);
            if (i == 2)
            {
                cfg.PEER_FLOOD_READING_CAPACITY = 1;
                cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE = 1;
            }
            return cfg;
        });

    auto v10SecretKey = SecretKey::fromSeed(sha256("v10"));
    auto v11SecretKey = SecretKey::fromSeed(sha256("v11"));

    SCPQuorumSet n0_qset;
    n0_qset.threshold = 1;
    n0_qset.validators.push_back(v10SecretKey.getPublicKey());
    auto n0 = s->addNode(v10SecretKey, n0_qset);
    auto n1 = s->addNode(v11SecretKey, n0_qset);
    s->addPendingConnection(v10SecretKey.getPublicKey(),
                            v11SecretKey.getPublicKey());
    s->startAllNodes();
    n0->getHerder().setMaxClassicTxSize(txSize);
    n1->getHerder().setMaxClassicTxSize(txSize);
    s->stopOverlayTick();
    s->crankForAtLeast(std::chrono::seconds(5), false);

    auto p0 = n0->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n1->getConfig().PEER_PORT});
    auto p1 = n1->getOverlayManager().getConnectedPeer(
        PeerBareAddress{"127.0.0.1", n0->getConfig().PEER_PORT});

    REQUIRE(p0);
    REQUIRE(p1);
    REQUIRE(p0->isAuthenticatedForTesting());
    REQUIRE(p1->isAuthenticatedForTesting());

    p0->sendAuthenticatedMessageForTesting(txMsgPtr);
    p0->sendAuthenticatedMessageForTesting(txMsgPtr);

    s->crankUntil(
        [&]() {
            return !p0->isConnectedForTesting() && !p1->isConnectedForTesting();
        },
        std::chrono::seconds(10), false);

    REQUIRE(!p0->isConnectedForTesting());
    REQUIRE(!p1->isConnectedForTesting());
    REQUIRE(p1->getDropReason() ==
            "unexpected flood message, peer at capacity");

    s->stopAllNodes();
}
}
