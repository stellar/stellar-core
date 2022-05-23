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

// This is a simple engine for evaluating boolean expresions containing literals
// and XDR fields.
namespace xdrquery
{
// This type represents an optional XDR fields that is not set.
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

// A function that resolves the field path to an actual value.
using FieldResolver =
    std::function<ResultType(std::vector<std::string> const&)>;

enum class EvalNodeType
{
    LITERAL,
    FIELD,
    BOOL_OP,
    COMPARISON_OP
};

// Expression node that can be evaluated.
struct EvalNode
{
    virtual ResultType eval(FieldResolver const& fieldResolver) const = 0;
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

    ResultType eval(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

    // We only resolve integer literals when they're compared to XDR fields and
    // for simplicity do that lazily via calling this function in eval(). Hence
    // it has to be `const` and `mValue` has to be mutable.
    void resolveIntType(ResultValueType const& fieldValue,
                        std::vector<std::string> const& fieldPath) const;

    LiteralNodeType mType;
    mutable ResultType mValue;
};

// Node representing an XDR field in expression.
struct FieldNode : public EvalNode
{
    FieldNode(std::string const& initField);

    ResultType eval(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

    std::vector<std::string> mFieldPath;
};

// `EvalNode` that always has a `bool` evaluation result.
struct BoolEvalNode : public EvalNode
{
    ResultType eval(FieldResolver const& fieldResolver) const override;

    virtual bool evalBool(FieldResolver const& fieldResolver) const = 0;
};

enum class BoolOpNodeType
{
    AND,
    OR
};

// Node for binary bool operations.
struct BoolOpNode : public BoolEvalNode
{
    BoolOpNode(BoolOpNodeType nodeType, std::unique_ptr<BoolEvalNode> left,
               std::unique_ptr<BoolEvalNode> right);

    bool evalBool(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

  private:
    BoolOpNodeType mType;
    std::unique_ptr<BoolEvalNode> mLeft;
    std::unique_ptr<BoolEvalNode> mRight;
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
    ComparisonNode(ComparisonNodeType nodeType, std::unique_ptr<EvalNode> left,
                   std::unique_ptr<EvalNode> right);

    bool evalBool(FieldResolver const& fieldResolver) const override;

    EvalNodeType getType() const override;

  private:
    bool compareNullFields(bool leftIsNull, bool rightIsNull) const;

    ComparisonNodeType mType;
    std::unique_ptr<EvalNode> mLeft;
    std::unique_ptr<EvalNode> mRight;
};
} // namespace xdrquery
