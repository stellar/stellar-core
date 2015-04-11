// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "generated/StellarCoreVersion.h"
#include "overlay/OverlayManager.h"
#include "overlay/LoopbackPeer.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/XDRStream.h"
#include "main/Config.h"

#include "main/fuzz.h"

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

void
fuzz(std::string const& filename, el::Level logLevel,
     std::vector<std::string> const& metrics)
{
    Logging::setFmt("<fuzz>");
    Logging::setLogLevel(logLevel, nullptr);
    LOG(INFO) << "Fuzzing stellar-core " << STELLAR_CORE_VERSION;
    LOG(INFO) << "Fuzz input is in " << filename;

    Config cfg1, cfg2;

    cfg1.RUN_STANDALONE = true;
    cfg1.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg1.LOG_FILE_PATH = "fuzz-app-1.log";
    cfg1.TMP_DIR_PATH = "fuzz-tmp-1";
    cfg1.BUCKET_DIR_PATH = "fuzz-buckets-1";

    cfg2.RUN_STANDALONE = true;
    cfg2.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    cfg1.LOG_FILE_PATH = "fuzz-app-2.log";
    cfg2.TMP_DIR_PATH = "fuzz-tmp-2";
    cfg2.BUCKET_DIR_PATH = "fuzz-buckets-2";

    VirtualClock clock;
    Application::pointer app1 = Application::create(clock, cfg1);
    Application::pointer app2 = Application::create(clock, cfg2);
    LoopbackPeerConnection loop(*app1, *app2);
    while (clock.crank(false) > 0)
        ;

    XDRInputFileStream in;
    in.open(filename);
    StellarMessage msg;
    size_t i = 0;
    while (in.readOne(msg))
    {
        ++i;
        if (msg.type() != HELLO)
        {
            LOG(INFO) << "Fuzzer injecting message " << i << ": "
                      << msgSummary(msg);
            loop.getAcceptor()->Peer::sendMessage(msg);
        }
        while (clock.crank(false) > 0)
            ;
    }
}

void
genfuzz(std::string const& filename)
{
    Logging::setFmt("<fuzz>");
    size_t n = 8;
    LOG(INFO) << "Writing " << n << "-message random fuzz file " << filename;
    XDROutputFileStream out;
    out.open(filename);
    autocheck::generator<StellarMessage> gen;
    for (size_t i = 0; i < n; ++i)
    {
        try
        {
            StellarMessage m(gen(20));
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
