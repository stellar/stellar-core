#pragma once

#include <cereal/cereal.hpp>
#include <string>
#include <system_error>
#include <memory>
#include "generated/Stellar-types.h"

namespace asio
{
typedef std::error_code error_code;
};

namespace stellar
{

class Application;
class BucketList;

struct HistoryStateBucket
{
    std::string curr;
    std::string snap;

    template <class Archive>
    void
    serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(curr), CEREAL_NVP(snap));
    }

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(CEREAL_NVP(curr), CEREAL_NVP(snap));
    }
};

struct HistoryArchiveState
{
    unsigned version{0};
    uint32_t currentLedger{0};
    std::vector<HistoryStateBucket> currentBuckets;

    HistoryArchiveState();

    HistoryArchiveState(uint32_t ledgerSeq,
                        BucketList& buckets);

    static std::string baseName();
    static std::string wellKnownRemoteDir();
    static std::string wellKnownRemoteName();
    static std::string remoteDir(uint32_t snapshotNumber);
    static std::string remoteName(uint32_t snapshotNumber);
    static std::string localName(Application& app,
                                 std::string const& archiveName);


    // Return cumulative hash of the bucketlist (a.k.a. clfHash) for this
    // archive state.
    Hash getBucketListHash();

    // Return vector of buckets to fetch/apply to turn 'other' into 'this'.
    // Vector
    // is sorted from largest/highest-numbered bucket to smallest/lowest, and
    // with snap buckets occurring before curr buckets. Zero-buckets are
    // omitted.
    std::vector<std::string>
    differingBuckets(HistoryArchiveState const& other) const;

    template <class Archive>
    void
    serialize(Archive& ar)
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

    template <class Archive>
    void
    serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(version), CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

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
    std::string qualifiedFilename(Application& app,
                                  std::string const& basename) const;

    void getMostRecentState(
        Application& app,
        std::function<void(asio::error_code const&, HistoryArchiveState const&)>
            handler) const;

    void
    getSnapState(Application& app, uint32_t snap,
                 std::function<void(asio::error_code const&,
                                    HistoryArchiveState const&)> handler) const;

    void getStateFromPath(
        Application& app, std::string const& remoteName,
        std::function<void(asio::error_code const&, HistoryArchiveState const&)>
            handler) const;

    void putState(Application& app, HistoryArchiveState const& s,
                  std::function<void(asio::error_code const&)> handler) const;

    void
    putStateInDir(Application& app, HistoryArchiveState const& s,
                  std::string const& local, std::string const& remoteDir,
                  std::string const& remoteName,
                  std::function<void(asio::error_code const&)> handler) const;

    std::string getFileCmd(std::string const& remote,
                           std::string const& local) const;
    std::string putFileCmd(std::string const& local,
                           std::string const& remote) const;
    std::string mkdirCmd(std::string const& remoteDir) const;
};
}
