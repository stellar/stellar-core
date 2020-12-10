#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <string>

namespace stellar
{

class XDRInputFileStream;

// Fuzzer is an encapsulation over some state that receives an
// XDRInputFileStream and deterministically injects it, applying it to its
// state.
class Fuzzer
{
  public:
    virtual ~Fuzzer()
    {
    }
    // inject receives an XDRInputFileStream attached to a fuzzed input file and
    // attempts to apply it to the state according to whatever apply may mean,
    // i.e. apply a transaction in the case of a TransactionFuzzer or send a
    // message in case of an OverlayFuzzer
    virtual void inject(std::string const& filename) = 0;
    virtual void initialize() = 0;
    virtual void shutdown() = 0;
    // genFuzz randomly generates an XDR input for the given fuzzer. For the
    // TransactionFuzzer, this is a xdr::xvector of Operations, for the
    // OverlayFuzzer this is a StellarMessage
    virtual void genFuzz(std::string const& filename) = 0;
    virtual int xdrSizeLimit() = 0;
};
}