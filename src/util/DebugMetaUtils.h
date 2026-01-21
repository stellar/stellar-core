// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <filesystem>
#include <regex>

namespace stellar
{

namespace metautils
{

std::string const META_DEBUG_DIRNAME{"meta-debug"};
std::string const DEBUG_TX_SET_FILENAME{"debug-tx-set.xdr"};
constexpr char META_DEBUG_FILE_FMT_STR[]{"meta-debug-{:08x}-{}.xdr"};
std::regex const META_DEBUG_FILE_REGEX{
    "meta-debug-[[:xdigit:]]+-[[:xdigit:]]+\\.xdr(\\.gz)?"};
std::regex const META_DEBUG_ZIP_FILE_REGEX{
    "meta-debug-[[:xdigit:]]+-[[:xdigit:]]+\\.xdr\\.gz?"};

// This number can be changed in the future without any coordination,
// it just controls the granularity of new meta-debug XDR segments.
//
// 256 ledgers == ~21 minutes. At time of writing, ~5mb meta / minute
// gives ~105mb meta / segment, which should compress to ~20mb.
uint32_t const META_DEBUG_LEDGER_SEGMENT_SIZE = 256;

std::filesystem::path
getMetaDebugFilePath(std::filesystem::path const& bucketDir, uint32_t seqNum);

std::filesystem::path
getLatestTxSetFilePath(std::filesystem::path const& bucketDir);

std::filesystem::path
getMetaDebugDirPath(std::filesystem::path const& bucketDir);

std::vector<std::filesystem::path>
listMetaDebugFiles(std::filesystem::path const& bucketDir);

bool isDebugSegmentBoundary(uint32_t ledgerSeq);

size_t getNumberOfDebugFilesToKeep(uint32_t ledgersToKeep);

std::regex getDebugMetaRegexForLedger(uint32_t ledgerSeq);
}
}
