// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <stdexcept>

namespace xdrquery
{
// A common exception for any XDR query-related errors, including parsing, field
// resolution, type checks etc.
class XDRQueryError : public std::invalid_argument
{
  public:
    explicit XDRQueryError(std::string const& msg) : std::invalid_argument{msg}
    {
    }
    virtual ~XDRQueryError() = default;
};
} // namespace xdrquery
