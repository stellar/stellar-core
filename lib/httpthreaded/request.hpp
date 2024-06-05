//
// request.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2024 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef HTTP_THREADED_REQUEST_HPP
#define HTTP_THREADED_REQUEST_HPP

#include "header.hpp"
#include <string>
#include <vector>

namespace httpThreaded
{
namespace server
{

/// A request received from a client.
struct request
{
    std::string method;
    std::string uri;
    int http_version_major;
    int http_version_minor;
    std::vector<header> headers;
    std::string body{};
};

} // namespace server
} // namespace httpThreaded

#endif // HTTP_THREADED_REQUEST_HPP
