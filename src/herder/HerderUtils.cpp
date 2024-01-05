// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderUtils.h"
#include "scp/Slot.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger.h"
#include <algorithm>
#include <xdrpp/marshal.h>

namespace stellar
{

std::vector<Hash>
getTxSetHashes(SCPEnvelope const& envelope)
{
    auto values = getStellarValues(envelope.statement);
    auto result = std::vector<Hash>{};
    result.resize(values.size());

    std::transform(std::begin(values), std::end(values), std::begin(result),
                   [](StellarValue const& sv) { return sv.txSetHash; });

    return result;
}

std::vector<StellarValue>
getStellarValues(SCPStatement const& statement)
{
    auto values = Slot::getStatementValues(statement);
    auto result = std::vector<StellarValue>{};
    result.resize(values.size());

    std::transform(std::begin(values), std::end(values), std::begin(result),
                   [](Value const& v) {
                       auto wb = StellarValue{};
                       xdr::xdr_from_opaque(v, wb);
                       return wb;
                   });

    return result;
}

bool
shouldDropPeerPredicate(Peer::pointer peer, uint32_t protocolVersion)
{
    bool upgraded =
        protocolVersionStartsFrom(protocolVersion, SOROBAN_PROTOCOL_VERSION);
    auto ovVersion = peer->getRemoteOverlayVersion();
    if (upgraded && ovVersion &&
        ovVersion.value() < Peer::FIRST_VERSION_REQUIRED_FOR_PROTOCOL_20)
    {
        return true;
    }
    return false;
}
}
