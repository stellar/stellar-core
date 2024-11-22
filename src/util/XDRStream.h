#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
#include "util/FileSystemException.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/types.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#endif

namespace stellar
{

/**
 * Helper for loading a sequence of XDR objects from a file one at a time,
 * rather than all at once.
 */
class XDRInputFileStream
{
    std::ifstream mIn;
    std::vector<char> mBuf;
    size_t mSizeLimit;
    size_t mSize;

  public:
    XDRInputFileStream(unsigned int sizeLimit = 0)
        : mSizeLimit{sizeLimit}, mSize{0}
    {
    }

    void
    close()
    {
        ZoneScoped;
        mIn.close();
    }

    void
    open(std::string const& filename)
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
    open(std::filesystem::path const& filename)
    {
        open(filename.string());
    }

    operator bool() const
    {
        return mIn.good();
    }

    size_t
    size() const
    {
        return mSize;
    }

    std::streamoff
    pos()
    {
        releaseAssertOrThrow(!mIn.fail());
        return mIn.tellg();
    }

    void
    seek(size_t pos)
    {
        releaseAssertOrThrow(!mIn.fail());
        mIn.seekg(pos);
    }

    static inline uint32_t
    getXDRSize(char* buf)
    {
        // Read 4 bytes of size, big-endian, with XDR 'continuation' bit cleared
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

    template <typename T>
    bool
    readOne(T& out)
    {
        ZoneScoped;
        char szBuf[4];
        if (!mIn.read(szBuf, 4))
        {
            // checks that there was no trailing data
            if (mIn.eof() && mIn.gcount() == 0)
            {
                mIn.clear(std::ios_base::eofbit);
                return false;
            }
            else
            {
                throw xdr::xdr_runtime_error("IO failure in readOne");
            }
        }

        auto sz = getXDRSize(szBuf);
        if (mSizeLimit != 0 && sz > mSizeLimit)
        {
            return false;
        }
        if (sz > mBuf.size())
        {
            mBuf.resize(sz);
        }
        if (!mIn.read(mBuf.data(), sz))
        {
            throw xdr::xdr_runtime_error(
                "malformed XDR file or IO failure in readOne");
        }

        xdr::xdr_get g(mBuf.data(), mBuf.data() + sz);
        xdr::xdr_argpack_archive(g, out);
        return true;
    }

    // `readPage` reads records of XDR type `T` from the stream into output
    // variable `out`, until it has exceeded `pageSize` bytes or until it finds
    // an `out` value for which `getBucketLedgerKey(out) == key`. It returns
    // `true` if it located such a value, or false if it failed to find one.
    template <typename T>
    bool
    readPage(T& out, LedgerKey const& key, size_t pageSize)
    {
        ZoneScoped;
        if (mBuf.size() != pageSize)
        {
            mBuf.resize(pageSize);
        }

        if (!mIn.read(mBuf.data(), pageSize))
        {
            if (mIn.eof())
            {
                // Hitting eof just means there's not a full pageSize
                // worth of data left in the file. Not a problem: resize our
                // buffer down to the amount the page we _did_ manage to read.
                mBuf.resize(mIn.gcount());
                mIn.clear(std::ios_base::eofbit);
            }
            else
            {
                throw xdr::xdr_runtime_error("IO failure in readPage");
            }
        }

        size_t xdrStart = 0;
        while (xdrStart + 4 <= mBuf.size())
        {
            const uint32_t xdrSz = getXDRSize(mBuf.data() + xdrStart);
            xdrStart += 4;
            const size_t xdrEnd = xdrStart + xdrSz;

            // If entry continues past end of buffer, temporarily expand
            // it and load the extra bytes. The buffer will be resized
            // back to pageSize in the next readPage.
            if (xdrEnd > mBuf.size())
            {
                const size_t extraStart = mBuf.size();
                const size_t extraSz = xdrEnd - extraStart;
                mBuf.resize(xdrEnd);
                releaseAssert(extraStart + extraSz == mBuf.size());
                if (!mIn.read(mBuf.data() + extraStart, extraSz))
                {
                    throw xdr::xdr_runtime_error(
                        "malformed XDR file or IO failure in readPage");
                }
            }

            ZoneNamedN(__unpack, "xdr_unpack_entry", true);
            releaseAssert(xdrStart <= xdrEnd);
            releaseAssert(xdrEnd <= mBuf.size());
            xdr::xdr_get g(mBuf.data() + xdrStart, mBuf.data() + xdrEnd);
            xdr::xdr_argpack_archive(g, out);
            if (getBucketLedgerKey(out) == key)
            {
                return true;
            }

            xdrStart = xdrEnd;
        }

        return false;
    }
};

/*
IMPORTANT: some areas of core require durable writes that
are resistant to application and system crashes. If you need durable writes:
1. Use a stream implementation that supports fsync, e.g. OutputFileStream
2. Write to a temp file first. If you don't intent to persist temp files across
runs, fsyncing on close is sufficient. Otherwise, use durableWriteOne to flush
and fsync after every write.
3. Close the temp stream to make sure flush and fsync are called.
4. Rename the temp file to the final location using durableRename.
*/
class OutputFileStream
{
  protected:
    std::vector<char> mBuf;
    const bool mFsyncOnClose;

#ifdef WIN32
    // Windows implementation assumes calls can't get interrupted
    fs::native_handle_t mHandle;
    FILE* mOut{nullptr};
#else
    // use buffered stream which supports accessing a file descriptor needed to
    // fsync
    asio::buffered_write_stream<stellar::fs::stream_t> mBufferedWriteStream;
#endif

