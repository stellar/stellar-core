---
title: Fuzzing
---

This document describes the fuzzing infrastructure in stellar-core.

## Concepts

### Fuzz Drivers and Fuzz Targets

Fuzzing involves two key components:

- **Fuzz Driver**: A fuzzing engine that generates and mutates test inputs, tracks code coverage, and orchestrates the fuzzing process. Examples include libFuzzer, AFL++, and honggfuzz.

- **Fuzz Target**: A function that takes arbitrary input data and exercises some code path in the software being tested. The fuzz driver repeatedly calls the fuzz target with different inputs, looking for crashes, hangs, or other anomalous behavior.

stellar-core has multiple fuzz targets, each of which implements `stellar::FuzzTarget`. Some targets exercise code directly in stellar-core, while others pass through to fuzz targets defined in Soroban.

### Instrumentation

All the fuzzers we work with are so-called **greybox** fuzzers. This means the fuzzer has _some_ insight into the behaviour of the program. It is not just looking for crashes; it's also watching to see which inputs _exercise new paths_ in the control flow.

The fuzzer does this by using instrumentation that is injected into the binary at compile time. Specifically: it instruments every branch instruction in the program to increment a counter, and all the counters are hashed together during the run to tell the fuzzer if it hit a new control flow path.

This means that you have to compile core -- all of it, and soroban too -- with **instrumentation turned on** if you want the fuzzer to see anything. If instrumentation is turned off the fuzzer will be flying blind and will not
be able to work effectively.

## Architecture

### LibFuzzer-based Persistent Fuzzing API

Our fuzzing setup is based on the libFuzzer standard persistent entrypoint:  `LLVMFuzzerTestOneInput`:

```c
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
```

We provide an implementation of this (in `FuzzMain.cpp`) that is multiply compiled into separate binaries, each hard-wired to call a specific fuzz target, for each of our fuzz targets.

This API allows us to work with multiple fuzz drivers (libFuzzer, AFL++, honggfuzz, etc.) that all support this interface.

### Selection of fuzz drivers

We support 3 fuzz drivers:

1. **libFuzzer** (Clang's built-in fuzzer):
  ```bash
  CXXFLAGS="-O2 -g -fsanitize=fuzzer-no-link" \
  LIB_FUZZING_ENGINE="-fsanitize=fuzzer" \
  ./configure --enable-fuzz
  ```

2. **AFL++** (American Fuzzy Lop):
  ```bash
  CC=afl-clang-fast \
  CXX=afl-clang-fast++ \
  LIB_FUZZING_ENGINE="/path/to/AFLplusplus/libAFLDriver.a" \
  ./configure --enable-fuzz
  ```

3. **honggfuzz**:
  ```bash
  CC=hfuzz-clang \
  CXX=hfuzz-clang++ \
  LIB_FUZZING_ENGINE="/path/to/honggfuzz/libhfuzz/libhfuzz.a" \
  ./configure --enable-fuzz
  ```

Note that 3 separate things are happening here in configure:

  - The `--enable-fuzz` configure option enables building the fuzz targets.
  - The `LIB_FUZZING_ENGINE` variable specifies the library that provides a `main()` containing the fuzz driver that will call the fuzz target harness back.
  - The `CC`, `CXX`, and/or `CXXFLAGS` alter compilation to inject the fuzzer's greybox / coverage instrumentation into the compiled binary.

You need to do **all 3 of these** to get fuzzing to work.

### Binary Organization

Our fuzz targets are compiled into separate binaries, each with the fuzz driver supplying `main()`, us supplying `LLVMFuzzerTestOneInput`, and then everything else identical from one binary to the other. For example the `tx` fuzz target is compiled into a binary called `fuzz_tx`. To fuzz, you just run it; it has the fuzz driver linked into it.

This organization is dictated by [oss-fuzz](https://github.com/google/oss-fuzz), Google's continuous fuzzing service, which expects this structure for integration.

Each fuzz target binary can be run standalone with any compatible fuzz driver, or used directly for reproducing and debugging crashes.

## Testing Integration

### Corpus-based Unit Tests

Fuzz targets are reused as unit tests. The unit test suite runs each fuzz target against a retained corpus of interesting test cases. This ensures that:

1. Previously discovered edge cases continue to be tested
2. Regressions in fuzz target behavior are caught early
3. The corpus grows over time as new interesting inputs are found

## Command-line Tools

### Checking a Testcase: `fuzz-one`

stellar-core provides a one-shot `fuzz-one` command to check a given fuzz testcase:

```
stellar-core fuzz-one --target <target-name> <testcase-file>
```

This is useful for:
- Reproducing crashes found by fuzzers
- Debugging specific inputs
- Validating that a fix addresses a particular crash

It is _not_ useful for exploring the state-space of the fuzz target -- i.e. for "real fuzzing" under greybox supervision. Use the separate per-target binaries linked to fuzz drivers for real fuzzing (`fuzz_tx`, `fuzz_overlay`, etc.)

### Generating Seed Corpus: `gen-fuzz`

The `gen-fuzz` command generates a seed corpus for the fuzzers:

```
stellar-core gen-fuzz --target <target-name> --output-dir <directory> [--count <n>]
```

A good seed corpus helps fuzzers explore interesting code paths more quickly by providing structurally valid inputs as starting points for mutation.

