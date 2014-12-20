#include "history/HistoryArchive.h"

#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

#include <iostream>
#include <fstream>

namespace stellar
{

void
HistoryArchiveParams::save(std::string const& outFile)
{
    std::ofstream out(outFile);
    cereal::JSONOutputArchive ar(out);
    serialize(ar);
}

void
HistoryArchiveParams::load(std::string const& inFile)
{
    std::ifstream in(inFile);
    cereal::JSONInputArchive ar(in);
    serialize(ar);
}

HistoryArchive::HistoryArchive(std::string const& name,
                               std::string const& getCmd,
                               std::string const& putCmd)
    : mName(name)
    , mGetCmd(getCmd)
    , mPutCmd(putCmd)
{
}

HistoryArchiveParams
HistoryArchive::fetchParams()
{
    return HistoryArchiveParams();
}


}
