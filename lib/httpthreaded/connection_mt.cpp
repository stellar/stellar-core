//
// connection.cpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2024 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "connection_mt.hpp"
#include "server_mt.hpp"
#include <utility>
#include <vector>

#define MAX_REQUEST_SIZE (1024 * 1024 * 10) // 10 MB

namespace httpThreaded
{
namespace server
{

connection::connection(asio::ip::tcp::socket socket, server& handler)
    : socket_(std::move(socket)), request_handler_(handler), received_count_(0)
{
}

void
connection::start()
{
    do_read();
}

void
connection::do_read()
{
    auto self(shared_from_this());
    socket_.async_read_some(
        asio::buffer(buffer_),
        [this, self](asio::error_code ec, std::size_t bytes_transferred) {
            if (!ec)
            {
                request_parser::result_type result;
                std::tie(result, std::ignore) =
                    request_parser_.parse(request_, buffer_.data(),
                                          buffer_.data() + bytes_transferred);
                received_count_ += bytes_transferred;

                if (result == request_parser::bad ||
                    received_count_ > MAX_REQUEST_SIZE)
                {
                    reply_ = reply::stock_reply(reply::bad_request);
                    do_write();
                }
                else if (result == request_parser::good)
                {
                    request_handler_.handle_request(request_, reply_);
                    do_write();
                }
                else
                {
                    do_read();
                }
            }
            // If an error occurs then no new asynchronous operations are
            // started. This means that all shared_ptr references to the
            // connection object will disappear and the object will be
            // destroyed automatically after this handler returns. The
            // connection class's destructor closes the socket.
        });
}

void
connection::do_write()
{
    auto self(shared_from_this());
    asio::async_write(socket_, reply_.to_buffers(),
                      [this, self](asio::error_code ec, std::size_t) {
                          if (!ec)
                          {
                              // Initiate graceful connection closure.
                              asio::error_code ignored_ec;
                              socket_.shutdown(
                                  asio::ip::tcp::socket::shutdown_both,
                                  ignored_ec);
                          }

                          // No new asynchronous operations are started. This
                          // means that all shared_ptr references to the
                          // connection object will disappear and the object
                          // will be destroyed automatically after this handler
                          // returns. The connection class's destructor closes
                          // the socket.
                      });
}

} // namespace server
} // namespace httpThreaded
