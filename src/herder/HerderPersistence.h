// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/QuorumTracker.h"
#include "xdr/Stellar-SCP.h"
#include <cstdint>
#include <memory>
#include <optional>
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

class HerderPersistence
{
  public:
    static std::unique_ptr<HerderPersistence> create(Application& app);

    virtual ~HerderPersistence()
    {
    }

    virtual void saveSCPHistory(uint32_t seq,
                                std::vector<SCPEnvelope> const& envs,
                                QuorumTracker::QuorumMap const& qmap) = 0;

    static size_t copySCPHistoryToStream(soci::session& sess,
                                         uint32_t ledgerSeq,
                                         uint32_t ledgerCount,
                                         XDROutputFileStream& scpHistory);
    // quorum information lookup
    static std::optional<Hash> getNodeQuorumSet(soci::session& sess,
                                                NodeID const& nodeID);
    static SCPQuorumSetPtr getQuorumSet(soci::session& sess,
                                        Hash const& qSetHash);

    static void maybeDropAndCreateNew(soci::session& sess);
    static void deleteOldEntries(soci::session& sess, uint32_t ledgerSeq,
                                 uint32_t count);
};
}
