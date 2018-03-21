#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifndef _WIN32

#include <string>

#include <spawn.h>
#include <sys/wait.h>

namespace stellar
{

class PosixSpawnFileActions
{
  public:
    PosixSpawnFileActions() = default;
    ~PosixSpawnFileActions();

    void addOpen(int fildes, std::string const& fileName, int oflag,
                 mode_t mode);

    operator posix_spawn_file_actions_t*();

  private:
    posix_spawn_file_actions_t mFileActions;
    bool mInitialized{false};

    void initialize();
};
}

#endif
