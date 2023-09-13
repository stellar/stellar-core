// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/DebugMetaUtils.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "util/Fs.h"
#include <fmt/format.h>

namespace stellar
{
namespace metautils
{

std::filesystem::path
getMetaDebugDirPath(std::filesystem::path const& bucketDir)
{
    return bucketDir / META_DEBUG_DIRNAME;
}

std::filesystem::path
getMetaDebugFilePath(std::filesystem::path const& bucketDir, uint32_t seqNum)
{
    auto file =
        fmt::format(META_DEBUG_FILE_FMT_STR, seqNum, binToHex(randomBytes(8)));
    return getMetaDebugDirPath(bucketDir) / file;
}

std::filesystem::path
getLatestTxSetFilePath(std::filesystem::path const& bucketDir)
{
    auto dir = getMetaDebugDirPath(bucketDir);
    return dir / DEBUG_TX_SET_FILENAME;
}

std::vector<std::filesystem::path>
listMetaDebugFiles(std::filesystem::path const& bucketDir)
{
    auto dir = getMetaDebugDirPath(bucketDir);
    auto files = fs::findfiles(dir.string(), [](std::string const& file) {
        return std::regex_match(file, META_DEBUG_FILE_REGEX);
    });
    std::sort(files.begin(), files.end());
    return std::vector<std::filesystem::path>(files.begin(), files.end());
}

bool
isDebugSegmentBoundary(uint32_t ledgerSeq)
{
    return ledgerSeq % META_DEBUG_LEDGER_SEGMENT_SIZE == 0;
}

size_t
getNumberOfDebugFilesToKeep(uint32_t numLedgers)
{
    size_t segLen = META_DEBUG_LEDGER_SEGMENT_SIZE;
    // Always keep an extra older file in case the newest file just got rotated,
    // to preserve METADATA_DEBUG_LEDGERS at all times.
    return ((numLedgers + segLen - 1) / segLen) + 1;
}

std::regex
getDebugMetaRegexForLedger(uint32_t ledgerSeq)
{
    std::string const regexStr =
        fmt::format("meta-debug-{:08x}-[[:xdigit:]]+\\.xdr(\\.gz)?", ledgerSeq);
    return std::regex{regexStr};
}

}
}