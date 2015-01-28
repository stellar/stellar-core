#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <string>

namespace stellar
{
class Application;
class TmpDir
{
    class Impl;
    std::unique_ptr<Impl> mImpl;
public:
    TmpDir(Application& app, std::string const& prefix);
    TmpDir(TmpDir&&);
    ~TmpDir();
    std::string const& getName() const;

    static bool exists(std::string const& path);
    static void deltree(std::string const& path);
    static bool mkdir(std::string const& path);
};

class TmpDirMaster
{
    Application& mApp;
    void clean();
public:
    TmpDirMaster(Application& app);
    ~TmpDirMaster();
    TmpDir tmpDir(std::string const& prefix);
};

}
