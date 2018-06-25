// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderUtils.h"
#include "herder/Herder.h"
#include "main/Application.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger.h"

#include <algorithm>
#include <xdrpp/marshal.h>

namespace stellar
{

Hash
getQuorumSetHash(SCPEnvelope const& envelope)
{
    return Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
}

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

void
traceEnvelope(Application& app, std::string const& message,
              SCPEnvelope const& envelope)
{
    if (Logging::logTrace("Herder"))
        CLOG(TRACE, "Herder")
            << message << ": " << app.getHerder().envToStr(envelope);
}
}
