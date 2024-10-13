#pragma once

//
// server.hpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2024 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "connection.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace httpThreaded
{
namespace server
{

/// The top-level class of the HTTP server.
class server
{

  public:
    // If route handler returns true, send response with 200 OK status.
    // Otherwise, send 404 Not Found.
    typedef std::function<bool(const std::string&, const std::string&,
                               std::string&)>
        routeHandler;
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    /// Construct the server to listen on the specified TCP address and port.
    explicit server(const std::string& address, unsigned short port,
                    int maxClient, std::size_t threadPoolSize);
    ~server();

    void addRoute(const std::string& routeName, routeHandler callback);
    void add404(routeHandler callback);

    void handle_request(const request& req, reply& rep);

    /// Start the server's io_context loop. Returns PIDs of all worker threads.
    std::vector<std::thread::id> start();

    static void parseParams(const std::string& params,
                            std::map<std::string, std::string>& retMap);
    static void
    parsePostParams(const std::string& params,
                    std::map<std::string, std::vector<std::string>>& retMap);

  private:
    /// Perform an asynchronous accept operation.
    void do_accept();

    void stop();

    /// Wait for a request to stop the server.
    void do_await_stop();

    /// Perform URL-decoding on a string. Returns false if the encoding was
    /// invalid.
    static bool url_decode(const std::string& in, std::string& out);

    /// The number of threads that will call io_context::run().
    std::size_t thread_pool_size_;

    /// The io_context used to perform asynchronous operations.
    asio::io_context io_context_;

    /// The signal_set is used to register for process termination
    /// notifications.
    asio::signal_set signals_;

    /// Acceptor used to listen for incoming connections.
    asio::ip::tcp::acceptor acceptor_;

    std::vector<std::thread> worker_threads_{};

    std::map<std::string, routeHandler> mRoutes;
};

} // namespace server
} // namespace httpThreaded
