// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryArchive.h"
#include "util/make_unique.h"
#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include "lib/util/format.h"

#include <iostream>
#include <fstream>

namespace stellar
{

class HistoryArchive::Impl
{
public:
    std::string mName;
    std::string mGetCmd;
    std::string mPutCmd;
    Impl(std::string const& name,
         std::string const& getCmd,
         std::string const& putCmd)
        : mName(name)
        , mGetCmd(getCmd)
        , mPutCmd(putCmd)
        {}
};

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
    : mImpl(make_unique<Impl>(name, getCmd, putCmd))
{
}

HistoryArchive::~HistoryArchive()
{
}

HistoryArchiveParams
HistoryArchive::fetchParams()
{
    return HistoryArchiveParams();
}

std::string
HistoryArchive::getFileCmd(std::string const& basename, std::string const& filename)
{
    if (mImpl->mGetCmd.empty())
        return "";
    return fmt::format(mImpl->mGetCmd, basename, filename);
}

std::string
HistoryArchive::putFileCmd(std::string const& filename, std::string const& basename)
{
    if (mImpl->mPutCmd.empty())
        return "";
    return fmt::format(mImpl->mPutCmd, filename, basename);
}


}
