// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifndef _WIN32

#include "process/PosixSpawnFileActions.h"
#include "util/Logging.h"

#include <cassert>
#include <errno.h>

namespace stellar
{

PosixSpawnFileActions::~PosixSpawnFileActions()
{
    if (mInitialized)
    {
        if (auto err = posix_spawn_file_actions_destroy(&mFileActions))
        {
            CLOG(ERROR, "Process")
                << "posix_spawn_file_actions_destroy() failed: "
                << strerror(err);
        }
    }
}

void
PosixSpawnFileActions::initialize()
{
    if (mInitialized)
    {
        return;
    }

    if (auto err = posix_spawn_file_actions_init(&mFileActions))
    {
        CLOG(ERROR, "Process")
            << "posix_spawn_file_actions_init() failed: " << strerror(err);
        throw std::runtime_error("posix_spawn_file_actions_init() failed");
    }
    mInitialized = true;
}

void
PosixSpawnFileActions::addOpen(int fildes, std::string const& fileName,
                               int oflag, mode_t mode)
{
    assert(!fileName.empty());
    initialize();

    if (auto err = posix_spawn_file_actions_addopen(
            &mFileActions, fildes, fileName.c_str(), oflag, mode))
    {
        CLOG(ERROR, "Process")
            << "posix_spawn_file_actions_addopen() failed: " << strerror(err);
        throw std::runtime_error("posix_spawn_file_actions_addopen() failed");
    }
}

PosixSpawnFileActions::operator posix_spawn_file_actions_t*()
{
    return mInitialized ? &mFileActions : nullptr;
}
}

#endif
