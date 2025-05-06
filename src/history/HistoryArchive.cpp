// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "history/HistoryArchive.h"
#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "main/StellarCoreVersion.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <fstream>
#include <iostream>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <set>
#include <sstream>

namespace stellar
{

template <typename... Tokens>
std::string
formatString(std::string const& templateString, Tokens const&... tokens)
{
    try
    {
        return fmt::format(templateString, tokens...);
    }
    catch (fmt::format_error const& ex)
    {
        CLOG_ERROR(History, "Failed to format string \"{}\":{}", templateString,
                   ex.what());
        CLOG_ERROR(History, "Check your HISTORY entry in configuration file");
        throw std::runtime_error("failed to format command string");
    }
}

bool
HistoryArchiveState::futuresAllResolved() const
{
    ZoneScoped;
    for (auto const& level : currentBuckets)
    {
        if (level.next.isMerging())
        {
            return false;
        }
    }

    for (auto const& level : hotArchiveBuckets)
    {
        if (level.next.isMerging())
        {
            return false;
        }
    }
    return true;
}

bool
HistoryArchiveState::futuresAllClear() const
{
    return std::all_of(currentBuckets.begin(), currentBuckets.end(),
                       [](auto const& bl) { return bl.next.isClear(); }) &&
           std::all_of(hotArchiveBuckets.begin(), hotArchiveBuckets.end(),
                       [](auto const& bl) { return bl.next.isClear(); });
}

void
HistoryArchiveState::resolveAllFutures()
{
    ZoneScoped;
    for (auto& level : currentBuckets)
    {
        if (level.next.isMerging())
        {
            level.next.resolve();
        }
    }

    for (auto& level : hotArchiveBuckets)
    {
        if (level.next.isMerging())
        {
            level.next.resolve();
        }
    }
}

void
HistoryArchiveState::resolveAnyReadyFutures()
{
    ZoneScoped;
    auto resolveMerged = [](auto& buckets) {
        for (auto& level : buckets)
        {
            if (level.next.isMerging() && level.next.mergeComplete())
            {
                level.next.resolve();
            }
        }
    };

    resolveMerged(currentBuckets);
    resolveMerged(hotArchiveBuckets);
}

void
HistoryArchiveState::save(std::string const& outFile) const
{
    ZoneScoped;
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(outFile);
    cereal::JSONOutputArchive ar(out);
    serialize(ar);
}

std::string
HistoryArchiveState::toString() const
{
    ZoneScoped;
    // We serialize-to-a-string any HAS, regardless of resolvedness, as we are
    // usually doing this to write to the database on the main thread, just as a
    // durability step: we don't want to block.
    std::ostringstream out;
    {
        cereal::JSONOutputArchive ar(out);
        serialize(ar);
    }
    return out.str();
}

void
HistoryArchiveState::load(std::string const& inFile)
{
    ZoneScoped;
    std::ifstream in(inFile);
    if (!in)
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Error opening file {}"), inFile));
    }
    in.exceptions(std::ios::badbit);
    cereal::JSONInputArchive ar(in);
    serialize(ar);
    if (version != HISTORY_ARCHIVE_STATE_VERSION_BEFORE_HOT_ARCHIVE &&
        version != HISTORY_ARCHIVE_STATE_VERSION_WITH_HOT_ARCHIVE)
    {
        CLOG_ERROR(History, "Unexpected history archive state version: {}",
                   version);
        throw std::runtime_error("unexpected history archive state version");
    }
}

void
HistoryArchiveState::fromString(std::string const& str)
{
    ZoneScoped;
    std::istringstream in(str);
    cereal::JSONInputArchive ar(in);
    serialize(ar);
}

std::string
HistoryArchiveState::baseName()
{
    return std::string("stellar-history.json");
}

std::string
HistoryArchiveState::wellKnownRemoteDir()
{
    // The RFC 5785 dir
    return std::string(".well-known");
}

std::string
HistoryArchiveState::wellKnownRemoteName()
{
    return wellKnownRemoteDir() + "/" + baseName();
}

std::string
HistoryArchiveState::remoteDir(uint32_t snapshotNumber)
{
    return fs::remoteDir("history", fs::hexStr(snapshotNumber));
}

std::string
HistoryArchiveState::remoteName(uint32_t snapshotNumber)
{
    return fs::remoteName("history", fs::hexStr(snapshotNumber), "json");
}

