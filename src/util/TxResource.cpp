// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/TxResource.h"
#include "util/GlobalChecks.h"
#include "util/numeric.h"
#include <algorithm>
#include <stdexcept>

namespace stellar
{

std::string
Resource::getStringFromType(Type type)
{
    switch (type)
    {
    case Type::OPERATIONS:
        return "Operations";
    case Type::INSTRUCTIONS:
        return "Instructions";
    case Type::TX_BYTE_SIZE:
        return "TxByteSize";
    case Type::DISK_READ_BYTES:
        return "DiskReadBytes";
    case Type::WRITE_BYTES:
        return "WriteBytes";
    case Type::READ_LEDGER_ENTRIES:
        return "ReadLedgerEntries";
    case Type::WRITE_LEDGER_ENTRIES:
        return "WriteLedgerEntries";
    }
    return "Unknown";
}

Resource::Resource(std::vector<int64_t> args)
{
    if (args.size() != NUM_CLASSIC_TX_RESOURCES &&
        args.size() != NUM_SOROBAN_TX_RESOURCES &&
        args.size() != NUM_CLASSIC_TX_BYTES_RESOURCES)
    {
        throw std::runtime_error("Invalid number of resources");
    }
    mResources = args;
}

Resource::Resource(int64_t arg)
{
    mResources = std::vector<int64_t>(1, arg);
}

bool
Resource::isZero() const
{
    return std::all_of(mResources.begin(), mResources.end(),
                       [](int64_t x) { return x == 0; });
}

bool
Resource::anyPositive() const
{
    return std::any_of(mResources.begin(), mResources.end(),
                       [](int64_t x) { return x > 0; });
}

size_t
Resource::size() const
{
    return mResources.size();
}

std::string
Resource::toString() const
{
    std::string res = "";
    for (auto const& r : mResources)
    {
        res += std::to_string(r) + ", ";
    }
    return res;
}

Resource
Resource::makeEmptySoroban()
{
    return makeEmpty(NUM_SOROBAN_TX_RESOURCES);
}

Resource
Resource::makeEmpty(size_t numRes)
{
    std::vector<int64_t> res(numRes, 0);
    return Resource(res);
}

int64_t
Resource::getVal(Resource::Type valType) const
{
    return mResources.at(static_cast<size_t>(valType));
}

void
Resource::setVal(Resource::Type valType, int64_t val)
{
    mResources.at(static_cast<size_t>(valType)) = val;
}

Resource
subtractNonNegative(Resource const& lhs, Resource const& rhs)
{
    releaseAssert(lhs.size() == rhs.size());
    auto newRes = lhs;

    for (size_t i = 0; i < lhs.size(); i++)
    {
        if (lhs.mResources[i] > rhs.mResources[i])
        {
            newRes.mResources[i] -= rhs.mResources[i];
        }
        else
        {
            newRes.mResources[i] = 0;
        }
    }

    return newRes;
}

bool
anyLessThan(Resource const& lhs, Resource const& rhs)
{
    releaseAssert(lhs.size() == rhs.size());
    for (size_t i = 0; i < lhs.size(); i++)
    {
        if (lhs.mResources[i] < rhs.mResources[i])
        {
            return true;
        }
    }
    return false;
}

// Throws if multiplication result overflows int64_t
Resource
multiplyByDouble(Resource const& res, double m)
{
    auto newRes = res;
    for (auto& resource : newRes.mResources)
    {
        auto tempResultDbl = resource * m;
        // Multiple each resource dimension by the rate
        releaseAssertOrThrow(tempResultDbl >= 0.0);
        releaseAssertOrThrow(isRepresentableAsInt64(tempResultDbl));
        resource = static_cast<int64_t>(tempResultDbl);
    }

    return newRes;
}

// Saturating multiply: cap at int64_t::max if result would overflow
Resource
saturatedMultiplyByDouble(Resource const& res, double m)
{
    auto newRes = res;
    for (auto& resource : newRes.mResources)
    {
        auto tempResultDbl = resource * m;
        releaseAssertOrThrow(tempResultDbl >= 0.0);

        if (isRepresentableAsInt64(tempResultDbl))
        {
            resource = static_cast<int64_t>(tempResultDbl);
        }
        else
        {
            resource = std::numeric_limits<int64_t>::max();
        }
    }

    return newRes;
}

Resource
bigDivideOrThrow(Resource const& res, int64_t B, int64_t C, Rounding rounding)
{
    auto newRes = res;
    for (auto& resource : newRes.mResources)
    {
        resource = bigDivideOrThrow(resource, B, C, rounding);
    }

    return newRes;
}

bool
operator==(Resource const& lhs, Resource const& rhs)
{
    return lhs.mResources == rhs.mResources;
}

Resource
operator+(Resource const& lhs, Resource const& rhs)
{
    Resource result(lhs);
    return result += rhs;
}

Resource
operator-(Resource const& lhs, Resource const& rhs)
{
    Resource result(lhs);
    return result -= rhs;
}

bool
operator<=(Resource const& lhs, Resource const& rhs)
{
    releaseAssert(lhs.size() == rhs.size());
    for (size_t i = 0; i < lhs.size(); i++)
    {
        if (lhs.mResources[i] > rhs.mResources[i])
        {
            return false;
        }
    }
    return true;
}

bool
anyGreater(Resource const& lhs, Resource const& rhs)
{
    releaseAssert(lhs.size() == rhs.size());
    for (size_t i = 0; i < lhs.size(); i++)
    {
        if (lhs.mResources[i] > rhs.mResources[i])
        {
            return true;
        }
    }
    return false;
}

// All greater
bool
operator>(Resource const& lhs, Resource const& rhs)
{
    return !(lhs <= rhs);
}

Resource&
Resource::operator+=(Resource const& other)
{
    releaseAssert(canAdd(other));
    for (size_t i = 0; i < mResources.size(); i++)
    {
        mResources[i] += other.mResources[i];
    }
    return *this;
}

Resource&
Resource::operator-=(Resource const& other)
{
    releaseAssert(mResources.size() == other.mResources.size());
    for (size_t i = 0; i < mResources.size(); i++)
    {
        releaseAssert(mResources[i] >= other.mResources[i]);
        mResources[i] -= other.mResources[i];
    }
    return *this;
}

Resource
limitTo(Resource const& curr, Resource const& limit)
{
    releaseAssert(curr.size() == limit.size());
    Resource limited(std::vector<int64_t>(curr.size(), 0));
    for (int i = 0; i < limited.size(); i++)
    {
        limited.mResources[i] =
            std::min<int64_t>(curr.mResources[i], limit.mResources[i]);
    }
    return limited;
}

bool
Resource::canAdd(Resource const& other) const
{
    releaseAssert(size() == other.size());
    for (size_t i = 0; i < size(); i++)
    {
        if (std::numeric_limits<int64_t>::max() - mResources[i] <
            other.mResources[i])
        {
            return false;
        }
    }
    return true;
}

} // namespace stellar
