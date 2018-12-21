#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/ByteSlice.h"
#include "crypto/SHA.h"
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
    XDRInputFileStream(unsigned int sizeLimit = 0) : mSizeLimit{sizeLimit}
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
            throw std::runtime_error(msg);
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

class XDROutputFileStream
{
    std::ofstream mOut;
    std::vector<char> mBuf;

  public:
    void
    close()
    {
        mOut.close();
    }

    void
    open(std::string const& filename)
    {
        mOut.open(filename, std::ofstream::binary | std::ofstream::trunc);
        if (!mOut)
        {
            std::string msg("failed to open XDR file: ");
            msg += filename;
            msg += ", reason: ";
            msg += std::to_string(errno);
            CLOG(FATAL, "Fs") << msg;
            throw std::runtime_error(msg);
        }
    }

    operator bool() const
    {
        return mOut.good();
    }

    template <typename T>
    bool
    writeOne(T const& t, SHA256* hasher = nullptr, size_t* bytesPut = nullptr)
    {
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

        if (!mOut.write(mBuf.data(), sz + 4))
        {
            return false;
        }
        if (hasher)
        {
            hasher->add(ByteSlice(mBuf.data(), sz + 4));
        }
        if (bytesPut)
        {
            *bytesPut += (sz + 4);
        }
        return true;
    }
};
}
