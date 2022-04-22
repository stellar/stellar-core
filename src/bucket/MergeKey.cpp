// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/MergeKey.h"
#include "bucket/HashID.h"
#include "crypto/Hex.h"
#include "fmt/format.h"
#include <sstream>

namespace stellar
{

MergeKey::MergeKey(bool keepDeadEntries,
                   std::shared_ptr<Bucket> const& inputCurr,
                   std::shared_ptr<Bucket> const& inputSnap,
                   std::vector<std::shared_ptr<Bucket>> const& inputShadows)
    : mKeepDeadEntries(keepDeadEntries)
    , mInputCurrBucket(inputCurr->getHashID())
    , mInputSnapBucket(inputSnap->getHashID())
{
    mInputShadowBuckets.reserve(inputShadows.size());
    for (auto const& s : inputShadows)
    {
        mInputShadowBuckets.emplace_back(s->getHashID());
    }
}

bool
MergeKey::operator==(MergeKey const& other) const
{
    return mKeepDeadEntries == other.mKeepDeadEntries &&
           mInputCurrBucket == other.mInputCurrBucket &&
           mInputSnapBucket == other.mInputSnapBucket &&
           mInputShadowBuckets == other.mInputShadowBuckets;
}

std::ostream&
operator<<(std::ostream& out, MergeKey const& b)
{
    out << "[curr=" << b.mInputCurrBucket.toLogString()
        << ", snap=" << b.mInputSnapBucket.toLogString() << ", shadows=[";
    bool first = true;
    for (auto const& s : b.mInputShadowBuckets)
    {
        if (!first)
        {
            out << ", ";
        }
        first = false;
        out << s.toLogString();
    }
    out << fmt::format(FMT_STRING("], keep={}]"), b.mKeepDeadEntries);
    return out;
}
}

namespace std
{
size_t
hash<stellar::MergeKey>::operator()(stellar::MergeKey const& key) const noexcept
{
    std::ostringstream oss;
    oss << key.mKeepDeadEntries << ',' << key.mInputCurrBucket.toHex() << ','
        << key.mInputSnapBucket.toHex();
    for (auto const& e : key.mInputShadowBuckets)
    {
        oss << e.toHex() << ',';
    }
    std::hash<std::string> h;
    return h(oss.str());
}
}
