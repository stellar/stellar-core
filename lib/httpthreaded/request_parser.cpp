//
// request_parser.cpp
// ~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2024 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "request_parser.hpp"
#include "request.hpp"

namespace httpThreaded
{
namespace server
{

request_parser::request_parser() : header_state_(method_start)
{
}

void
request_parser::reset()
{
    header_state_ = method_start;
    parsed_header_ = false;
    body_consumed_bytes_ = 0;
}

request_parser::result_type
request_parser::consumeHeader(request& req, char input)
{
    switch (header_state_)
    {
    case method_start:
        if (!is_char(input) || is_ctl(input) || is_tspecial(input))
        {
            return bad;
        }
        else
        {
            header_state_ = method;
            req.method.push_back(input);
            return indeterminate;
        }
    case method:
        if (input == ' ')
        {
            header_state_ = uri;
            return indeterminate;
        }
        else if (!is_char(input) || is_ctl(input) || is_tspecial(input))
        {
            return bad;
        }
        else
        {
            req.method.push_back(input);
            return indeterminate;
        }
    case uri:
        if (input == ' ')
        {
            header_state_ = http_version_h;
            return indeterminate;
        }
        else if (is_ctl(input))
        {
            return bad;
        }
        else
        {
            req.uri.push_back(input);
            return indeterminate;
        }
    case http_version_h:
        if (input == 'H')
        {
            header_state_ = http_version_t_1;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_t_1:
        if (input == 'T')
        {
            header_state_ = http_version_t_2;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_t_2:
        if (input == 'T')
        {
            header_state_ = http_version_p;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_p:
        if (input == 'P')
        {
            header_state_ = http_version_slash;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_slash:
        if (input == '/')
        {
            req.http_version_major = 0;
            req.http_version_minor = 0;
            header_state_ = http_version_major_start;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_major_start:
        if (is_digit(input))
        {
            req.http_version_major = req.http_version_major * 10 + input - '0';
            header_state_ = http_version_major;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_major:
        if (input == '.')
        {
            header_state_ = http_version_minor_start;
            return indeterminate;
        }
        else if (is_digit(input))
        {
            req.http_version_major = req.http_version_major * 10 + input - '0';
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_minor_start:
        if (is_digit(input))
        {
            req.http_version_minor = req.http_version_minor * 10 + input - '0';
            header_state_ = http_version_minor;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case http_version_minor:
        if (input == '\r')
        {
            header_state_ = expecting_newline_1;
            return indeterminate;
        }
        else if (is_digit(input))
        {
            req.http_version_minor = req.http_version_minor * 10 + input - '0';
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case expecting_newline_1:
        if (input == '\n')
        {
            header_state_ = header_line_start;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case header_line_start:
        if (input == '\r')
        {
            header_state_ = expecting_newline_3;
            return indeterminate;
        }
        else if (!req.headers.empty() && (input == ' ' || input == '\t'))
        {
            header_state_ = header_lws;
            return indeterminate;
        }
        else if (!is_char(input) || is_ctl(input) || is_tspecial(input))
        {
            return bad;
        }
        else
        {
            req.headers.push_back(header());
            req.headers.back().name.push_back(input);
            header_state_ = header_name;
            return indeterminate;
        }
    case header_lws:
        if (input == '\r')
        {
            header_state_ = expecting_newline_2;
            return indeterminate;
        }
        else if (input == ' ' || input == '\t')
        {
            return indeterminate;
        }
        else if (is_ctl(input))
        {
            return bad;
        }
        else
        {
            header_state_ = header_value;
            req.headers.back().value.push_back(input);
            return indeterminate;
        }
    case header_name:
        if (input == ':')
        {
            header_state_ = space_before_header_value;
            return indeterminate;
        }
        else if (!is_char(input) || is_ctl(input) || is_tspecial(input))
        {
            return bad;
        }
        else
        {
            req.headers.back().name.push_back(input);
            return indeterminate;
        }
    case space_before_header_value:
        if (input == ' ')
        {
            header_state_ = header_value;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case header_value:
        if (input == '\r')
        {
            header_state_ = expecting_newline_2;
            return indeterminate;
        }
        else if (is_ctl(input))
        {
            return bad;
        }
        else
        {
            req.headers.back().value.push_back(input);
            return indeterminate;
        }
    case expecting_newline_2:
        if (input == '\n')
        {
            header_state_ = header_line_start;
            return indeterminate;
        }
        else
        {
            return bad;
        }
    case expecting_newline_3:
        return (input == '\n') ? good : bad;
    default:
        return bad;
    }
}

request_parser::result_type
request_parser::initializeForBody(request const& req)
{
    for (auto const& header : req.headers)
    {
        if (header.name == "Content-Length")
        {
            body_content_length_ = stoi(header.value);
            if (body_content_length_ == 0)
            {
                return bad;
            }

            return good;
        }
    }

    return bad;
}

request_parser::result_type
request_parser::consumeBody(request& req, char input)
{
    if (is_ctl(input))
    {
        return bad;
    }

    req.body.push_back(input);
    ++body_consumed_bytes_;
    if (body_consumed_bytes_ == body_content_length_)
    {
        return good;
    }

    return indeterminate;
}

bool
request_parser::is_char(int c)
{
    return c >= 0 && c <= 127;
}

bool
request_parser::is_ctl(int c)
{
    return (c >= 0 && c <= 31) || (c == 127);
}

bool
request_parser::is_tspecial(int c)
{
    switch (c)
    {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t':
        return true;
    default:
        return false;
    }
}

bool
request_parser::is_digit(int c)
{
    return c >= '0' && c <= '9';
}

} // namespace server
} // namespace httpThreaded
