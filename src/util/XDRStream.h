#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
#include "util/FileSystemException.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

#include <fstream>
#include <string>
#include <vector>

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
        mIn.close();
    }

    void
    open(std::string const& filename)
    {
        mIn.open(filename, std::ifstream::binary);
        if (!mIn)
        {
            std::string msg("failed to open XDR file: ");
            msg += filename;
            msg += ", reason: ";
            msg += std::to_string(errno);
            CLOG(ERROR, "Fs") << msg;
            throw FileSystemException(msg);
        }

        mSize = fs::size(mIn);
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

    size_t
    pos()
    {
        assert(!mIn.fail());

        return mIn.tellg();
    }

    template <typename T>
    bool
    readOne(T& out)
    {
        char szBuf[4];
        if (!mIn.read(szBuf, 4))
        {
            return false;
        }

        // Read 4 bytes of size, big-endian, with XDR 'continuation' bit cleared
        // (high bit of high byte).
        uint32_t sz = 0;
        sz |= static_cast<uint8_t>(szBuf[0] & '\x7f');
        sz <<= 8;
        sz |= static_cast<uint8_t>(szBuf[1]);
        sz <<= 8;
        sz |= static_cast<uint8_t>(szBuf[2]);
        sz <<= 8;
        sz |= static_cast<uint8_t>(szBuf[3]);

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
            throw xdr::xdr_runtime_error("malformed XDR file");
        }
        xdr::xdr_get g(mBuf.data(), mBuf.data() + sz);
        xdr::xdr_argpack_archive(g, out);
        return true;
    }
};

// XDROutputStream needs access to a file descriptor to do
// fsync, so we use cstdio here rather than fstreams.
class XDROutputFileStream
{
    FILE* mOut{nullptr};
    std::vector<char> mBuf;
    const bool mFsyncOnClose;

  public:
    XDROutputFileStream(bool fsyncOnClose) : mFsyncOnClose(fsyncOnClose)
    {
    }

    ~XDROutputFileStream()
    {
        if (mOut)
        {
            close();
        }
    }

    void
    close()
    {
        if (!mOut)
        {
            FileSystemException::failWith(
                "XDROutputFileStream::close() on non-open FILE*");
        }
        if (fflush(mOut) != 0)
        {
            FileSystemException::failWithErrno(
                "XDROutputFileStream::close() failed on fflush(): ");
        }
        if (mFsyncOnClose)
        {
            fs::flushFileChanges(mOut);
        }
        if (fclose(mOut) != 0)
        {
            FileSystemException::failWithErrno(
                "XDROutputFileStream::close() failed on fclose(): ");
        }
        mOut = nullptr;
    }

    void
    open(std::string const& filename)
    {
        if (mOut)
        {
            FileSystemException::failWith(
                "XDROutputFileStream::open() on already-open stream");
        }
        mOut = fopen(filename.c_str(), "wb");
        if (!mOut)
        {
            FileSystemException::failWithErrno(
                std::string("XDROutputFileStream::open(\"") + filename +
                "\") failed: ");
        }
    }

    operator bool() const
    {
        return (mOut && !static_cast<bool>(ferror(mOut)) &&
                !static_cast<bool>(feof(mOut)));
    }

    template <typename T>
    void
    writeOne(T const& t, SHA256* hasher = nullptr, size_t* bytesPut = nullptr)
    {
        if (!mOut)
        {
            FileSystemException::failWith(
                "XDROutputFileStream::writeOne() on non-open FILE*");
        }

        uint32_t sz = (uint32_t)xdr::xdr_size(t);
        assert(sz < 0x80000000);

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

        if (fwrite(mBuf.data(), 1, sz + 4, mOut) != sz + 4)
        {
            FileSystemException::failWithErrno(
                "XDROutputFileStream::writeOne() failed:");
        }

        if (hasher)
        {
            hasher->add(ByteSlice(mBuf.data(), sz + 4));
        }
        if (bytesPut)
        {
            *bytesPut += (sz + 4);
        }
    }
};
}
