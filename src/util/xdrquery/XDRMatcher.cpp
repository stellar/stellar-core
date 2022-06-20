// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/xdrquery/XDRMatcher.h"

#include "util/xdrquery/XDRQueryParser.h"

#include <sstream>
#include <stack>
#include <unordered_map>

namespace xdrquery
{
XDRMatcher::XDRMatcher(std::string const& query) : mQuery(query)
{
}

bool
XDRMatcher::matchInternal(FieldResolver const& fieldResolver)
{
    // Lazily parse the query in order to simplify exception handling as we
    // might throw XDRQueryError both during query parsing and query execution
    // against XDR.
    if (mEvalRoot == nullptr)
    {
        mEvalRoot = parseXDRQuery(mQuery);
    }
    return mEvalRoot->evalBool(fieldResolver);
}

} // namespace xdrquery
