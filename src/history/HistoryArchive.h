#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "bucket/FutureBucket.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "main/Config.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-types.h"

#include <cereal/cereal.hpp>
#include <memory>
#include <string>
#include <system_error>

namespace asio
{
typedef std::error_code error_code;
}

namespace medida
{
class Meter;
}

namespace stellar
{

class Application;

template <class BucketT> struct HistoryStateBucket
{
    BUCKET_TYPE_ASSERT(BucketT);
    using bucket_type = BucketT;
    std::string curr;

    FutureBucket<BucketT> next;
    std::string snap;

    template <class Archive>
    void
    serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(curr), CEREAL_NVP(next), CEREAL_NVP(snap));
    }

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(CEREAL_NVP(curr), CEREAL_NVP(next), CEREAL_NVP(snap));
    }
};

/**
 * A snapshot of a ledger number and associated set of buckets; this is used
 * when writing to HistoryArchives as well as when persisting the state of the
 * BucketList to the local database, as PersistentState kHistoryArchiveState. It
 * might reasonably be renamed BucketListState or similar, since it really only
 * describes a BucketList, not an entire HistoryArchive.
 */
struct HistoryArchiveState
{
    // Maximum supported size of a bucket in the history archive. This is used
    // as a very basic DOS protection against downloading very large malicious
    // buckets. If a downloaded bucket is above this size, we automatically fail
    // it as invalid. Note that we will still publish Buckets over this size so
    // the network doesn't halt, but we will warn significantly, as at that
    // point no new nodes could assume state from lcl. This value is _very_
    // large given ledger state size as of Feb 2025, but we may want to revisit
    // in the future. Worst case if we forget about this, new nodes could still
    // assume state from an earlier ledger where Bucket sizes were not above
    // the limit and replay ledgers to join the network.
    static constexpr size_t MAX_HISTORY_ARCHIVE_BUCKET_SIZE =
        1024ull * 1024ull * 1024ull * 100ull; // 100 GB

    static inline unsigned const
        HISTORY_ARCHIVE_STATE_VERSION_BEFORE_HOT_ARCHIVE = 1;
    static inline unsigned const
        HISTORY_ARCHIVE_STATE_VERSION_WITH_HOT_ARCHIVE = 2;

    struct BucketHashReturnT
    {
        std::vector<std::string> live;
        std::vector<std::string> hot;

        explicit BucketHashReturnT(std::vector<std::string>&& live,
                                   std::vector<std::string>&& hot)
            : live(live), hot(hot)
        {
        }
    };

    unsigned version{HISTORY_ARCHIVE_STATE_VERSION_BEFORE_HOT_ARCHIVE};
    std::string server;
    std::string networkPassphrase;
    uint32_t currentLedger{0};
    std::vector<HistoryStateBucket<LiveBucket>> currentBuckets;
    std::vector<HistoryStateBucket<HotArchiveBucket>> hotArchiveBuckets;

    HistoryArchiveState();

    HistoryArchiveState(uint32_t ledgerSeq, LiveBucketList const& liveBuckets,
                        HotArchiveBucketList const& hotBuckets,
                        std::string const& networkPassphrase);

    HistoryArchiveState(uint32_t ledgerSeq, LiveBucketList const& liveBuckets,
                        std::string const& networkPassphrase);

    static std::string baseName();
    static std::string wellKnownRemoteDir();
    static std::string wellKnownRemoteName();
    static std::string remoteDir(uint32_t snapshotNumber);
    static std::string remoteName(uint32_t snapshotNumber);
    static std::string localName(Application& app,
                                 std::string const& archiveName);

    // Return cumulative hash of the bucketlist for this archive state.
    Hash getBucketListHash() const;

    // Return vector of buckets to fetch/apply to turn 'other' into 'this'.
    // Vector is sorted from largest/highest-numbered bucket to smallest/lowest,
    // and with snap buckets occurring before curr buckets. Zero-buckets are
    // omitted. Hashes are distinguished by live and Hot Archive buckets.
    BucketHashReturnT differingBuckets(HistoryArchiveState const& other) const;

    // Return vector of all buckets referenced by this state.
    std::vector<std::string> allBuckets() const;

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(server), CEREAL_NVP(currentLedger));
        try
        {
            ar(CEREAL_NVP(networkPassphrase));
        }
        catch (cereal::Exception& e)
        {
            // networkPassphrase wasn't parsed.
            // This is expected when the input file does not contain it, but
            // should only ever happen for older versions of History Archive
            // State that do not support HotArchive.
            if (hasHotArchiveBuckets())
            {
                throw e;
            }
        }
        ar(CEREAL_NVP(currentBuckets));
        if (hasHotArchiveBuckets())
        {
            ar(CEREAL_NVP(hotArchiveBuckets));
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(server), CEREAL_NVP(currentLedger));
        if (!networkPassphrase.empty())
        {
            ar(CEREAL_NVP(networkPassphrase));
        }
        else
        {
            // New versions of HistoryArchiveState (i.e. support HotArchive)
            // should always have a networkPassphrase.
            releaseAssertOrThrow(!hasHotArchiveBuckets());
        }
        ar(CEREAL_NVP(currentBuckets));
        if (hasHotArchiveBuckets())
        {
            ar(CEREAL_NVP(hotArchiveBuckets));
        }
    }

    // Return true if all futures are in FB_CLEAR state
    bool futuresAllClear() const;

    // Return true if all futures have already been resolved, otherwise false.
    bool futuresAllResolved() const;

    // Resolve all futures, filling in the 'next' bucket hashes of each level.
    // NB: this may block the calling thread, careful!
    void resolveAllFutures();

    // Resolve any futures that are ready, filling in the 'next' bucket hashes
    // of each resolved level.  NB: this will not block the calling thread, may
    // slightly improve the resolved-ness of the FutureBuckets such that a nicer
    // value is available for saving and reloading (fewer captured shadows).
    void resolveAnyReadyFutures();

    void save(std::string const& outFile) const;
    void load(std::string const& inFile);

    std::string toString() const;
    void fromString(std::string const& str);

    void prepareForPublish(Application& app);
    bool containsValidBuckets(Application& app) const;

    bool
    hasHotArchiveBuckets() const
    {
        return version >= HISTORY_ARCHIVE_STATE_VERSION_WITH_HOT_ARCHIVE;
    }
};

class HistoryArchive : public std::enable_shared_from_this<HistoryArchive>
{
  public:
    explicit HistoryArchive(HistoryArchiveConfiguration const& config);
    ~HistoryArchive();
    bool hasGetCmd() const;
    bool hasPutCmd() const;
    bool hasMkdirCmd() const;
    std::string const& getName() const;

    std::string getFileCmd(std::string const& remote,
                           std::string const& local) const;
    std::string putFileCmd(std::string const& local,
                           std::string const& remote) const;
    std::string mkdirCmd(std::string const& remoteDir) const;

  private:
    HistoryArchiveConfiguration mConfig;
};
}
