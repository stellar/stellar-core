#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderPersistence.h"

namespace stellar
{
class Application;

class HerderPersistenceImpl : public HerderPersistence
{

  public:
    HerderPersistenceImpl(Application& app);
    ~HerderPersistenceImpl();

    void saveSCPHistory(uint32_t seq, std::vector<SCPEnvelope> const& envs,
                        QuorumTracker::QuorumMap const& qmap) override;

    void copySCPHistoryFromEntries(SCPHistoryEntryVec const& hEntries,
                                   uint32_t ledgerSeq) override;

  private:
    Application& mApp;

    // Save quorum sets at a given sequence number
    void saveQuorumSets(uint32_t seq,
                        UnorderedMap<Hash, SCPQuorumSetPtr> const& qsets);

    // Delete `scphistory` entries at a given sequence number.
    void clearSCPHistoryAtSeq(uint32_t seq);
};
}
