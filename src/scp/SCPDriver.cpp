// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "SCPDriver.h"

#include <algorithm>

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SecretKey.h"
#include "util/GlobalChecks.h"
#include "xdrpp/marshal.h"

namespace stellar
{

bool
WrappedValuePtrComparator::operator()(ValueWrapperPtr const& l,
                                      ValueWrapperPtr const& r) const
{
    releaseAssert(l && r);
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
SCPDriver::toStrKey(NodeID const& pk, bool fullKey) const
{
    return fullKey ? KeyUtils::toStrKey(pk) : toShortString(pk);
}

std::string
SCPDriver::toShortString(NodeID const& pk) const
{
    return KeyUtils::toShortString(pk);
}

}
