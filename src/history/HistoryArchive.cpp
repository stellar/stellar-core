// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "history/HistoryArchive.h"
#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "main/StellarCoreVersion.h"
#include "process/ProcessManager.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <set>
#include <sstream>

namespace stellar
{

unsigned const HistoryArchiveState::HISTORY_ARCHIVE_STATE_VERSION = 1;

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
    return true;
}

bool
HistoryArchiveState::futuresAllClear() const
{
    return std::all_of(
        currentBuckets.begin(), currentBuckets.end(),
        [](HistoryStateBucket const& bl) { return bl.next.isClear(); });
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
}

void
HistoryArchiveState::resolveAnyReadyFutures()
{
    ZoneScoped;
    for (auto& level : currentBuckets)
    {
        if (level.next.isMerging() && level.next.mergeComplete())
        {
            level.next.resolve();
        }
    }
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
        throw std::runtime_error(fmt::format("Error opening file {}", inFile));
    }
    in.exceptions(std::ios::badbit);
    cereal::JSONInputArchive ar(in);
    serialize(ar);
    if (version != HISTORY_ARCHIVE_STATE_VERSION)
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
HistoryArchiveState::localName(Application& app, std::string const& archiveName)
{
    return app.getHistoryManager().localFilename(archiveName + "-" +
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

    SHA256 totalHash;
    for (auto const& level : currentBuckets)
    {
        SHA256 levelHash;
        levelHash.add(hexToBin(level.curr));
        levelHash.add(hexToBin(level.snap));
        totalHash.add(levelHash.finish());
    }
    return totalHash.finish();
}

std::vector<std::string>
HistoryArchiveState::differingBuckets(HistoryArchiveState const& other) const
{
    ZoneScoped;
    assert(futuresAllResolved());
    std::set<std::string> inhibit;
    uint256 zero;
    inhibit.insert(binToHex(zero));
    for (auto b : other.currentBuckets)
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
    std::vector<std::string> ret;
    for (size_t i = BucketList::kNumLevels; i != 0; --i)
    {
        auto s = currentBuckets[i - 1].snap;
        auto n = s;
        if (currentBuckets[i - 1].next.hasOutputHash())
        {
            n = currentBuckets[i - 1].next.getOutputHash();
        }
        auto c = currentBuckets[i - 1].curr;
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
}

std::vector<std::string>
HistoryArchiveState::allBuckets() const
{
    ZoneScoped;
    std::set<std::string> buckets;
    for (auto const& level : currentBuckets)
    {
        buckets.insert(level.curr);
        buckets.insert(level.snap);
        auto nh = level.next.getHashes();
        buckets.insert(nh.begin(), nh.end());
    }
    return std::vector<std::string>(buckets.begin(), buckets.end());
}

bool
HistoryArchiveState::containsValidBuckets(Application& app) const
{
    ZoneScoped;
    // This function assumes presence of required buckets to verify state
    uint32_t minBucketVersion = 0;
    bool nonEmptySeen = false;
    Hash const emptyHash;

    auto validateBucketVersion = [&](uint32_t bucketVersion) {
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

    // Process bucket, return version
    auto processBucket = [&](std::string const& bucketHash) {
        auto bucket =
            app.getBucketManager().getBucketByHash(hexToBin256(bucketHash));
        releaseAssert(bucket);
        int32_t version = 0;
        if (bucket->getHash() != emptyHash)
        {
            version = Bucket::getBucketVersion(bucket);
            if (!nonEmptySeen)
            {
                nonEmptySeen = true;
            }
        }
        return version;
    };

    // Iterate bottom-up, from oldest to newest buckets
    for (uint32_t i = BucketList::kNumLevels - 1; i >= 0; i--)
    {
        auto const& level = currentBuckets[i];

        // Note: snap is always older than curr, and therefore must be processed
        // first
        if (!validateBucketVersion(processBucket(level.snap)) ||
            !validateBucketVersion(processBucket(level.curr)))
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
        auto const& prev = currentBuckets[i - 1];
        uint32_t prevSnapVersion = processBucket(prev.snap);

        if (!nonEmptySeen)
        {
            // No real buckets seen yet, move on
            continue;
        }
        else if (prevSnapVersion >= Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
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

void
HistoryArchiveState::prepareForPublish(Application& app)
{
    ZoneScoped;
    // Level 0 future buckets are always clear
    assert(currentBuckets[0].next.isClear());

    for (uint32_t i = 1; i < BucketList::kNumLevels; i++)
    {
        auto& level = currentBuckets[i];
        auto& prev = currentBuckets[i - 1];

        auto snap =
            app.getBucketManager().getBucketByHash(hexToBin256(prev.snap));
        if (!level.next.isClear() && Bucket::getBucketVersion(snap) >=
                                         Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
        {
            level.next.clear();
        }
        else if (level.next.hasHashes() && !level.next.isLive())
        {
            // Note: this `maxProtocolVersion` is over-approximate. The actual
            // max for the ledger being published might be lower, but if the
            // "true" (lower) max-value were actually in conflict with the state
            // we're about to publish it should have caused an error earlier
            // anyways, back when the bucket list and HAS for this state was
            // initially formed. Since we're just reconstituting a HAS here, we
            // assume it was legit when formed. Given that getting the true
            // value here therefore doesn't seem to add much checking, and given
            // that it'd be somewhat convoluted _to_ materialize the true value
            // here, we're going to live with the approximate value for now.
            uint32_t maxProtocolVersion =
                Config::CURRENT_LEDGER_PROTOCOL_VERSION;
            level.next.makeLive(app, maxProtocolVersion, i);
        }
    }
}

HistoryArchiveState::HistoryArchiveState() : server(STELLAR_CORE_VERSION)
{
    uint256 u;
    std::string s = binToHex(u);
    HistoryStateBucket b;
    b.curr = s;
    b.snap = s;
    while (currentBuckets.size() < BucketList::kNumLevels)
    {
        currentBuckets.push_back(b);
    }
}

HistoryArchiveState::HistoryArchiveState(uint32_t ledgerSeq,
                                         BucketList const& buckets,
                                         std::string const& passphrase)
    : server(STELLAR_CORE_VERSION)
    , networkPassphrase(passphrase)
    , currentLedger(ledgerSeq)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        HistoryStateBucket b;
        auto& level = buckets.getLevel(i);
        b.curr = binToHex(level.getCurr()->getHash());
        b.next = level.getNext();
        b.snap = binToHex(level.getSnap()->getHash());
        currentBuckets.push_back(b);
    }
}

HistoryArchive::HistoryArchive(Application& app,
                               HistoryArchiveConfiguration const& config)
    : mConfig(config)
    , mSuccessMeter(app.getMetrics().NewMeter(
          {"history-archive", config.mName, "success"}, "event"))
    , mFailureMeter(app.getMetrics().NewMeter(
          {"history-archive", config.mName, "failure"}, "event"))
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

void
HistoryArchive::markSuccess()
{
    mSuccessMeter.Mark();
}

void
HistoryArchive::markFailure()
{
    mFailureMeter.Mark();
}

uint64_t
HistoryArchive::getSuccessCount() const
{
    return mSuccessMeter.count();
}

uint64_t
HistoryArchive::getFailureCount() const
{
    return mFailureMeter.count();
}
}
