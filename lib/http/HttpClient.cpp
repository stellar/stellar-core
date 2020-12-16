// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include <iostream>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include "util/Logging.h"

using asio::ip::tcp;

int
http_request(std::string domain, std::string path, unsigned short port, std::string& ret)
{
    try
    {
        std::ostringstream retSS;
        asio::io_service io_service;

        asio::ip::tcp::socket socket(io_service);
        asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(domain),
                                         port);
        socket.connect(endpoint);

        // Form the request. We specify the "Connection: close" header so that
        // the
        // server will close the socket after transmitting the response. This
        // will
        // allow us to treat all data up until the EOF as the content.
        asio::streambuf request;
        std::ostream request_stream(&request);
        request_stream << "GET " << path << " HTTP/1.0\r\n";
        request_stream << "Host: " << domain << "\r\n";
        request_stream << "Accept: */*\r\n";
        request_stream << "Connection: close\r\n\r\n";

        // Send the request.
        asio::write(socket, request);

        // Read the response status line. The response streambuf will
        // automatically
        // grow to accommodate the entire line. The growth may be limited by
        // passing
        // a maximum size to the streambuf constructor.
        asio::streambuf response;
        asio::read_until(socket, response, "\r\n");

        // Check that response is OK.
        std::istream response_stream(&response);
        std::string http_version;
        response_stream >> http_version;
        unsigned int status_code;
        response_stream >> status_code;
        std::string status_message;
        std::getline(response_stream, status_message);
        if (!response_stream || http_version.substr(0, 5) != "HTTP/")
        {
            LOG_DEBUG(DEFAULT_LOG, "Invalid response\n");
            return 1;
        }
        if (status_code != 200)
        {
            LOG_DEBUG(DEFAULT_LOG, "Response returned with status code {}\n", status_code);
            return status_code;
        }

        // Read the response headers, which are terminated by a blank line.
        asio::read_until(socket, response, "\r\n\r\n");

        // Process the response headers.
        std::string header;
        while (std::getline(response_stream, header) && header != "\r")
            std::cout << header << "\n";
        std::cout << "\n";

        // Write whatever content we already have to output.
        if (response.size() > 0)
            retSS << &response;

        // Read until EOF, writing data to output as we go.
        asio::error_code error;
        while (asio::read(socket, response, asio::transfer_at_least(1), error))
            retSS << &response;
        if (error != asio::error::eof)
            throw asio::system_error(error);

        ret = retSS.str();
        return 200;
    }
    catch (std::exception& e)
    {
        LOG_DEBUG(DEFAULT_LOG, "Exception: {}\n", e.what());
        return 1;
    }
}
