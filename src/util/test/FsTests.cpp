// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Random.h"
#include "history/FileTransferInfo.h"
#include "lib/catch.hpp"
#include "util/FileSystemException.h"
#include "util/Fs.h"
#include "util/TmpDir.h"

using namespace stellar;
namespace stdfs = std::filesystem;
namespace fs = stellar::fs;

TEST_CASE("filesystem locks", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path lockfile = root / "file.lock";
    fs::lockFile(lockfile.string());
    REQUIRE(fs::exists(lockfile.string()));
    REQUIRE_THROWS_AS(fs::lockFile(lockfile.string()), std::runtime_error);
    fs::unlockFile(lockfile.string());
    REQUIRE_THROWS_AS(fs::unlockFile(lockfile.string()), std::runtime_error);
    fs::lockFile(lockfile.string());
    fs::unlockFile(lockfile.string());
    stdfs::remove(lockfile);
}

TEST_CASE("filesystem durable rename", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path fileA = root / "fileA.txt";
    stdfs::path fileB = root / "fileB.txt";
    {
        std::ofstream out(fileA.string());
        out << "hi";
    }
    REQUIRE(fs::durableRename(fileA.string(), fileB.string(), root.string()));
    REQUIRE(!fs::exists(fileA.string()));
    REQUIRE(fs::exists(fileB.string()));
}

TEST_CASE("filesystem findfiles", "[fs]")
{
    TmpDir tmp("fstests");
    stdfs::path root(tmp.getName());
    stdfs::path textFile = root / "file.txt";
    stdfs::path docFile = root / "file.doc";
    {
        std::ofstream out(textFile.string());
        out << "hi";
    }
    stdfs::copy_file(textFile, docFile);
    REQUIRE(stdfs::exists(textFile));
    REQUIRE(stdfs::exists(docFile));
    auto files =
        fs::findfiles(root.string(), [](std::string const& name) -> bool {
            return stdfs::path(name).extension().string() == ".doc";
        });
    REQUIRE(files == std::vector<std::string>{docFile.filename().string()});
}

TEST_CASE("filesystem remoteName", "[fs]")
{
    REQUIRE(fs::remoteName(typeString(FileType::HISTORY_FILE_TYPE_LEDGER),
                           fs::hexStr(0x0abbccdd), "xdr.gz") ==
            "ledger/0a/bb/cc/ledger-0abbccdd.xdr.gz");
}
