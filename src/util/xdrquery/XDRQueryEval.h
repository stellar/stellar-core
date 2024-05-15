// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// This is a simple engine for evaluating boolean expressions containing
// literals and XDR fields.
namespace xdrquery
{
// This type represents an optional XDR field that is not set.
struct NullField
{
    bool operator==(NullField other) const;
    bool operator!=(NullField other) const;
    bool operator<(NullField other) const;
    bool operator<=(NullField other) const;
    bool operator>(NullField other) const;
    bool operator>=(NullField other) const;

  private:
    bool operationNotSupported() const;
};

// All the possible intermediate expression result types that are defined
// values.
using ResultValueType = std::variant<NullField, bool, int32_t, uint32_t,
                                     int64_t, uint64_t, std::string>;

// All the possible expression result types. When this is set to std::nullopt,
// this represents an expression that cannot be meaningfully evaluated to either
// `true` or `false`. Currently that's only set to std::nullopt for the cases
// when an XDR union has an alternative selected that is not in the field path.
using ResultType = std::optional<ResultValueType>;

// An interface for getting information from XDR structs.
struct DynamicXDRGetter
{
    // Gets a field specified by the provided path.
    virtual ResultType
    getField(std::vector<std::string> const& fieldPath) const = 0;

    // Gets the serialized XDR size of the entire struct.
    virtual uint64_t getSize() const = 0;

    virtual ~DynamicXDRGetter() = default;
};

std::string resultToString(ResultValueType const& result);

enum class EvalNodeType
{
    LITERAL,
    COLUMN,
    BOOL_OP,
    COMPARISON_OP
};

// Expression node that can be evaluated.
struct EvalNode
{
    virtual ResultType eval(DynamicXDRGetter const& xdrGetter) const = 0;
    virtual EvalNodeType getType() const = 0;

    virtual ~EvalNode() = default;
};

enum class LiteralNodeType
{
    INT,
    STR,
    NULL_LITERAL
};

// Node representing literal in the expression.
struct LiteralNode : public EvalNode
{
    LiteralNode(LiteralNodeType valueType, std::string const& val);

    ResultType eval(DynamicXDRGetter const& xdrGetter) const override;

    EvalNodeType getType() const override;

    // We only resolve integer literals when they're compared to columns and
    // for simplicity do that lazily via calling this function in eval(). Hence
    // it has to be `const` and `mValue` has to be mutable.
    void resolveIntType(ResultValueType const& columnValue,
                        std::string const& columnName) const;

    LiteralNodeType mType;
    mutable ResultType mValue;
};

// Node representing a 'column': a value inferred from an XDR entry,
// such as a field or the size of the entry.
struct ColumnNode : public EvalNode
{
    virtual std::string getName() const = 0;
};

// Node representing an XDR field in expression.
struct FieldNode : public ColumnNode
{
    FieldNode(std::string const& initField);

    ResultType eval(DynamicXDRGetter const& xdrGetter) const override;

    EvalNodeType getType() const override;

    virtual std::string getName() const override;

    std::vector<std::string> mFieldPath;
};

// Node representing the size of an XDR entry in expression.
struct EntrySizeNode : public ColumnNode
{
    ResultType eval(DynamicXDRGetter const& xdrGetter) const override;

    EvalNodeType getType() const override;

    virtual std::string getName() const override;
};

// `EvalNode` that always has a `bool` evaluation result.
struct BoolEvalNode : public EvalNode
{
    ResultType eval(DynamicXDRGetter const& xdrGetter) const override;

    virtual bool evalBool(DynamicXDRGetter const& xdrGetter) const = 0;
};

enum class BoolOpNodeType
{
    AND,
    OR
};

// Node for binary bool operations.
struct BoolOpNode : public BoolEvalNode
{
    BoolOpNode(BoolOpNodeType nodeType, std::shared_ptr<BoolEvalNode> left,
               std::shared_ptr<BoolEvalNode> right);

    bool evalBool(DynamicXDRGetter const& xdrGetter) const override;

    EvalNodeType getType() const override;

  private:
    BoolOpNodeType mType;
    std::shared_ptr<BoolEvalNode> mLeft;
    std::shared_ptr<BoolEvalNode> mRight;
};

enum class ComparisonNodeType
{
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE
};

// Node for comparing arbitrary values. Values have to have the same type.
struct ComparisonNode : public BoolEvalNode
{
    ComparisonNode(ComparisonNodeType nodeType, std::shared_ptr<EvalNode> left,
                   std::shared_ptr<EvalNode> right);

    bool evalBool(DynamicXDRGetter const& xdrGetter) const override;

    EvalNodeType getType() const override;

  private:
    bool compareNullFields(bool leftIsNull, bool rightIsNull) const;

    ComparisonNodeType mType;
    std::shared_ptr<EvalNode> mLeft;
    std::shared_ptr<EvalNode> mRight;
};

enum class AccumulatorType
{
    SUM,
    AVERAGE,
    COUNT,
};

using AccumulatorResultType = std::variant<uint64_t, double>;

struct Accumulator
{
    explicit Accumulator(AccumulatorType nodeType);
    Accumulator(AccumulatorType nodeType, std::shared_ptr<ColumnNode> column);

    void addEntry(DynamicXDRGetter const& xdrGetter);

    AccumulatorResultType getValue() const;
    std::string getName() const;

  private:
    AccumulatorType mType;
    AccumulatorResultType mValue;
    std::shared_ptr<ColumnNode> mColumn;
    uint64_t mCount = 0;
};

struct AccumulatorList
{
    explicit AccumulatorList(std::shared_ptr<Accumulator> accumulator);

    void addAccumulator(std::shared_ptr<Accumulator> accumulator);

    void addEntry(DynamicXDRGetter const& xdrGetter) const;

    std::vector<std::shared_ptr<Accumulator>> const& getAccumulators() const;

  private:
    std::vector<std::shared_ptr<Accumulator>> mAccumulators;
};

// Node representing a list of columns.
struct ColumnList
{
    explicit ColumnList(std::shared_ptr<ColumnNode> column);

    void addColumn(std::shared_ptr<ColumnNode> column);

    std::vector<ResultType> getValues(DynamicXDRGetter const& xdrGetter) const;
    std::vector<std::string> getColumnNames() const;

  private:
    std::vector<std::shared_ptr<ColumnNode>> mColumns;
};

using XDRQueryStatement =
    std::variant<std::shared_ptr<BoolEvalNode>,
                 std::shared_ptr<AccumulatorList>, std::shared_ptr<ColumnList>>;
} // namespace xdrquery
