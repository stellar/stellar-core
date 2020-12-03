// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz.h"
#include "test/FuzzerImpl.h"
#include "test/test.h"
#include "util/XDRStream.h"
#include "util/types.h"

#include <xdrpp/autocheck.h>
/**
 * This is a very simple fuzzer _stub_. It's intended to be run under an
 * external fuzzer with some fuzzing brains, at this time, preferably AFL.
 *
 * It has two modes:
 *
 *   - In genfuzz mode it spits out a small file containing a handful of
 *     random FuzzTransactionInputs or StellarMessages. This is the mode you use
 *     to generate seed data for the external fuzzer's corpus.
 *
 *   - In fuzz mode it reads back a file and appplies it to a stellar-core
 *     instance, applying but not committing transactions one by one to simulate
 *     certain transaction/overlay scenarios. It exits when it's applied the
 *     input. This is the mode the external fuzzer will run its mutant inputs
 *     through.
 *
 */

namespace stellar
{
namespace FuzzUtils
{
std::unique_ptr<Fuzzer>
createFuzzer(int processID, FuzzerMode fuzzerMode)
{
    gBaseInstance = processID;
    switch (fuzzerMode)
    {
    case FuzzerMode::OVERLAY:
        return std::make_unique<OverlayFuzzer>();
    case FuzzerMode::TRANSACTION:
        return std::make_unique<TransactionFuzzer>();
    default:
        abort();
    }
}
}

#define PERSIST_MAX 1000000
void
fuzz(std::string const& filename, std::vector<std::string> const& metrics,
     int processID, FuzzerMode fuzzerMode)
{
    auto fuzzer = FuzzUtils::createFuzzer(processID, fuzzerMode);
    fuzzer->initialize();

// "To make this work, the library and this shim need to be compiled in LLVM
// mode using afl-clang-fast (other compiler wrappers will *not* work)."
// -- AFL docs
#ifdef AFL_LLVM_MODE
    while (__AFL_LOOP(PERSIST_MAX))
#endif // AFL_LLVM_MODE
    {
        fuzzer->inject(filename);
    }
    fuzzer->shutdown();
}
}
