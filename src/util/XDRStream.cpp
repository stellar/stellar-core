// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/XDRStream.h"
#include "util/FileSystemException.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <cerrno>

namespace stellar
{

// XDRInputFileStream

void
XDRInputFileStream::close()
{
    ZoneScoped;
    mIn.close();
}

void
XDRInputFileStream::open(std::string const& filename)
{
    ZoneScoped;
    mIn.open(filename, std::ifstream::binary);
    if (!mIn)
    {
        std::string msg("failed to open XDR file: ");
        msg += filename;
        msg += ", reason: ";
        msg += std::to_string(errno);
        CLOG_ERROR(Fs, "{}", msg);
        throw FileSystemException(msg);
    }
    mIn.exceptions(std::ios::badbit);
    mSize = fs::size(mIn);
}

void
XDRInputFileStream::open(std::filesystem::path const& filename)
{
    open(filename.string());
}

XDRInputFileStream::
operator bool() const
{
    return mIn.good();
}

size_t
XDRInputFileStream::size() const
{
    return mSize;
}

std::streamoff
XDRInputFileStream::pos()
{
    releaseAssertOrThrow(!mIn.fail());
    return mIn.tellg();
}

void
XDRInputFileStream::seek(size_t pos)
{
    releaseAssertOrThrow(!mIn.fail());
    mIn.seekg(pos);
}

uint32_t
XDRInputFileStream::getXDRSize(char* buf)
{
    // Read 4 bytes of size, big-endian, with last-fragment flag bit cleared
    // (high bit of high byte).
    uint32_t sz = 0;
    sz |= static_cast<uint8_t>(buf[0] & '\x7f');
    sz <<= 8;
    sz |= static_cast<uint8_t>(buf[1]);
    sz <<= 8;
    sz |= static_cast<uint8_t>(buf[2]);
    sz <<= 8;
    sz |= static_cast<uint8_t>(buf[3]);
    return sz;
}

// OutputFileStream

OutputFileStream::OutputFileStream(asio::io_context& ctx, bool fsyncOnClose)
    : mFsyncOnClose(fsyncOnClose)
#ifndef WIN32
    , mBufferedWriteStream(ctx, stellar::fs::bufsz())
#endif
{
}

OutputFileStream::~OutputFileStream()
{
    if (isOpen())
    {
        close();
    }
}

bool
OutputFileStream::isOpen()
{
#ifdef WIN32
    return mOut != nullptr;
#else
    return mBufferedWriteStream.next_layer().is_open();
#endif
}

fs::native_handle_t
OutputFileStream::getHandle()
{
#ifdef WIN32
    return mHandle;
#else
    return mBufferedWriteStream.next_layer().native_handle();
#endif
}

void
OutputFileStream::close()
{
    ZoneScoped;
    if (!isOpen())
    {
        FileSystemException::failWith(
            "XDROutputFileStream::close() on non-open FILE*");
    }
    flush();
    if (mFsyncOnClose)
    {
        fs::flushFileChanges(getHandle());
    }
#ifdef WIN32
    fclose(mOut);
    mOut = nullptr;
#else
    mBufferedWriteStream.close();
#endif
}

void
OutputFileStream::fdopen(int fd)
{
#ifdef WIN32
    FileSystemException::failWith(
        "XDROutputFileStream::fdopen() not supported on windows");
#else
    if (isOpen())
    {
        FileSystemException::failWith(
            "XDROutputFileStream::fdopen() on already-open stream");
    }
    mBufferedWriteStream.next_layer().assign(fd);
    if (!isOpen())
    {
        FileSystemException::failWith("XDROutputFileStream::fdopen() failed");
    }
#endif
}

void
OutputFileStream::flush()
{
    ZoneScoped;
    if (!isOpen())
    {
        FileSystemException::failWith(
            "XDROutputFileStream::flush() on non-open stream");
    }
#ifdef WIN32
    fflush(mOut);
#else
    asio::error_code ec;
    do
    {
        mBufferedWriteStream.flush(ec);
        if (ec && ec != asio::error::interrupted)
        {
            FileSystemException::failWith(
                std::string("XDROutputFileStream::flush() failed: ") +
                ec.message());
        }
    } while (ec);
#endif
}

void
OutputFileStream::open(std::string const& filename)
{
    ZoneScoped;
    if (isOpen())
    {
        FileSystemException::failWith(
            "XDROutputFileStream::open() on already-open stream");
    }
    fs::native_handle_t handle = fs::openFileToWrite(filename);
#ifdef WIN32
    mOut = fs::fdOpen(handle);
    if (mOut != NULL)
    {
        mHandle = handle;
    }
    else
    {
        FileSystemException::failWith(
            "XDROutputFileStream::open() could not open file");
    }
#else
    mBufferedWriteStream.next_layer().assign(handle);
#endif
}

OutputFileStream::
operator bool()
{
    return isOpen();
}

void
OutputFileStream::writeBytes(char const* buf, size_t const sizeBytes)
{
    ZoneScoped;
    if (!isOpen())
    {
        FileSystemException::failWith(
            "OutputFileStream::writeBytes() on non-open stream");
    }

    size_t written = 0;
    while (written < sizeBytes)
    {
#ifdef WIN32
        auto w = fwrite(buf + written, 1, sizeBytes - written, mOut);
        if (w == 0)
        {
            FileSystemException::failWith(
                std::string("XDROutputFileStream::writeOne() failed"));
        }
        written += w;
#else
        asio::error_code ec;
        auto asioBuf = asio::buffer(buf + written, sizeBytes - written);
        written += asio::write(mBufferedWriteStream, asioBuf, ec);
        if (ec)
        {
            if (ec == asio::error::interrupted)
            {
                continue;
            }
            else
            {
                FileSystemException::failWith(
                    std::string("XDROutputFileStream::writeOne() failed: ") +
                    ec.message());
            }
        }
#endif
    }
}
}