  public:
    OutputFileStream(asio::io_context& ctx, bool fsyncOnClose)
        : mFsyncOnClose(fsyncOnClose)
#ifndef WIN32
        , mBufferedWriteStream(ctx, stellar::fs::bufsz())
#endif
    {
    }

    ~OutputFileStream()
    {
        if (isOpen())
        {
            close();
        }
    }

    bool
    isOpen()
    {
#ifdef WIN32
        return mOut != nullptr;
#else
        return mBufferedWriteStream.next_layer().is_open();
#endif
    }

    fs::native_handle_t
    getHandle()
    {
#ifdef WIN32
        return mHandle;
#else
        return mBufferedWriteStream.next_layer().native_handle();
#endif
    }

    void
    close()
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
    fdopen(int fd)
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
            FileSystemException::failWith(
                "XDROutputFileStream::fdopen() failed");
        }
#endif
    }

    void
    flush()
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
    open(std::string const& filename)
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

    operator bool()
    {
        return isOpen();
    }

    void
    writeBytes(char const* buf, size_t const sizeBytes)
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
                        std::string(
                            "XDROutputFileStream::writeOne() failed: ") +
                        ec.message());
                }
            }
#endif
        }
    }
};

class XDROutputFileStream : public OutputFileStream
{
  public:
    XDROutputFileStream(asio::io_context& ctx, bool fsyncOnClose)
        : OutputFileStream(ctx, fsyncOnClose)
    {
    }

    template <typename T>
    void
    durableWriteOne(T const& t, SHA256* hasher = nullptr,
                    size_t* bytesPut = nullptr)
    {
        writeOne(t, hasher, bytesPut);
        flush();
        fs::flushFileChanges(getHandle());
    }

    template <typename T>
    void
    writeOne(T const& t, SHA256* hasher = nullptr, size_t* bytesPut = nullptr)
    {
        ZoneScoped;
        uint32_t sz = (uint32_t)xdr::xdr_size(t);
        releaseAssertOrThrow(sz < 0x80000000);

        if (mBuf.size() < sz + 4)
        {
            mBuf.resize(sz + 4);
        }

        // Write 4 bytes of size, big-endian, with XDR 'continuation' bit set on
        // high bit of high byte.
        mBuf[0] = static_cast<char>((sz >> 24) & 0xFF) | '\x80';
        mBuf[1] = static_cast<char>((sz >> 16) & 0xFF);
        mBuf[2] = static_cast<char>((sz >> 8) & 0xFF);
        mBuf[3] = static_cast<char>(sz & 0xFF);
        xdr::xdr_put p(mBuf.data() + 4, mBuf.data() + 4 + sz);
        xdr_argpack_archive(p, t);

        // Buffer is 4 bytes of encoded size, followed by encoded object
        size_t const toWrite = sz + 4;
        writeBytes(mBuf.data(), toWrite);

        if (hasher)
        {
            hasher->add(ByteSlice(mBuf.data(), toWrite));
        }
        if (bytesPut)
        {
            *bytesPut += (sz + 4);
        }
    }
};
}
