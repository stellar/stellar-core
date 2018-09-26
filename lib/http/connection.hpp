#pragma once

//
// connection.hpp
// ~~~~~~~~~~~~~~
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

#include <array>
#include <memory>
#include "reply.hpp"
#include "request.hpp"
#include "request_parser.hpp"

namespace http {
namespace server {

class connection_manager;
class server;

/// Represents a single connection from a client.
class connection
  : public std::enable_shared_from_this<connection>
{
public:
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

  /// Construct a connection with the given socket.
  explicit connection(asio::ip::tcp::socket socket,
      connection_manager& manager, server& handler);

  /// Start the first asynchronous operation for the connection.
  void start();

  /// Stop all asynchronous operations associated with the connection.
  void stop();

private:
  /// Perform an asynchronous read operation.
  void do_read();

  /// Perform an asynchronous write operation.
  void do_write();

  /// Socket for the connection.
  asio::ip::tcp::socket socket_;

  /// The manager for this connection.
  connection_manager& connection_manager_;

  /// The handler used to process the incoming request.
  server& request_handler_;

  /// Buffer for incoming data.
  std::array<char, 8192> buffer_;

  /// Size of received data
  size_t received_count_;

  /// The incoming request.
  request request_;

  /// The parser for the incoming request.
  request_parser request_parser_;

  /// The reply to be sent back to the client.
  reply reply_;
};

typedef std::shared_ptr<connection> connection_ptr;

} // namespace server
} // namespace http