std::string
HistoryArchiveState::localName(Application& app,
                               std::string const& uniquePrefix)
{
    return app.getHistoryManager().localFilename(uniquePrefix + "-" +
                                                 baseName());
}

Hash
HistoryArchiveState::getBucketListHash() const
{
    ZoneScoped;
    // NB: This hash algorithm has to match "what the BucketList does" to
    // calculate its BucketList hash exactly. It's not a particularly complex
    // algorithm -- just hash all the hashes of all the bucket levels, in order,
    // with each level-hash being the hash of its curr bucket then its snap
    // bucket -- but we duplicate the logic here because it honestly seems like
    // it'd be less readable to try to abstract the code between the two
    // relatively-different representations. Everything will explode if there is
    // any difference in these algorithms anyways, so..

    auto hashBuckets = [](auto const& buckets) {
        SHA256 hash;
        for (auto const& level : buckets)
        {
            SHA256 levelHash;
            levelHash.add(hexToBin(level.curr));
            levelHash.add(hexToBin(level.snap));
            hash.add(levelHash.finish());
        }

        return hash.finish();
    };

    if (hasHotArchiveBuckets())
    {
        SHA256 hash;
        hash.add(hashBuckets(currentBuckets));
        hash.add(hashBuckets(hotArchiveBuckets));
        return hash.finish();
    }

    return hashBuckets(currentBuckets);
}

HistoryArchiveState::BucketHashReturnT
HistoryArchiveState::differingBuckets(HistoryArchiveState const& other) const
{
    ZoneScoped;
    releaseAssert(futuresAllResolved());
    std::set<std::string> inhibit;
    uint256 zero;
    inhibit.insert(binToHex(zero));
    auto processBuckets = [&inhibit](auto const& buckets,
                                     auto const& otherBuckets) {
        std::vector<std::string> ret;
        for (auto b : otherBuckets)
        {
            inhibit.insert(b.curr);
            if (b.next.isLive())
            {
                b.next.resolve();
            }
            if (b.next.hasOutputHash())
            {
                inhibit.insert(b.next.getOutputHash());
            }
            inhibit.insert(b.snap);
        }

        for (size_t i = buckets.size(); i != 0; --i)
        {
            auto s = buckets[i - 1].snap;
            auto n = s;
            if (buckets[i - 1].next.hasOutputHash())
            {
                n = buckets[i - 1].next.getOutputHash();
            }
            auto c = buckets[i - 1].curr;
            auto bs = {s, n, c};
            for (auto const& j : bs)
            {
                if (inhibit.find(j) == inhibit.end())
                {
                    ret.push_back(j);
                    inhibit.insert(j);
                }
            }
        }
        return ret;
    };

    auto liveHashes = processBuckets(currentBuckets, other.currentBuckets);
    auto hotHashes = processBuckets(hotArchiveBuckets, other.hotArchiveBuckets);
    return BucketHashReturnT(std::move(liveHashes), std::move(hotHashes));
}

std::vector<std::string>
HistoryArchiveState::allBuckets() const
{
    ZoneScoped;
    std::set<std::string> buckets;
    auto processBuckets = [&buckets](auto const& bucketList) {
        for (auto const& level : bucketList)
        {
            buckets.insert(level.curr);
            buckets.insert(level.snap);
            auto nh = level.next.getHashes();
            buckets.insert(nh.begin(), nh.end());
        }
    };

    processBuckets(currentBuckets);
    processBuckets(hotArchiveBuckets);
    return std::vector<std::string>(buckets.begin(), buckets.end());
}

