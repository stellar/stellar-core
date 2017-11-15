// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "Application.h"
#include "ApplicationImpl.h"
#include "util/format.h"

namespace stellar
{
using namespace std;

void
validateNetworkPassphrase(Application::pointer app)
{
    std::string networkPassphrase = app->getConfig().NETWORK_PASSPHRASE;
    if (networkPassphrase.empty())
    {
        throw std::invalid_argument("NETWORK_PASSPHRASE not configured");
    }

    auto& persistentState = app->getPersistentState();
    std::string prevNetworkPassphrase =
        persistentState.getState(PersistentState::kNetworkPassphrase);
    if (prevNetworkPassphrase.empty())
    {
        persistentState.setState(PersistentState::kNetworkPassphrase,
                                 networkPassphrase);
    }
    else if (networkPassphrase != prevNetworkPassphrase)
    {
        throw std::invalid_argument(
            fmt::format("NETWORK_PASSPHRASE \"{}\" does not match"
                        " previous NETWORK_PASSPHRASE \"{}\"",
                        networkPassphrase, prevNetworkPassphrase));
    }
}

Application::pointer
Application::create(VirtualClock& clock, Config const& cfg, bool newDB)
{
    return create<ApplicationImpl>(clock, cfg, newDB);
}
}
