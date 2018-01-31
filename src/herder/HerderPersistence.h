#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <cstdint>
#include <memory>
#include <vector>

namespace soci
{
class session;
}

namespace stellar
{
class Application;
class Database;
class XDROutputFileStream;
struct SCPEnvelope;

class HerderPersistence
{
  public:
    static std::unique_ptr<HerderPersistence> create(Application& app);

    virtual ~HerderPersistence()
    {
    }

    virtual void saveSCPHistory(uint32_t seq,
                                std::vector<SCPEnvelope> const& envs) = 0;

    static size_t copySCPHistoryToStream(Database& db, soci::session& sess,
                                         uint32_t ledgerSeq,
                                         uint32_t ledgerCount,
                                         XDROutputFileStream& scpHistory);
    static void dropAll(Database& db);
    static void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                 uint32_t count);
};
}
