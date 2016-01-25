#pragma once

//
// server.hpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2014 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include <string>
#include <map>
#include <functional>
#include "connection.hpp"
#include "connection_manager.hpp"

namespace http {
namespace server {

/// The top-level class of the HTTP server.
class server
{

public:
    typedef std::function<void(const std::string&, std::string&)> routeHandler;
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // construct a server that just doesn't listen
    server(asio::io_service& io_service);

    /// Construct the server to listen on the specified TCP address and port
    explicit server(asio::io_service& io_service,
                    const std::string& address, unsigned short port, int maxClient);
    ~server();

    void addRoute(const std::string& routeName, routeHandler callback);
    void add404(routeHandler callback);

    void handle_request(const request& req, reply& rep);

    static void parseParams(const std::string& params, std::map<std::string, std::string>& retMap);

private:
    /// Perform an asynchronous accept operation.
    void do_accept();

    /// Perform URL-decoding on a string. Returns false if the encoding was
    /// invalid.
    static bool url_decode(const std::string& in, std::string& out);

    /// The io_service used to perform asynchronous operations.
    asio::io_service& io_service_;

    /// The signal_set is used to register for process termination notifications.
    asio::signal_set signals_;

    /// Acceptor used to listen for incoming connections.
    asio::ip::tcp::acceptor acceptor_;

    /// The connection manager which owns all live connections.
    connection_manager connection_manager_;

    /// The next socket to be accepted.
    asio::ip::tcp::socket socket_;

    std::map<std::string, routeHandler> mRoutes;
};

} // namespace server
} // namespace http
