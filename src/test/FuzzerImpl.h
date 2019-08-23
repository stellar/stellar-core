#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/Fuzzer.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

class XDRInputFileStream;
class Simulation;
struct StellarMessage;

class OverlayFuzzer : public Fuzzer
{
    const uint ACCEPTOR_INDEX = 0;
    const uint INITIATOR_INDEX = 1;

  public:
    OverlayFuzzer()
    {
    }
    void inject(XDRInputFileStream&) override;
    void initialize() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    std::shared_ptr<Simulation> mSimulation;
};
}