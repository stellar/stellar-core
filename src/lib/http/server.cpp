//
// server.cpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2014 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "server.hpp"
#include <signal.h>
#include <utility>

namespace http
{
namespace server
{

server::server(const std::string& address, const int port)
    : io_service_()
    , signals_(io_service_)
    , acceptor_(io_service_)
    , connection_manager_()
    , socket_(io_service_)
{
    // Register to handle the signals that indicate when the server should exit.
    // It is safe to register for the same signal multiple times in a program,
    // provided all registration for the specified signal is made through Asio.
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
#if defined(SIGQUIT)
    signals_.add(SIGQUIT);
#endif // defined(SIGQUIT)

    do_await_stop();

    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(address),
                                     port);
    // Open the acceptor with the option to reuse the address (i.e.
    // SO_REUSEADDR).
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    do_accept();
}

void
server::addRoute(const std::string& routeName, routeHandler callback)
{
    mRoutes[routeName] = callback;
}

void
server::run()
{

    // The io_service::run() call will block until all asynchronous operations
    // have finished. While the server is running, there is always at least one
    // asynchronous operation outstanding: the asynchronous accept call waiting
    // for new incoming connections.
    io_service_.run();
}

void
server::do_accept()
{
    acceptor_.async_accept(socket_, [this](std::error_code ec)
                           {
        // Check whether the server was stopped by a signal before this
        // completion handler had a chance to run.
        if (!acceptor_.is_open())
        {
            return;
        }

        if (!ec)
        {
            connection_manager_.start(std::make_shared<connection>(
                std::move(socket_), connection_manager_, *this));
        }

        do_accept();
    });
}

void
server::do_await_stop()
{
    signals_.async_wait([this](std::error_code /*ec*/, int /*signo*/)
                        {
                            // The server is stopped by canceling all
                            // outstanding asynchronous
                            // operations. Once all operations have finished the
                            // io_service::run()
                            // call will exit.
                            acceptor_.close();
                            connection_manager_.stop_all();
                        });
}

void
server::handle_request(const request& req, reply& rep)
{
    // Decode url to path.
    std::string request_path;
    if (!url_decode(req.uri, request_path))
    {
        rep = reply::stock_reply(reply::bad_request);
        return;
    }

    if (request_path.size() && request_path[0] == '/')
        request_path = request_path.substr(1);

    std::string command;
    std::string params;
    int pos = request_path.find('?');
    if (pos == std::string::npos)
        command = request_path;
    else
    {
        command = request_path.substr(0, pos);
        params = request_path.substr(pos);
    }

    if (mRoutes.find(command) != mRoutes.end())
    {
        mRoutes[command](params, rep.content);

        rep.status = reply::ok;
        rep.headers.resize(2);
        rep.headers[0].name = "Content-Length";
        rep.headers[0].value = std::to_string(rep.content.size());
        rep.headers[1].name = "Content-Type";
        rep.headers[1].value = "text/plain";
    }
    else
    {
        rep = reply::stock_reply(reply::not_found);
        return;
    }
}

bool
server::url_decode(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%')
        {
            if (i + 3 <= in.size())
            {
                int value = 0;
                std::istringstream is(in.substr(i + 1, 2));
                if (is >> std::hex >> value)
                {
                    out += static_cast<char>(value);
                    i += 2;
                }
                else
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        else if (in[i] == '+')
        {
            out += ' ';
        }
        else
        {
            out += in[i];
        }
    }
    return true;
}

} // namespace server
} // namespace http
