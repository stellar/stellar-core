// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCPDriver.h"

#include <algorithm>

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "xdrpp/marshal.h"

namespace stellar
{

bool
WrappedValuePtrComparator::operator()(ValueWrapperPtr const& l,
                                      ValueWrapperPtr const& r) const
{
    assert(l && r);
    return l->getValue() < r->getValue();
}

SCPEnvelopeWrapper::SCPEnvelopeWrapper(SCPEnvelope const& e) : mEnvelope(e)
{
}

SCPEnvelopeWrapper::~SCPEnvelopeWrapper()
{
}

ValueWrapper::ValueWrapper(Value const& value) : mValue(value)
{
}

ValueWrapper::~ValueWrapper()
{
}

SCPEnvelopeWrapperPtr
SCPDriver::wrapEnvelope(SCPEnvelope const& envelope)
{
    auto res = std::make_shared<SCPEnvelopeWrapper>(envelope);
    return res;
}

ValueWrapperPtr
SCPDriver::wrapValue(Value const& value)
{
    auto res = std::make_shared<ValueWrapper>(value);
    return res;
}

std::string
SCPDriver::getValueString(Value const& v) const
{
    Hash valueHash = getHashOf({xdr::xdr_to_opaque(v)});

    return hexAbbrev(valueHash);
}

std::string
SCPDriver::toStrKey(PublicKey const& pk, bool fullKey) const
{
    return fullKey ? KeyUtils::toStrKey(pk) : toShortString(pk);
}

std::string
SCPDriver::toShortString(PublicKey const& pk) const
{
    return KeyUtils::toShortString(pk);
}

// values used to switch hash function between priority and neighborhood checks
static const uint32 hash_N = 1;
static const uint32 hash_P = 2;
static const uint32 hash_K = 3;

uint64
SCPDriver::hashHelper(
    uint64 slotIndex, Value const& prev,
    std::function<void(std::vector<xdr::opaque_vec<>>&)> extra)
{
    std::vector<xdr::opaque_vec<>> vals;
    vals.emplace_back(xdr::xdr_to_opaque(slotIndex));
    vals.emplace_back(xdr::xdr_to_opaque(prev));
    extra(vals);
    Hash t = getHashOf(vals);
    uint64 res = 0;
    for (size_t i = 0; i < sizeof(res); i++)
    {
        res = (res << 8) | t[i];
    }
    return res;
}

uint64
SCPDriver::computeHashNode(uint64 slotIndex, Value const& prev, bool isPriority,
                           int32_t roundNumber, NodeID const& nodeID)
{
    return hashHelper(
        slotIndex, prev, [&](std::vector<xdr::opaque_vec<>>& vals) {
            vals.emplace_back(xdr::xdr_to_opaque(isPriority ? hash_P : hash_N));
            vals.emplace_back(xdr::xdr_to_opaque(roundNumber));
            vals.emplace_back(xdr::xdr_to_opaque(nodeID));
        });
}

uint64
SCPDriver::computeValueHash(uint64 slotIndex, Value const& prev,
                            int32_t roundNumber, Value const& value)
{
    return hashHelper(slotIndex, prev,
                      [&](std::vector<xdr::opaque_vec<>>& vals) {
                          vals.emplace_back(xdr::xdr_to_opaque(hash_K));
                          vals.emplace_back(xdr::xdr_to_opaque(roundNumber));
                          vals.emplace_back(xdr::xdr_to_opaque(value));
                      });
}

static const int MAX_TIMEOUT_SECONDS = (30 * 60);

std::chrono::milliseconds
SCPDriver::computeTimeout(uint32 roundNumber)
{
    // straight linear timeout
    // starting at 1 second and capping at MAX_TIMEOUT_SECONDS

    int timeoutInSeconds;
    if (roundNumber > MAX_TIMEOUT_SECONDS)
    {
        timeoutInSeconds = MAX_TIMEOUT_SECONDS;
    }
    else
    {
        timeoutInSeconds = (int)roundNumber;
    }
    return std::chrono::seconds(timeoutInSeconds);
}
}
