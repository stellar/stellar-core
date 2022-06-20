// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/xdrquery/XDRFieldResolver.h"
#include "util/xdrquery/XDRQueryEval.h"
#include <string>
#include <variant>

namespace xdrquery
{
// Helper to match multiple XDR messages of the same type using the provided
// query.
// Queries may consist of literals, XDR fields, comparisons and boolean
// operations, e.g.
// `data.account.balance >= 100000 || data.trustLine.balance < 5000`
// See more examples in `XDRQueryTests`.
class XDRMatcher
{
  public:
    XDRMatcher(std::string const& query);

    template <typename T>
    bool
    matchXDR(T const& xdrMessage)
    {
        return matchInternal(
            [&xdrMessage, this](std::vector<std::string> const& fieldPath) {
                if (mFirstMatch)
                {
                    mFirstMatch = false;
                    return getXDRFieldValidated(xdrMessage, fieldPath);
                }
                return getXDRField(xdrMessage, fieldPath);
            });
    }

  private:
    bool matchInternal(FieldResolver const& fieldResolver);

    std::string const mQuery;
    std::unique_ptr<BoolEvalNode> mEvalRoot;
    bool mFirstMatch = true;
};
} // namespace xdrquery
