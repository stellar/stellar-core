#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/Fuzzer.h"
#include "util/Timer.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

class XDRInputFileStream;
class Simulation;
class Application;
struct StellarMessage;
struct Operation;

class TransactionFuzzer : public Fuzzer
{
  public:
    TransactionFuzzer()
    {
    }
    void inject(std::string const& filename) override;
    void initialize() override;
    void shutdown() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    VirtualClock mClock;
    std::shared_ptr<Application> mApp;
    PublicKey mSourceAccountID;
};

class OverlayFuzzer : public Fuzzer
{
    int const ACCEPTOR_INDEX = 0;
    int const INITIATOR_INDEX = 1;

  public:
    OverlayFuzzer()
    {
    }
    void inject(std::string const& filename) override;
    void initialize() override;
    void shutdown() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    std::shared_ptr<Simulation> mSimulation;
};
}