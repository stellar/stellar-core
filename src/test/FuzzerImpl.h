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
class Application;
struct StellarMessage;
struct Operation;

class TransactionFuzzer : public Fuzzer
{
  public:
    TransactionFuzzer(unsigned int numAccounts, int processID)
        : mNumAccounts(numAccounts), mProcessID(processID)
    {
    }
    void inject(XDRInputFileStream&) override;
    void initialize() override;
    void genFuzz(std::string const& filename) override;
    int xdrSizeLimit() override;

  private:
    std::shared_ptr<Application> mApp;
    PublicKey mSourceAccountID;
    unsigned int mNumAccounts;
    int mProcessID;
};

class OverlayFuzzer : public Fuzzer
{
    const int ACCEPTOR_INDEX = 0;
    const int INITIATOR_INDEX = 1;

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