// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/MergeKey.h"
#include "crypto/Hex.h"
#include "fmt/format.h"
#include <sstream>

namespace stellar
{

MergeKey::MergeKey(bool keepTombstoneEntries, Hash const& currHash,
                   Hash const& snapHash, std::vector<Hash> const& shadowHashes)
    : mKeepTombstoneEntries(keepTombstoneEntries)
    , mInputCurrBucket(currHash)
    , mInputSnapBucket(snapHash)
    , mInputShadowBuckets(shadowHashes)
{
}

bool
MergeKey::operator==(MergeKey const& other) const
{
    return mKeepTombstoneEntries == other.mKeepTombstoneEntries &&
           mInputCurrBucket == other.mInputCurrBucket &&
           mInputSnapBucket == other.mInputSnapBucket &&
           mInputShadowBuckets == other.mInputShadowBuckets;
}

std::ostream&
operator<<(std::ostream& out, MergeKey const& b)
{
    out << "[curr=" << hexAbbrev(b.mInputCurrBucket)
        << ", snap=" << hexAbbrev(b.mInputSnapBucket) << ", shadows=[";
    bool first = true;
    for (auto const& s : b.mInputShadowBuckets)
    {
        if (!first)
        {
            out << ", ";
        }
        first = false;
        out << hexAbbrev(s);
    }
    out << fmt::format(FMT_STRING("], keep={}]"), b.mKeepTombstoneEntries);
    return out;
}

std::string
format_as(MergeKey const& k)
{
    std::stringstream ss;
    ss << k;
    return ss.str();
}
}

namespace std
{
size_t
hash<stellar::MergeKey>::operator()(stellar::MergeKey const& key) const noexcept
{
    std::ostringstream oss;
    oss << key.mKeepTombstoneEntries << ','
        << stellar::binToHex(key.mInputCurrBucket) << ','
        << stellar::binToHex(key.mInputSnapBucket);
    for (auto const& e : key.mInputShadowBuckets)
    {
        oss << stellar::binToHex(e) << ',';
    }
    std::hash<std::string> h;
    return h(oss.str());
}
}
