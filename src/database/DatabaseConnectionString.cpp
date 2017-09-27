// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "DatabaseConnectionString.h"

#include <stdexcept>

namespace stellar
{

namespace
{

class ParserException : public std::runtime_error
{
  public:
    explicit ParserException(std::string const& what) : std::runtime_error{what}
    {
    }
};

using Token = std::pair<std::string::iterator, std::string::iterator>;
struct Parameter
{
    std::string name;
    Token valueToken;
};

std::string::iterator
takeSpaces(std::string::iterator it, std::string::iterator const& end)
{
    while (it != end && (*it == ' '))
        ++it;
    return it;
}

std::string::iterator
takeEquals(std::string::iterator it, std::string::iterator const& end)
{
    it = takeSpaces(it, end);
    if (it == end)
        throw ParserException{"Expected '=', found end of string"};
    if (*it != '=')
        throw ParserException{std::string{"Expected '=', found "} + *it};

    return it + 1;
}

std::string::iterator
takeEscapedToken(std::string::iterator it, std::string::iterator const& end)
{
    ++it;
    while (it != end && *it != '\'')
    {
        if (*it == '\\')
        {
            ++it;
            if (it == end)
            {
                throw ParserException{
                    "Expected any character after '\\', found end of string"};
            }
        }

        ++it;
        if (it == end)
        {
            return it;
        }
    }

    if (it == end)
    {
        throw ParserException{"Expected \', found end of string"};
    }

    return it + 1;
}

std::string::iterator
takePlainToken(std::string::iterator it, std::string::iterator const& end)
{
    while (it != end && *it != ' ' && *it != '=')
    {
        ++it;
    }

    return it;
}

Token
nextToken(std::string::iterator it, std::string::iterator const& end)
{
    it = takeSpaces(it, end);
    if (it == end)
        return {end, end};

    if (*it == '\'')
        return {it, takeEscapedToken(it, end)};
    else
        return {it, takePlainToken(it, end)};
}

Parameter
nextParameter(std::string::iterator it, std::string::iterator const& end)
{
    auto nameToken = nextToken(it, end);
    if (nameToken.first == end)
    {
        return Parameter{"", {}};
    }

    it = takeEquals(nameToken.second, end);
    auto valueToken = nextToken(it, end);

    auto name = std::string{nameToken.first, nameToken.second};
    return Parameter{name, valueToken};
}
}

std::string
removePasswordFromConnectionString(std::string connectionString)
{
    auto const protocolSeparator = std::string{"://"};
    auto const p = connectionString.find(protocolSeparator);
    if (p == std::string::npos)
    {
        return connectionString;
    }

    try
    {
        auto end = std::end(connectionString);
        auto it = std::begin(connectionString) + p + 3;
        auto lastPasswordEnd = it;
        auto result = std::string{std::begin(connectionString), it};

        while (true)
        {
            auto parameter = nextParameter(it, end);
            it = parameter.valueToken.second;
            if (parameter.name == "")
            {
                break;
            }
            if (parameter.name == "password")
            {
                result.append(lastPasswordEnd, parameter.valueToken.first);
                result.append("********");
                lastPasswordEnd = it;
            }
        }

        result.append(lastPasswordEnd, end);
        return result;
    }
    catch (ParserException&)
    {
        return connectionString;
    }
}
}
