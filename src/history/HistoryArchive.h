#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/FutureBucket.h"
#include "xdr/Stellar-types.h"
#include <cereal/cereal.hpp>
#include <memory>
#include <string>
#include <system_error>

namespace asio
{
typedef std::error_code error_code;
};

namespace stellar
{

class Application;
class BucketList;
class Bucket;

struct HistoryStateBucket
{
    std::string curr;
    FutureBucket next;
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
    static unsigned const HISTORY_ARCHIVE_STATE_VERSION;

    unsigned version{HISTORY_ARCHIVE_STATE_VERSION};
    std::string server;
    uint32_t currentLedger{0};
    std::vector<HistoryStateBucket> currentBuckets;

    HistoryArchiveState();

    HistoryArchiveState(uint32_t ledgerSeq, BucketList const& buckets);

    static std::string baseName();
    static std::string wellKnownRemoteDir();
    static std::string wellKnownRemoteName();
    static std::string remoteDir(uint32_t snapshotNumber);
    static std::string remoteName(uint32_t snapshotNumber);
    static std::string localName(Application& app,
                                 std::string const& archiveName);

    // Return cumulative hash of the bucketlist for this archive state.
    Hash getBucketListHash();

    // Return vector of buckets to fetch/apply to turn 'other' into 'this'.
    // Vector is sorted from largest/highest-numbered bucket to smallest/lowest,
    // and with snap buckets occurring before curr buckets. Zero-buckets are
    // omitted.
    std::vector<std::string>
    differingBuckets(HistoryArchiveState const& other) const;

    // Return vector of all buckets referenced by this state.
    std::vector<std::string> allBuckets() const;

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(server), CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

    template <class Archive>
    void
    serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(server), CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

    // Return true if all the 'next' bucket-futures that can be resolved are
    // ready to be (instantaneously) resolved, or false if a merge is still
    // in progress on one or more of them.
    bool futuresAllReady() const;

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
};

class HistoryArchive : public std::enable_shared_from_this<HistoryArchive>
{
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;
    std::string mMkdirCmd;

  public:
    HistoryArchive(std::string const& name, std::string const& getCmd,
                   std::string const& putCmd, std::string const& mkdirCmd);
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
};
}
