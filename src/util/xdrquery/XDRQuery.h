// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/xdrquery/XDRFieldResolver.h"
#include "util/xdrquery/XDRQueryEval.h"
#include "util/xdrquery/XDRQueryParser.h"

#include <string>
#include <variant>

namespace xdrquery
{

template <typename T>
FieldResolver
createFieldResolver(T const& xdrMessage, bool validate)
{
    return [&xdrMessage, validate](std::vector<std::string> const& fieldPath) {
        if (validate)
        {
            return getXDRFieldValidated(xdrMessage, fieldPath);
        }
        return getXDRField(xdrMessage, fieldPath);
    };
}

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
        // Lazily parse the query in order to simplify exception handling as we
        // might throw XDRQueryError both during query parsing and query
        // execution against XDR.
        bool firstEval = false;
        if (mEvalRoot == nullptr)
        {
            firstEval = true;
            auto statement = parseXDRQuery(mQuery);
            if (!std::holds_alternative<std::shared_ptr<BoolEvalNode>>(
                    statement))
            {
                throw XDRQueryError("The query doesn't evaluate to bool.");
            }
            mEvalRoot = std::get<std::shared_ptr<BoolEvalNode>>(statement);
        }
        return mEvalRoot->evalBool(createFieldResolver(xdrMessage, firstEval));
    }

  private:
    std::string const mQuery;
    std::shared_ptr<BoolEvalNode> mEvalRoot;
};

// Helper to extract leaf fields from multiple XDR messages using the provided
// query.
// Query should contain one or multiple comma-separated fields, e.g.
// `data.account.balance, data.offer.selling.assetCode`
class XDRFieldExtractor
{
  public:
    XDRFieldExtractor(std::string const& query);

    template <typename T>
    std::vector<ResultType>
    extractFields(T const& xdrMessage)
    {
        // Lazily parse the query in order to simplify exception handling as we
        // might throw XDRQueryError both during query parsing and query
        // execution against XDR.
        bool firstEval = false;
        if (mFieldList == nullptr)
        {
            firstEval = true;
            auto statement = parseXDRQuery(mQuery);
            if (!std::holds_alternative<std::shared_ptr<FieldList>>(statement))
            {
                throw XDRQueryError(
                    "The query doesn't evaluate to field list.");
            }
            mFieldList = std::get<std::shared_ptr<FieldList>>(statement);
        }
        return mFieldList->getValues(
            createFieldResolver(xdrMessage, firstEval));
    }

    // Gets names of the fields from the query.
    std::vector<std::string> getFieldNames() const;

  private:
    std::string mQuery;
    std::shared_ptr<FieldList> mFieldList;
};

// Helper that allows aggregating values of fields in multiple XDR messages
// using the provided query.
// Query consists of one or multiple comma-separated aggregation functions.
// The supported functions are `sum`, `avg` (average) and `count` (without
// arguments), e.g.:
// `sum(data.account.balance), avg(data.account.balance), count()`.
class XDRAccumulator
{
  public:
    XDRAccumulator(std::string const& query);

    template <typename T>
    void
    addEntry(T const& xdrMessage)
    {
        // Lazily parse the query in order to simplify exception handling as we
        // might throw XDRQueryError both during query parsing and query
        // execution against XDR.
        bool firstEval = false;
        if (mAccumulatorList == nullptr)
        {
            firstEval = true;
            auto statement = parseXDRQuery(mQuery);
            if (!std::holds_alternative<std::shared_ptr<AccumulatorList>>(
                    statement))
            {
                throw XDRQueryError(
                    "The query doesn't evaluate to accumulator list.");
            }
            mAccumulatorList =
                std::get<std::shared_ptr<AccumulatorList>>(statement);
        }
        mAccumulatorList->addEntry(createFieldResolver(xdrMessage, firstEval));
    }

    // Gets the accumulators with aggregated values of each field.
    std::vector<std::shared_ptr<Accumulator>> const& getAccumulators() const;

  private:
    std::string mQuery;
    std::shared_ptr<AccumulatorList> mAccumulatorList;
};
} // namespace xdrquery
