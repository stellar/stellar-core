// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/numeric.h"
#include <cstdint>
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
        DISK_READ_BYTES = 3,
        WRITE_BYTES = 4,
        READ_LEDGER_ENTRIES = 5,
        WRITE_LEDGER_ENTRIES = 6
    };

    static std::string getStringFromType(Type type);

    Resource(std::vector<int64_t> args);
    Resource(int64_t arg);

    bool isZero() const;
    bool anyPositive() const;
    size_t size() const;
    std::string toString() const;

    Resource& operator+=(Resource const& other);
    Resource& operator-=(Resource const& other);

    static Resource makeEmptySoroban();
    static Resource makeEmpty(size_t numRes);

    int64_t getVal(Resource::Type valType) const;
    void setVal(Resource::Type valType, int64_t val);

    bool canAdd(Resource const& other) const;

    friend Resource multiplyByDouble(Resource const& res, double m);
    friend Resource saturatedMultiplyByDouble(Resource const& res, double m);
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