namespace
{

// Checks for structural validity of the given BucketList. This includes
// checking that the bucket list has the correct number of levels, versioning
// consistency, and future bucket state.
template <typename HistoryStateBucketT>
bool
validateBucketListHelper(Application& app,
                         std::vector<HistoryStateBucketT> const& buckets,
                         uint32_t expectedLevels)
{
    // Get Bucket version and set nonEmptySeen
    bool nonEmptySeen = false;
    auto getVersionAndCheckEmpty = [&nonEmptySeen](auto const& bucket) {
        int32_t version = 0;
        releaseAssert(bucket);
        if (!bucket->isEmpty())
        {
            version = bucket->getBucketVersion();
            if (!nonEmptySeen)
            {
                nonEmptySeen = true;
            }
        }
        return version;
    };

    uint32_t minBucketVersion = 0;
    auto validateBucketVersion = [&minBucketVersion](uint32_t bucketVersion) {
        if (bucketVersion < minBucketVersion)
        {
            CLOG_ERROR(History,
                       "Incompatible bucket versions: expected version "
                       "{} or higher, got {}",
                       minBucketVersion, bucketVersion);
            return false;
        }
        minBucketVersion = bucketVersion;
        return true;
    };

    using BucketT =
        typename std::decay_t<decltype(buckets)>::value_type::bucket_type;

    if (buckets.size() != expectedLevels)
    {
        CLOG_ERROR(History, "Invalid HAS: bucket list size mismatch");
        return false;
    }

    for (uint32_t j = expectedLevels; j != 0; --j)
    {
        auto i = j - 1;
        auto const& level = buckets[i];
        auto curr = app.getBucketManager().getBucketByHash<BucketT>(
            hexToBin256(level.curr));
        auto snap = app.getBucketManager().getBucketByHash<BucketT>(
            hexToBin256(level.snap));

        // Note: snap is always older than curr, and therefore must be
        // processed first
        if (!validateBucketVersion(getVersionAndCheckEmpty(snap)) ||
            !validateBucketVersion(getVersionAndCheckEmpty(curr)))
        {
            return false;
        }

        // Level 0 future buckets are always clear
        if (i == 0)
        {
            if (!level.next.isClear())
            {
                CLOG_ERROR(History,
                           "Invalid HAS: next must be clear at level 0");
                return false;
            }
            break;
        }

        // Validate "next" field
        // Use previous level snap to determine "next" validity
        auto const& prev = buckets[i - 1];
        auto prevSnap = app.getBucketManager().getBucketByHash<BucketT>(
            hexToBin256(prev.snap));
        uint32_t prevSnapVersion = getVersionAndCheckEmpty(prevSnap);

        if (!nonEmptySeen)
        {
            // We're iterating from the bottom up, so if we haven't seen a
            // non-empty bucket yet, we can skip the check because the
            // bucket is default initialized
            continue;
        }
        else if (protocolVersionStartsFrom(
                     prevSnapVersion,
                     LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
        {
            if (!level.next.isClear())
            {
                CLOG_ERROR(History, "Invalid HAS: future must be cleared ");
                return false;
            }
        }
        else if (!level.next.hasOutputHash())
        {
            CLOG_ERROR(History,
                       "Invalid HAS: future must have resolved output");
            return false;
        }
    }

    return true;
}
}

bool
HistoryArchiveState::containsValidBuckets(Application& app) const
{
    ZoneScoped;
    if (!validateBucketListHelper(app, currentBuckets,
                                  LiveBucketList::kNumLevels))
    {
        return false;
    }

    if (hasHotArchiveBuckets() &&
        !validateBucketListHelper(app, hotArchiveBuckets,
                                  HotArchiveBucketList::kNumLevels))
    {
        return false;
    }

    return true;
}

void
HistoryArchiveState::prepareForPublish(Application& app)
{
    ZoneScoped;
    auto prepareBucketList = [&](auto& buckets, size_t numLevels) {
        using BucketT =
            typename std::decay_t<decltype(buckets)>::value_type::bucket_type;

        // Level 0 future buckets are always clear
        releaseAssert(buckets[0].next.isClear());
        for (uint32_t i = 1; i < numLevels; i++)
        {
            auto& level = buckets[i];
            auto& prev = buckets[i - 1];

            auto snap = app.getBucketManager().getBucketByHash<BucketT>(
                hexToBin256(prev.snap));
            if (!level.next.isClear() &&
                protocolVersionStartsFrom(
                    snap->getBucketVersion(),
                    LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
            {
                level.next.clear();
            }
            else if (level.next.hasHashes() && !level.next.isLive())
            {
                // Note: this `maxProtocolVersion` is over-approximate. The
                // actual max for the ledger being published might be lower, but
                // if the "true" (lower) max-value were actually in conflict
                // with the state we're about to publish it should have caused
                // an error earlier anyways, back when the bucket list and HAS
                // for this state was initially formed. Since we're just
                // reconstituting a HAS here, we assume it was legit when
                // formed. Given that getting the true value here therefore
                // doesn't seem to add much checking, and given that it'd be
                // somewhat convoluted _to_ materialize the true value here,
                // we're going to live with the approximate value for now.
                uint32_t maxProtocolVersion =
                    app.getConfig().LEDGER_PROTOCOL_VERSION;
                level.next.makeLive(app, maxProtocolVersion, i);
            }
        }
    };

    prepareBucketList(currentBuckets, LiveBucketList::kNumLevels);
    if (hasHotArchiveBuckets())
    {
        prepareBucketList(hotArchiveBuckets, HotArchiveBucketList::kNumLevels);
    }
}

HistoryArchiveState::HistoryArchiveState() : server(STELLAR_CORE_VERSION)
{
    uint256 u;
    std::string s = binToHex(u);
    HistoryStateBucket<LiveBucket> b;
    b.curr = s;
    b.snap = s;
    while (currentBuckets.size() < LiveBucketList::kNumLevels)
    {
        currentBuckets.push_back(b);
    }
}

HistoryArchiveState::HistoryArchiveState(uint32_t ledgerSeq,
                                         LiveBucketList const& buckets,
                                         std::string const& passphrase)
    : server(STELLAR_CORE_VERSION)
    , networkPassphrase(passphrase)
    , currentLedger(ledgerSeq)
{
    for (uint32_t i = 0; i < LiveBucketList::kNumLevels; ++i)
    {
        HistoryStateBucket<LiveBucket> b;
        auto& level = buckets.getLevel(i);
        auto const& curr = level.getCurr();
        auto const& snap = level.getSnap();
        b.curr = binToHex(curr->getHash());
        b.next = level.getNext();
        b.snap = binToHex(snap->getHash());
        currentBuckets.push_back(b);

        auto checkBucketSize = [](auto const& bucket) {
            if (bucket->getSize() > MAX_HISTORY_ARCHIVE_BUCKET_SIZE)
            {
                CLOG_FATAL(
                    History,
                    "Bucket size ({}) is greater than the maximum allowed "
                    "size ({}) for Bucket {}. stellar-core must be upgraded "
                    "to a version supporting larger buckets or new nodes "
                    "will not be able to join the network!",
                    bucket->getSize(), MAX_HISTORY_ARCHIVE_BUCKET_SIZE,
                    binToHex(bucket->getHash()));
            }
        };

        checkBucketSize(curr);
        checkBucketSize(snap);
    }
}

HistoryArchiveState::HistoryArchiveState(uint32_t ledgerSeq,
                                         LiveBucketList const& liveBuckets,
                                         HotArchiveBucketList const& hotBuckets,
                                         std::string const& passphrase)
    : HistoryArchiveState(ledgerSeq, liveBuckets, passphrase)
{
    version = HISTORY_ARCHIVE_STATE_VERSION_WITH_HOT_ARCHIVE;
    for (uint32_t i = 0; i < HotArchiveBucketList::kNumLevels; ++i)
    {
        HistoryStateBucket<HotArchiveBucket> b;
        b.curr = binToHex(hotBuckets.getLevel(i).getCurr()->getHash());
        b.next = hotBuckets.getLevel(i).getNext();
        b.snap = binToHex(hotBuckets.getLevel(i).getSnap()->getHash());
        hotArchiveBuckets.push_back(b);
    }
}

HistoryArchive::HistoryArchive(HistoryArchiveConfiguration const& config)
    : mConfig(config)
{
}

HistoryArchive::~HistoryArchive()
{
}

bool
HistoryArchive::hasGetCmd() const
{
    return !mConfig.mGetCmd.empty();
}

bool
HistoryArchive::hasPutCmd() const
{
    return !mConfig.mPutCmd.empty();
}

bool
HistoryArchive::hasMkdirCmd() const
{
    return !mConfig.mMkdirCmd.empty();
}

std::string const&
HistoryArchive::getName() const
{
    return mConfig.mName;
}

std::string
HistoryArchive::getFileCmd(std::string const& remote,
                           std::string const& local) const
{
    if (mConfig.mGetCmd.empty())
        return "";
    return formatString(mConfig.mGetCmd, remote, local);
}

std::string
HistoryArchive::putFileCmd(std::string const& local,
                           std::string const& remote) const
{
    if (mConfig.mPutCmd.empty())
        return "";
    return formatString(mConfig.mPutCmd, local, remote);
}

std::string
HistoryArchive::mkdirCmd(std::string const& remoteDir) const
{
    if (mConfig.mMkdirCmd.empty())
        return "";
    return formatString(mConfig.mMkdirCmd, remoteDir);
}
}
