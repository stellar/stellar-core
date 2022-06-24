// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/xdrquery/XDRQueryEval.h"
#include "fmt/format.h"
#include "util/xdrquery/XDRQueryError.h"

namespace fmt
{
template <> struct formatter<xdrquery::NullField>
{
    template <typename FormatContext>
    auto
    format(xdrquery::NullField, FormatContext& ctx)
    {
        return format_to(ctx.out(), "NULL");
    }
};
} // fmt

namespace xdrquery
{
bool
NullField::operator==(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operator!=(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operator<(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operator<=(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operator>(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operator>=(NullField other) const
{
    return operationNotSupported();
}
bool
NullField::operationNotSupported() const
{
    throw std::runtime_error("Null fields should not be compared directly.");
    return false;
}

LiteralNode::LiteralNode(LiteralNodeType valueType, std::string const& val)
    : mType(valueType), mValue(val)
{
    if (mType == LiteralNodeType::NULL_LITERAL)
    {
        mValue = NullField();
    }
}

ResultType
LiteralNode::eval(FieldResolver const& fieldResolver) const
{
    return mValue;
}

EvalNodeType
LiteralNode::getType() const
{
    return EvalNodeType::LITERAL;
}

void
LiteralNode::resolveIntType(ResultValueType const& fieldValue,
                            std::vector<std::string> const& fieldPath) const
{
    if (std::holds_alternative<std::string>(fieldValue))
    {
        std::string valueStr = resultToString(*mValue);
        throw XDRQueryError(fmt::format(
            FMT_STRING("String field '{}' is compared with int value: {}."),
            fmt::join(fieldPath, "."), valueStr));
    }
    std::string valueStr = std::get<std::string>(*mValue);
    try
    {
        mValue = std::visit(
            [&valueStr](auto&& v) -> ResultType {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>)
                {
                    auto v = std::stoi(valueStr);
                    if (v > std::numeric_limits<T>::max())
                    {
                        throw std::out_of_range("");
                    }
                    return std::make_optional<ResultValueType>(
                        std::in_place_type<int32_t>, std::stoi(valueStr));
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                    return std::make_optional<ResultValueType>(
                        std::in_place_type<int64_t>, std::stoll(valueStr));
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    auto v = std::stoul(valueStr);
                    if (v > std::numeric_limits<T>::max())
                    {
                        throw std::out_of_range("");
                    }
                    return std::make_optional<ResultValueType>(
                        std::in_place_type<uint32_t>, v);
                }
                else if constexpr (std::is_same_v<T, uint64_t>)
                    return std::make_optional<ResultValueType>(
                        std::in_place_type<uint64_t>, std::stoull(valueStr));
                throw std::runtime_error("Unexpected field type.");
            },
            fieldValue);
    }
    catch (std::out_of_range&)
    {
        throw XDRQueryError(fmt::format(
            FMT_STRING("Value for field '{}' is out of type range: {}."),
            fmt::join(fieldPath, "."), valueStr));
    }
}

FieldNode::FieldNode(std::string const& initField)
{
    mFieldPath.push_back(initField);
}

ResultType
FieldNode::eval(FieldResolver const& fieldResolver) const
{
    return fieldResolver(mFieldPath);
}

EvalNodeType
FieldNode::getType() const
{
    return EvalNodeType::FIELD;
}

ResultType
BoolEvalNode::eval(FieldResolver const& fieldResolver) const
{
    return evalBool(fieldResolver);
}

BoolOpNode::BoolOpNode(BoolOpNodeType nodeType,
                       std::shared_ptr<BoolEvalNode> left,
                       std::shared_ptr<BoolEvalNode> right)
    : mType(nodeType), mLeft(std::move(left)), mRight(std::move(right))
{
}

bool
BoolOpNode::evalBool(FieldResolver const& fieldResolver) const
{
    switch (mType)
    {
    case BoolOpNodeType::AND:
        return mLeft->evalBool(fieldResolver) &&
               mRight->evalBool(fieldResolver);
    case BoolOpNodeType::OR:
        return mLeft->evalBool(fieldResolver) ||
               mRight->evalBool(fieldResolver);
    }
}

EvalNodeType
BoolOpNode::getType() const
{
    return EvalNodeType();
}

ComparisonNode::ComparisonNode(ComparisonNodeType nodeType,
                               std::shared_ptr<EvalNode> left,
                               std::shared_ptr<EvalNode> right)
    : mType(nodeType), mLeft(std::move(left)), mRight(std::move(right))
{
    // Keep the field as the left argument for simplicity of type check during
    // evaluation.
    if (mRight->getType() == EvalNodeType::FIELD)
    {
        std::swap(mLeft, mRight);
        // Invert the operation as we have swapped operands.
        switch (mType)
        {
        case ComparisonNodeType::LT:
            mType = ComparisonNodeType::GT;
            break;
        case ComparisonNodeType::LE:
            mType = ComparisonNodeType::GE;
            break;
        case ComparisonNodeType::GT:
            mType = ComparisonNodeType::LT;
            break;
        case ComparisonNodeType::GE:
            mType = ComparisonNodeType::LE;
            break;
        default:
            break;
        }
    }
}

bool
ComparisonNode::evalBool(FieldResolver const& fieldResolver) const
{
    auto leftType = mLeft->getType();
    auto leftVal = mLeft->eval(fieldResolver);

    if (!leftVal)
    {
        return false;
    }

    auto rightType = mRight->getType();
    if (leftType == EvalNodeType::FIELD && rightType == EvalNodeType::LITERAL)
    {
        // Lazily resolve the type of the int literal using the field type.
        // This allows to correctly check the literal range and simplifies the
        // comparisons.
        auto* lit = static_cast<LiteralNode const*>(mRight.get());
        if (lit->mType == LiteralNodeType::INT &&
            std::holds_alternative<std::string>(*lit->mValue))
        {
            auto* field = static_cast<FieldNode const*>(mLeft.get());
            lit->resolveIntType(*leftVal, field->mFieldPath);
        }
    }
    auto rightVal = mRight->eval(fieldResolver);
    if (!rightVal)
    {
        return false;
    }

    bool leftIsNull = std::holds_alternative<NullField>(*leftVal);
    bool rightIsNull = std::holds_alternative<NullField>(*rightVal);
    if (leftIsNull || rightIsNull)
    {
        return compareNullFields(leftIsNull, rightIsNull);
    }

    if (leftVal->index() != rightVal->index())
    {
        throw XDRQueryError(fmt::format(
            FMT_STRING("Type mismatch between values `{}` and `{}`."),
            resultToString(*leftVal), resultToString(*rightVal)));
    }

    switch (mType)
    {
    case ComparisonNodeType::EQ:
        return *leftVal == *rightVal;
    case ComparisonNodeType::NE:
        return *leftVal != *rightVal;
    case ComparisonNodeType::LT:
        return *leftVal < *rightVal;
    case ComparisonNodeType::LE:
        return *leftVal <= *rightVal;
    case ComparisonNodeType::GT:
        return *leftVal > *rightVal;
    case ComparisonNodeType::GE:
        return *leftVal >= *rightVal;
    }
}

EvalNodeType
ComparisonNode::getType() const
{
    return EvalNodeType::COMPARISON_OP;
}

bool
ComparisonNode::compareNullFields(bool leftIsNull, bool rightIsNull) const
{
    switch (mType)
    {
    case ComparisonNodeType::EQ:
        return leftIsNull == rightIsNull;
    case ComparisonNodeType::NE:
        return leftIsNull != rightIsNull;
    case ComparisonNodeType::LT:
    case ComparisonNodeType::LE:
    case ComparisonNodeType::GT:
    case ComparisonNodeType::GE:
        throw XDRQueryError(
            "Fields can only be compared with `NULL` using `==` and `!=`.");
    }
}

Accumulator::Accumulator(AccumulatorType nodeType)
    : Accumulator(nodeType, nullptr)
{
}

Accumulator::Accumulator(AccumulatorType nodeType,
                         std::shared_ptr<FieldNode> field)
    : mType(nodeType), mField(field)
{
    switch (mType)
    {
    case AccumulatorType::AVERAGE:
        mValue.emplace<double>() = 0;
        break;
    case AccumulatorType::COUNT:
    case AccumulatorType::SUM:
        mValue.emplace<uint64_t>() = 0;
        break;
    }
}

void
Accumulator::addEntry(FieldResolver const& fieldResolver)
{
    ResultType fieldValue;
    if (mType != AccumulatorType::COUNT)
    {
        fieldValue = mField->eval(fieldResolver);
        if (!fieldValue)
        {
            return;
        }
    }
    ++mCount;
    switch (mType)
    {
    case AccumulatorType::AVERAGE:
    {
        std::get<double>(mValue) += std::visit(
            [](auto&& v) -> double {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_integral_v<T>)
                {
                    return static_cast<double>(v);
                }
                throw XDRQueryError("Encountered non-aggregatable field.");
            },
            *fieldValue);
    }
    break;
    case AccumulatorType::SUM:
        std::get<uint64_t>(mValue) += std::visit(
            [](auto&& v) -> uint64_t {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_integral_v<T>)
                {
                    return static_cast<uint64_t>(v);
                }
                throw XDRQueryError("Encountered non-aggregatable field.");
            },
            *fieldValue);
        break;
    case AccumulatorType::COUNT:
        break;
    }
}

AccumulatorResultType
Accumulator::getValue() const
{
    if (mType == AccumulatorType::COUNT)
    {
        return mCount;
    }
    if (mType == AccumulatorType::AVERAGE)
    {
        if (mCount == 0)
        {
            return AccumulatorResultType(0.0);
        }
        return std::get<double>(mValue) / mCount;
    }
    return mValue;
}

std::string
Accumulator::getName() const
{
    switch (mType)
    {
    case AccumulatorType::AVERAGE:
        return fmt::format(FMT_STRING("avg({})"),
                           fmt::join(mField->mFieldPath, "."));
    case AccumulatorType::SUM:
        return fmt::format(FMT_STRING("sum({})"),
                           fmt::join(mField->mFieldPath, "."));
    case AccumulatorType::COUNT:
        return "count";
    }
}

AccumulatorList::AccumulatorList(std::shared_ptr<Accumulator> accumulator)
{
    mAccumulators.emplace_back(accumulator);
}

void
AccumulatorList::addAccumulator(std::shared_ptr<Accumulator> accumulator)
{
    mAccumulators.emplace_back(accumulator);
}

void
AccumulatorList::addEntry(FieldResolver const& fieldResolver) const
{
    for (auto const& accumulator : mAccumulators)
    {
        accumulator->addEntry(fieldResolver);
    }
}

std::vector<std::shared_ptr<Accumulator>> const&
AccumulatorList::getAccumulators() const
{
    return mAccumulators;
}

FieldList::FieldList(std::shared_ptr<FieldNode> field)
{
    mFields.emplace_back(field);
}

void
FieldList::addField(std::shared_ptr<FieldNode> field)
{
    mFields.emplace_back(field);
}

std::vector<ResultType>
FieldList::getValues(FieldResolver const& fieldResolver) const
{
    std::vector<ResultType> res;
    res.reserve(mFields.size());
    for (auto const& field : mFields)
    {
        res.emplace_back(field->eval(fieldResolver));
    }
    return res;
}

std::vector<std::string>
FieldList::getFieldNames() const
{
    std::vector<std::string> names;
    names.reserve(mFields.size());
    for (auto const& field : mFields)
    {
        names.emplace_back(fmt::to_string(fmt::join(field->mFieldPath, ".")));
    }
    return names;
}

std::string
resultToString(ResultValueType const& result)
{
    return std::visit([](auto&& v) { return fmt::to_string(v); }, result);
}

} // namespace xdrquery
