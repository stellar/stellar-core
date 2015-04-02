#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <string>

namespace stellar
{
class TmpDir
{
    std::unique_ptr<std::string> mPath;

  public:
    TmpDir(std::string const& prefix);
    TmpDir(TmpDir&&);
    ~TmpDir();
    std::string const& getName() const;
};

class TmpDirManager
{
    std::string mRoot;
    void clean();

  public:
    TmpDirManager(std::string const& root);
    ~TmpDirManager();
    TmpDir tmpDir(std::string const& prefix);
};
}
