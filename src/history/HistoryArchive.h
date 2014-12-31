#ifndef __HISTORYARCHIVE__
#define __HISTORYARCHIVE__

#include <cereal/cereal.hpp>
#include <string>

namespace stellar
{

struct HistoryArchiveParams
{
    unsigned version{0};
    unsigned hotExponent{4};
    unsigned coldExponent{12};

    template <class Archive>
    void serialize(Archive& ar)
    {
        ar(CEREAL_NVP(version),
           CEREAL_NVP(hotExponent),
           CEREAL_NVP(coldExponent));
    }

    void save(std::string const& outFile);
    void load(std::string const& inFile);
};

class HistoryArchive
{
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;

public:
    HistoryArchive(std::string const& name,
                   std::string const& getCmd,
                   std::string const& putCmd);

    HistoryArchiveParams fetchParams();
};

}

#endif
