//
// Copyright (C) 2004-2008 Maciej Sobczak, Stephen Hutton
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#define SOCI_POSTGRESQL_SOURCE
#include "soci-postgresql.h"
#include <libpq/libpq-fs.h> // libpq
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

#ifdef SOCI_POSTGRESQL_NOPARAMS
#ifndef SOCI_POSTGRESQL_NOBINDBYNAME
#define SOCI_POSTGRESQL_NOBINDBYNAME
#endif // SOCI_POSTGRESQL_NOBINDBYNAME
#endif // SOCI_POSTGRESQL_NOPARAMS

#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif

using namespace soci;
using namespace soci::details;


postgresql_blob_backend::postgresql_blob_backend(
    postgresql_session_backend & session)
    : session_(session), oid_(InvalidOid), fd_(-1)
{
    if (session.transactionLevel_ <= 0)
    {
        throw soci_error("Postgresql BLOBs can only be used inside transactions");
    }
}

postgresql_blob_backend::~postgresql_blob_backend()
{
    ensure_closed();
}

void
postgresql_blob_backend::ensure_closed()
{
    if (fd_ != -1)
    {
        // We don't check the return value of lo_close here because we _might_
        // be destructing after a transaction commits, at which point our fd is
        // invalid and closing would return error; we do a best-effort close and
        // allow the mandatory enclosing transaction-close to close any fds we
        // don't manage to close ourselves.
        lo_close(session_.conn_, fd_);
        fd_ = -1;
    }
    oid_ = InvalidOid;
}

void
postgresql_blob_backend::ensure_open()
{
    if (oid_ == 0 || oid_ == InvalidOid || fd_ == -1)
    {
        ensure_closed();
        oid_ = lo_creat(session_.conn_, INV_READ|INV_WRITE);
        if (oid_ == InvalidOid)
        {
            throw soci_error("Cannot create BLOB.");
        }
        fd_ = lo_open(session_.conn_, oid_, INV_READ|INV_WRITE);
        if (fd_ == -1)
        {
            throw soci_error("Cannot open BLOB.");
        }
    }
}

std::size_t postgresql_blob_backend::get_len()
{
    ensure_open();
    int const pos = lo_lseek(session_.conn_, fd_, 0, SEEK_END);
    if (pos == -1)
    {
        throw soci_error("Cannot retrieve the size of BLOB.");
    }

    return static_cast<std::size_t>(pos);
}

std::size_t postgresql_blob_backend::read(
    std::size_t offset, char * buf, std::size_t toRead)
{
    ensure_open();
    int const pos = lo_lseek(session_.conn_, fd_,
        static_cast<int>(offset), SEEK_SET);
    if (pos == -1)
    {
        throw soci_error("Cannot seek in BLOB.");
    }

    int const readn = lo_read(session_.conn_, fd_, buf, toRead);
    if (readn < 0)
    {
        throw soci_error("Cannot read from BLOB.");
    }

    return static_cast<std::size_t>(readn);
}

std::size_t postgresql_blob_backend::write(
    std::size_t offset, char const * buf, std::size_t toWrite)
{
    ensure_open();
    int const pos = lo_lseek(session_.conn_, fd_,
        static_cast<int>(offset), SEEK_SET);
    if (pos == -1)
    {
        throw soci_error("Cannot seek in BLOB.");
    }

    int const written = lo_write(session_.conn_, fd_,
        const_cast<char *>(buf), toWrite);
    if (written < 0)
    {
        throw soci_error("Cannot write to BLOB.");
    }

    return static_cast<std::size_t>(written);
}

std::size_t postgresql_blob_backend::append(
    char const * buf, std::size_t toWrite)
{
    ensure_open();
    int const pos = lo_lseek(session_.conn_, fd_, 0, SEEK_END);
    if (pos == -1)
    {
        throw soci_error("Cannot seek in BLOB.");
    }

    int const writen = lo_write(session_.conn_, fd_,
        const_cast<char *>(buf), toWrite);
    if (writen < 0)
    {
        throw soci_error("Cannot append to BLOB.");
    }

    return static_cast<std::size_t>(writen);
}

void postgresql_blob_backend::trim(std::size_t /* newLen */)
{
    throw soci_error("Trimming BLOBs is not supported.");
}
