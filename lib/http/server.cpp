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
#include <sstream>

namespace http
{
namespace server
{

server::server(asio::io_service& io_service)
    : io_service_(io_service)
    , signals_(io_service_)
    , acceptor_(io_service_)
    , connection_manager_()
    , socket_(io_service_)
{

}

server::server(asio::io_service& io_service, const std::string& address,
               unsigned short port, int maxClient)
    : io_service_(io_service)
    , signals_(io_service_)
    , acceptor_(io_service_)
    , connection_manager_()
    , socket_(io_service_)
{
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(address),
                                     port);
    // Open the acceptor with the option to reuse the address (i.e.
    // SO_REUSEADDR).
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(maxClient);
    do_accept();
}

void server::add404(routeHandler callback)
{
    addRoute("404", callback);
}

void
server::addRoute(const std::string& routeName, routeHandler callback)
{
    mRoutes[routeName] = callback;
}

void
server::do_accept()
{
    acceptor_.async_accept(socket_, [this](asio::error_code ec)
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

server::~server()
{
    acceptor_.close();
    connection_manager_.stop_all();
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
    auto pos = request_path.find('?');
    if (pos == std::string::npos)
        command = request_path;
    else
    {
        command = request_path.substr(0, pos);
        params = request_path.substr(pos);
    }

    auto it = mRoutes.find(command);
    if (it != mRoutes.end())
    {
        it->second(params, rep.content);

        rep.status = reply::ok;
        rep.headers.resize(2);
        rep.headers[0].name = "Content-Length";
        rep.headers[0].value = std::to_string(rep.content.size());
        rep.headers[1].name = "Content-Type";
        rep.headers[1].value = "application/json";
    }
    else
    {
        it = mRoutes.find("404");
        if (it != mRoutes.end())
        {
            it->second(params, rep.content);

            rep.status = reply::not_found;
            rep.headers.resize(2);
            rep.headers[0].name = "Content-Length";
            rep.headers[0].value = std::to_string(rep.content.size());
            rep.headers[1].name = "Content-Type";
            rep.headers[1].value = "text/html";
        } else
        {
            rep = reply::stock_reply(reply::not_found);
            return;
        }
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

void server::parseParams(const std::string& params, std::map<std::string, std::string>& retMap)
{
    bool buildingName=true;
    std::string name,value;
    for(auto c : params)
    {
        if(c == '?')
        {

        }else if(c == '=')
        {
            buildingName = false;
        }else if(c == '&')
        {
            buildingName = true;
            retMap[name] = value;
            name = "";
            value = "";
        } else
        {
            if(buildingName) name += c;
            else value += c;
        }
    }
    if(name.size() && value.size())
    {
        retMap[name] = value;
    }
}

} // namespace server
} // namespace http
