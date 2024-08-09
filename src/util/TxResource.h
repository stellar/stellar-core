#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/numeric.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace stellar
{
constexpr size_t NUM_CLASSIC_TX_RESOURCES(1);
constexpr size_t NUM_CLASSIC_TX_BYTES_RESOURCES(2);
constexpr size_t NUM_SOROBAN_TX_RESOURCES(7);

// Small helper class to allow arithmetic operations on tuples
class Resource
{
    std::vector<int64_t> mResources;

  public:
    enum class Type
    {
        OPERATIONS = 0,
        INSTRUCTIONS = 1,
        TX_BYTE_SIZE = 2,
        READ_BYTES = 3,
        WRITE_BYTES = 4,
        READ_LEDGER_ENTRIES = 5,
        WRITE_LEDGER_ENTRIES = 6
    };

    static std::string
    getStringFromType(Type type)
    {
        switch (type)
        {
        case Type::OPERATIONS:
            return "Operations";
        case Type::INSTRUCTIONS:
            return "Instructions";
        case Type::TX_BYTE_SIZE:
            return "TxByteSize";
        case Type::READ_BYTES:
            return "ReadBytes";
        case Type::WRITE_BYTES:
            return "WriteBytes";
        case Type::READ_LEDGER_ENTRIES:
            return "ReadLedgerEntries";
        case Type::WRITE_LEDGER_ENTRIES:
            return "WriteLedgerEntries";
        }
        return "Unknown";
    }

    Resource(std::vector<int64_t> args)
    {
        if (args.size() != NUM_CLASSIC_TX_RESOURCES &&
            args.size() != NUM_SOROBAN_TX_RESOURCES &&
            args.size() != NUM_CLASSIC_TX_BYTES_RESOURCES)
        {
            throw std::runtime_error("Invalid number of resources");
        }
        mResources = args;
    }

    Resource(int64_t arg)
    {
        mResources = std::vector<int64_t>(1, arg);
    }

    bool
    isZero() const
    {
        return std::all_of(mResources.begin(), mResources.end(),
                           [](int64_t x) { return x == 0; });
    }

    bool
    anyPositive() const
    {
        return std::any_of(mResources.begin(), mResources.end(),
                           [](int64_t x) { return x > 0; });
    }

    size_t
    size() const
    {
        return mResources.size();
    }

    std::string
    toString() const
    {
        std::string res = "";
        for (auto const& r : mResources)
        {
            res += std::to_string(r) + ", ";
        }
        return res;
    }

    Resource& operator+=(Resource const& other);
    Resource& operator-=(Resource const& other);

    static Resource
    makeEmptySoroban()
    {
        return makeEmpty(NUM_SOROBAN_TX_RESOURCES);
    }

    static Resource
    makeEmpty(size_t numRes)
    {
        std::vector<int64_t> res(numRes, 0);
        return Resource(res);
    }

    int64_t
    getVal(Resource::Type valType) const
    {
        return mResources.at(static_cast<size_t>(valType));
    }

    void
    setVal(Resource::Type valType, int64_t val)
    {
        mResources.at(static_cast<size_t>(valType)) = val;
    }

    bool canAdd(Resource const& other) const;

    friend Resource multiplyByDouble(Resource const& res, double m);
    friend Resource bigDivideOrThrow(Resource const& res, int64_t B, int64_t C,
                                     Rounding rounding);
    friend Resource operator+(Resource const& lhs, Resource const& rhs);
    friend Resource operator-(Resource const& lhs, Resource const& rhs);
    friend bool anyLessThan(Resource const& lhs, Resource const& rhs);
    friend bool anyGreater(Resource const& lhs, Resource const& rhs);
    friend Resource subtractNonNegative(Resource const& lhs,
                                        Resource const& rhs);
    friend Resource limitTo(Resource const& curr, Resource const& limit);
    friend bool operator<=(Resource const& lhs, Resource const& rhs);
    friend bool operator==(Resource const& lhs, Resource const& rhs);
    friend bool operator>(Resource const& lhs, Resource const& rhs);
};
} // namespace stellar
