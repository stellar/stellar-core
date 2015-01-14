#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <string>

namespace stellar
{
class TempDir
{
    class Impl;
    std::unique_ptr<Impl> mImpl;
public:
    TempDir(std::string prefix);
    ~TempDir();
    std::string const& getName() const;
};

}
