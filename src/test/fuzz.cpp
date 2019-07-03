// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/StellarCoreVersion.h"
#include "overlay/OverlayManager.h"
#include "overlay/TCPPeer.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Timer.h"
#include "util/XDRStream.h"

#include "test/fuzz.h"

#include <signal.h>
#include <xdrpp/autocheck.h>
#include <xdrpp/printer.h>

/**
 * This is a very simple fuzzer _stub_. It's intended to be run under an
 * external fuzzer with some fuzzing brains, AFL or fuzzgrind or whatever.
 *
 * It has two modes:
 *
 *   - In --genfuzz mode it spits out a small file containing a handful of
 *     random StellarMessages. This is the mode you use to generate seed data
 *     for the external fuzzer's corpus.
 *
 *   - In --fuzz mode it reads back a file and appplies it to a pair of of
 *     stellar-cores in loopback mode, cranking the I/O loop to simulate
 *     receiving the messages one by one. It exits when it's read the input.
 *     This is the mode the external fuzzer will run its mutant inputs through.
 *
 */

namespace stellar
{

std::string
msgSummary(StellarMessage const& m)
{
    xdr::detail::Printer p(0);
    xdr::archive(p, m.type(), nullptr);
    return p.buf_.str() + ":" + hexAbbrev(sha256(xdr::xdr_to_msg(m)));
}

bool
tryRead(XDRInputFileStream& in, StellarMessage& m)
{
    try
    {
        return in.readOne(m);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        LOG(INFO) << "Caught XDR error '" << e.what()
                  << "' on input substituting HELLO";
        m.type(HELLO);
        return true;
    }
}

#define PERSIST_MAX 10000000
#define INITIATOR 1
#define ACCEPTOR 0
void
fuzz(std::string const& filename, el::Level logLevel,
     std::vector<std::string> const& metrics)
{
    Logging::setFmt("<fuzz>", false);
    Logging::setLogLevel(logLevel, nullptr);
    LOG(INFO) << "Fuzzing stellar-core " << STELLAR_CORE_VERSION;
    LOG(INFO) << "Fuzz input is in " << filename;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto confGen = [](int instanceNumber) {
        Config cfg = getTestConfig(instanceNumber);
        cfg.MANUAL_CLOSE = true;
        cfg.CATCHUP_COMPLETE = false;
        cfg.CATCHUP_RECENT = 0;
        cfg.ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING = false;
        cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = UINT32_MAX;
        cfg.PUBLIC_HTTP_PORT = false;
        cfg.WORKER_THREADS = 1;
        cfg.QUORUM_INTERSECTION_CHECKER = false;
        cfg.PREFERRED_PEERS_ONLY = false;
        cfg.RUN_STANDALONE = true;

        return cfg;
    };
    auto simulation = std::make_shared<Simulation>(Simulation::OVER_LOOPBACK,
                                                   networkID, confGen);

    SIMULATION_CREATE_NODE(10);
    SIMULATION_CREATE_NODE(11);

    SCPQuorumSet qSet0;
    qSet0.threshold = 2;
    qSet0.validators.push_back(v10NodeID);
    qSet0.validators.push_back(v11NodeID);

    simulation->addNode(v10SecretKey, qSet0);
    simulation->addNode(v11SecretKey, qSet0);

    simulation->addPendingConnection(v10SecretKey.getPublicKey(),
                                     v11SecretKey.getPublicKey());

    simulation->startAllNodes();

    // crank until nodes are connected
    simulation->crankUntil(
        [&]() {
            auto nodes = simulation->getNodes();
            auto numberOfSimulationConnections =
                nodes[ACCEPTOR]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount() +
                nodes[INITIATOR]
                    ->getOverlayManager()
                    .getAuthenticatedPeersCount();
            return numberOfSimulationConnections == 2;
        },
        std::chrono::milliseconds{500}, false);

    while (__AFL_LOOP(PERSIST_MAX))
    {
        XDRInputFileStream in(MAX_MESSAGE_SIZE);
        in.open(filename);
        StellarMessage msg;
        while (tryRead(in, msg))
        {
            LOG(INFO) << "Fuzzer injecting message." << msgSummary(msg);

            auto nodeids = simulation->getNodeIDs();

            auto loopbackPeerConnection = simulation->getLoopbackConnection(
                nodeids[INITIATOR], nodeids[ACCEPTOR]);

            // ensure connection exists
            assert(loopbackPeerConnection);

            auto initiator = loopbackPeerConnection->getInitiator();
            auto acceptor = loopbackPeerConnection->getAcceptor();

            initiator->getApp().getClock().postToCurrentCrank(
                [initiator, msg]() { initiator->Peer::sendMessage(msg); });

            simulation->crankForAtMost(std::chrono::milliseconds{500}, false);

            // clear all queues and cancel all events
            initiator->clearInAndOutQueues();
            acceptor->clearInAndOutQueues();

            while (initiator->getApp().getClock().cancelAllEvents())
                ;
            while (acceptor->getApp().getClock().cancelAllEvents())
                ;
        }
    }
}

void
genfuzz(std::string const& filename)
{
    Logging::setFmt("<fuzz>");
    size_t n = 3;
    LOG(INFO) << "Writing " << n << "-message random fuzz file " << filename;
    XDROutputFileStream out;
    out.open(filename);
    autocheck::generator<StellarMessage> gen;
    for (size_t i = 0; i < n; ++i)
    {
        try
        {
            StellarMessage m(gen(10));
            out.writeOne(m);
            LOG(INFO) << "Message " << i << ": " << msgSummary(m);
        }
        catch (xdr::xdr_bad_discriminant const&)
        {
            LOG(INFO) << "Message " << i << ": malformed, omitted";
        }
    }
}
}
