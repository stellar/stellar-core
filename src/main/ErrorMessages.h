#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace stellar
{

constexpr auto const REPORT_INTERNAL_BUG =
    "Please report this bug along with this log file if this was not expected";
constexpr auto const POSSIBLY_CORRUPTED_HISTORY =
    "One or more of history archives may be corrupted. Update HISTORY "
    "configuration entry to only contain valid ones";
constexpr auto const POSSIBLY_CORRUPTED_LOCAL_FS =
    "There may be a problem with the local filesystem. Ensure that there is "
    "enough space to perform that operation and that disc is behaving "
    "correctly.";
constexpr auto const POSSIBLY_CORRUPTED_LOCAL_DATA =
    "It is possible that your bucket storage or database is corrupted. Restore "
    "latest backup or reset this instance to fix this issue.";
constexpr auto const POSSIBLY_CORRUPTED_QUORUM_SET =
    "Check your QUORUM_SET for corrupted nodes";
constexpr auto const UPGRADE_STELLAR_CORE =
    "Upgrade this stellar-core installation to newest version";
}
