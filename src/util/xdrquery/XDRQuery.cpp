// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/xdrquery/XDRQuery.h"

namespace xdrquery
{
XDRMatcher::XDRMatcher(std::string const& query) : mQuery(query)
{
}

XDRFieldExtractor::XDRFieldExtractor(std::string const& query) : mQuery(query)
{
}

std::vector<std::string>
XDRFieldExtractor::getColumnNames() const
{
    return mFieldList->getColumnNames();
}

XDRAccumulator::XDRAccumulator(std::string const& query) : mQuery(query)
{
}

std::vector<std::shared_ptr<Accumulator>> const&
XDRAccumulator::getAccumulators() const
{
    return mAccumulatorList->getAccumulators();
}

} // namespace xdrquery
