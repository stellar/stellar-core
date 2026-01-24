// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

/**
 * OverlayFuzzTarget.h - Overlay/P2P message fuzzing target
 *
 * This target fuzzes the overlay network by:
 * 1. Decoding fuzz input bytes into a StellarMessage
 * 2. Sending the message between two simulated peers
 * 3. Processing the message through the overlay layer
 */

#include "test/fuzz/FuzzTargetRegistry.h"
#include "test/fuzz/FuzzUtils.h"
#include "xdr/Stellar-types.h"

#include <memory>

namespace stellar
{

class Simulation;

class OverlayFuzzTarget : public FuzzTarget
{
  public:
    OverlayFuzzTarget() = default;

    std::string name() const override;
    std::string description() const override;
    void initialize() override;
    FuzzResultCode run(uint8_t const* data, size_t size) override;
    void shutdown() override;
    size_t maxInputSize() const override;
    std::vector<uint8_t> generateSeedInput() override;

  private:
    std::shared_ptr<Simulation> mSimulation;
    FuzzUtils::StoredLedgerKeys mStoredLedgerKeys;
    FuzzUtils::StoredPoolIDs mStoredPoolIDs;
    NodeID mInitiatorID;
    NodeID mAcceptorID;
};

} // namespace stellar
