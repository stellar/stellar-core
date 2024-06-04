//
// header.hpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2024 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef HTTP_THREADED_HEADER_HPP
#define HTTP_THREADED_HEADER_HPP

#include <string>

namespace httpThreaded
{
namespace server
{

struct header
{
    std::string name;
    std::string value;
};

} // namespace server
} // namespace httpThreaded

#endif // HTTP_THREADED_HEADER_HPP
