#pragma once

#include <cereal/cereal.hpp>
#include <string>
#include <system_error>

namespace asio
{
typedef std::error_code error_code;
};

namespace stellar
{

class Application;

struct HistoryStateBucket
{
    std::string curr;
    std::string snap;

    template <class Archive>
    void serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(curr),
           CEREAL_NVP(snap));
    }

    template <class Archive>
    void serialize(Archive& ar)
    {
        ar(CEREAL_NVP(curr),
           CEREAL_NVP(snap));
    }
};

struct HistoryArchiveState
{
    unsigned version{0};
    unsigned currentLedger;
    std::vector<HistoryStateBucket> currentBuckets;

    HistoryArchiveState();

    static std::string basename();

    // Return vector of buckets to fetch/apply to turn 'other' into 'this'. Vector
    // is sorted from largest/highest-numbered bucket to smallest/lowest, and
    // with snap buckets occurring before curr buckets. Zero-buckets are omitted.
    std::vector<std::string> differingBuckets(HistoryArchiveState const& other) const;

    template <class Archive>
    void serialize(Archive& ar)
    {
        ar(CEREAL_NVP(version),
           CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

    template <class Archive>
    void serialize(Archive& ar) const
    {
        ar(CEREAL_NVP(version),
           CEREAL_NVP(currentLedger),
           CEREAL_NVP(currentBuckets));
    }

    void save(std::string const& outFile) const;
    void load(std::string const& inFile);
};

class HistoryArchive
{
    class Impl;
    std::unique_ptr<Impl> mImpl;

public:
    HistoryArchive(std::string const& name,
                   std::string const& getCmd,
                   std::string const& putCmd);
    ~HistoryArchive();
    bool hasGetCmd() const;
    bool hasPutCmd() const;
    std::string const& getName() const;
    std::string qualifiedFilename(Application& app,
                                  std::string const& basename) const;
    void getState(Application& app,
                  std::function<void(asio::error_code const&,
                                     HistoryArchiveState const&)> handler) const;
    void putState(Application& app,
                  HistoryArchiveState const& s,
                  std::function<void(asio::error_code const&)> handler) const;
    std::string getFileCmd(std::string const& basename, std::string const& filename) const;
    std::string putFileCmd(std::string const& filename, std::string const& basename) const;
};

}


