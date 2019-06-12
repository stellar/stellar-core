// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/QuorumIntersectionChecker.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

#include <algorithm>
#include <set>

namespace stellar
{

void
checkQuorumIntersection(Config const& cfg)
{
    VirtualClock clock;
    Config cfg2(cfg);
    cfg2.setNoListen();
    Application::pointer app = Application::create(clock, cfg2, false);
    LOG(INFO) << "Checking last-heard quorum from herder";
    app->start();
    auto qic = QuorumIntersectionChecker::create(
        app->getHerder().getCurrentlyTrackedQuorum(), cfg);
    qic->networkEnjoysQuorumIntersection();
}

void
inferQuorumAndWrite(Config const& cfg)
{
    Config cfg2(cfg);
    InferredQuorum iq;
    {
        VirtualClock clock;
        cfg2.setNoListen();
        Application::pointer app = Application::create(clock, cfg2, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    LOG(INFO) << "Inferred quorum";
    std::cout << iq.toString(cfg2) << std::endl;
}

void
writeQuorumGraph(Config const& cfg, std::string const& outputFile)
{
    Config cfg2(cfg);
    InferredQuorum iq;
    {
        VirtualClock clock;
        cfg2.setNoListen();
        Application::pointer app = Application::create(clock, cfg2, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    std::string filename = outputFile.empty() ? "-" : outputFile;
    if (filename == "-")
    {
        std::stringstream out;
        iq.writeQuorumGraph(cfg2, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Quorum graph: " << out.str();
        LOG(INFO) << "*";
    }
    else
    {
        std::ofstream out(filename);
        iq.writeQuorumGraph(cfg2, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Wrote quorum graph to " << filename;
        LOG(INFO) << "*";
    }
}
}
