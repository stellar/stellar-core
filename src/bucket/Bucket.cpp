// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "medida/timer.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"
#include "xdrpp/message.h"
#include <cassert>
#include <future>

namespace stellar
{

RawBucket::RawBucket(std::string const& filename, Hash const& hash)
    : mFilename(filename), mHash(hash)
{
    assert(filename.empty() || fs::exists(filename));
    if (!filename.empty())
    {
        CLOG(TRACE, "Bucket")
            << "RawBucket::RawBucket() created, file exists : " << mFilename;
        mSize = fs::size(filename);
    }
}

RawBucket::RawBucket()
{
}

Hash const&
RawBucket::getHash() const
{
    return mHash;
}

std::string const&
RawBucket::getFilename() const
{
    return mFilename;
}

size_t
RawBucket::getSize() const
{
    return mSize;
}

bool
RawBucket::containsBucketIdentity(BucketEntry const& id) const
{
    BucketEntryIdCmp cmp;
    BucketInputIterator iter(shared_from_this());
    while (iter)
    {
        if (!(cmp(*iter, id) || cmp(id, *iter)))
        {
            return true;
        }
        ++iter;
    }
    return false;
}

std::pair<size_t, size_t>
RawBucket::countLiveAndDeadEntries() const
{
    size_t live = 0, dead = 0;
    BucketInputIterator iter(shared_from_this());
    while (iter)
    {
        if ((*iter).type() == LIVEENTRY)
        {
            ++live;
        }
        else
        {
            ++dead;
        }
        ++iter;
    }
    return std::make_pair(live, dead);
}

void
RawBucket::apply(Application& app) const
{
    BucketApplicator applicator(app, shared_from_this());
    while (applicator)
    {
        applicator.advance();
    }
}

std::vector<BucketEntry>
RawBucket::convertToBucketEntry(std::vector<LedgerEntry> const& liveEntries)
{
    std::vector<BucketEntry> live;
    live.reserve(liveEntries.size());
    for (auto const& e : liveEntries)
    {
        BucketEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        live.push_back(ce);
    }
    std::sort(live.begin(), live.end(), BucketEntryIdCmp());
    return live;
}

std::vector<BucketEntry>
RawBucket::convertToBucketEntry(std::vector<LedgerKey> const& deadEntries)
{
    std::vector<BucketEntry> dead;
    dead.reserve(deadEntries.size());
    for (auto const& e : deadEntries)
    {
        BucketEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        dead.push_back(ce);
    }
    std::sort(dead.begin(), dead.end(), BucketEntryIdCmp());
    return dead;
}
}
