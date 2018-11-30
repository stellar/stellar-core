// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/QuorumSetUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-types.h"

#include <algorithm>
#include <set>

namespace stellar
{

void
checkQuorumIntersection(Config cfg)
{
    VirtualClock clock;
    cfg.setNoListen();
    Application::pointer app = Application::create(clock, cfg, false);
    InferredQuorum iq = app->getHistoryManager().inferQuorum();
    iq.checkQuorumIntersection(cfg);
}

void
inferQuorumAndWrite(Config cfg)
{
    InferredQuorum iq;
    {
        VirtualClock clock;
        cfg.setNoListen();
        Application::pointer app = Application::create(clock, cfg, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    LOG(INFO) << "Inferred quorum";
    std::cout << iq.toString(cfg) << std::endl;
}

void
writeQuorumGraph(Config cfg, std::string const& outputFile)
{
    InferredQuorum iq;
    {
        VirtualClock clock;
        cfg.setNoListen();
        Application::pointer app = Application::create(clock, cfg, false);
        iq = app->getHistoryManager().inferQuorum();
    }
    std::string filename = outputFile.empty() ? "-" : outputFile;
    if (filename == "-")
    {
        std::stringstream out;
        iq.writeQuorumGraph(cfg, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Quorum graph: " << out.str();
        LOG(INFO) << "*";
    }
    else
    {
        std::ofstream out(filename);
        iq.writeQuorumGraph(cfg, out);
        LOG(INFO) << "*";
        LOG(INFO) << "* Wrote quorum graph to " << filename;
        LOG(INFO) << "*";
    }
}
}
