#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "scp/SCP.h"
#include "xdr/Stellar-types.h"

#include <lib/json/json.h>
#include <vector>

namespace stellar
{

class Application;
struct SCPEnvelope;
struct SCPStatement;
struct StellarValue;

Hash getQuorumSetHash(SCPEnvelope const& envelope);
std::vector<Hash> getTxSetHashes(SCPEnvelope const& envelope);
std::vector<StellarValue> getStellarValues(SCPStatement const& envelope);

template <typename T>
void
dumpEnvelopes(Application& app, Json::Value& ret, T const& container)
{
    for (auto const& e : container)
    {
        ret.append(app.getHerder().getSCP().envToStr(e));
    }
}

template <typename T>
void
dumpEnvelopes(Application& app, Json::Value& ret, T const& container,
              std::string const& name)
{
    if (container.empty())
    {
        return;
    }

    auto& i = ret[name];
    for (auto const& e : container)
    {
        i.append(app.getHerder().getSCP().envToStr(e));
    }
}

void traceEnvelope(Application& app, std::string const& message,
                   SCPEnvelope const& envelope);
}
