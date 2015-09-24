---
title: Fuzzing
---

This is a little howto on using the [AFL][0] (["American Fuzzy Lop"][1])
fuzzer with stellar-core. Support for this is still preliminary but it has
already shaken a couple bugs out and will no doubt find more the further down
this road we go, so you're encouraged to give this a try.

## Theory of operation

AFL is a black-box fuzzer -- it knows nothing much about the thing it's fuzzing
-- that uses an evolutionary algorithm to breed malicious input under a bunch of
heuristic mutations (bit flips and such). It thus uses a corpus of small inputs
to the victim program, which it expands as it finds inputs that take the program
into new portions of its control-flow space and/or trigger crashes. It writes
elements of its corpus to disk in an output directory any time it finds one that
crashes the program in a new control-flow path.

It identifies control-flow paths by instrumenting basic blocks and storing an
in-memory, very dense set of counters associated with control-flow tuples. When
the victim program takes a branch, it's instrumented to bump a counter based on
the (src, dst) pair of PC addresses making up a branch-edge, and the set of
branch-tuple counts produces a control-flow "signature" for a run, which is used
to differentiate runs / explore the control-flow space.

Every run of the program starts anew and is fed a small binary input from the
corpus. So to make this all work the program has to (a) run quickly and (b) take
small inputs in binary form. We've modified stellar-core to have a mode for (b)
but (a) is still unsatisfactory; the binary-input mode starts up a pair of
Application objects in loopback configuration, and reads-and-receives a
StellarMessage into them. This is quite a lot of setup and the fuzzing speed of
AFL suffers as a result (it actually complains that the target is too slow).


## Installing AFL

Go to the [AFL website][0], download the [tarball][2], unpack and run `make`,
then `sudo make install`. This will install both `afl-fuzz`, the fuzzer itself,
and `afl-gcc` and `afl-clang`, which are compiler-wrappers that instrument
binaries they compile with the fuzzer's branch-tracking machinery.


## Building an instrumented stellar-core

Start with a clean workspace, `make clean` or cleaner; then just do `./configure
--enable-afl && make` and make sure you have not enabled `asan` and `ccache`;
the former is incompatible, and the latter doesn't interoperate with the
compiler wrappers.


## Running the fuzzer

The simplest way is `make fuzz`; this will do the following:

  - Create a directory `fuzz-testcases` for storing the corpus input
  - Run `stellar-core --genfuzz fuzz-testcases/fuzz$i.xdr` ten times to produce
    some basic seed input for the corpus.
  - Create a directory `fuzz-findings` for storing crash-producing inputs.
  - Run `afl-fuzz` on `stellar-core --fuzz`, using those corpus directories.

You should get a nice old-school textmode TUI to monitor the fuzzer's progress;
it might be partly hidden depending on the color scheme of your terminal, as it
makes use of bold color highlighting.

While it runs, it will write new crashes to files in `fuzz-findings`; before
pouncing on these as definite evidence of a flaw, you should confirm the crash
by feeding it to an instance of `stellar-core --fuzz` run by hand, elsewhere (in
particular, not fighting for control over tmpdirs with the fuzzer's
`stellar-core` instances). Often a fuzzer "crash" is just the subprocess hitting
a ulimit; by default we use an 8GB virtual-address ulimit, but it is still
possible to exceed this. It is also useful to keep a separate build of
`stellar-core` in a different directory with `--enable-asan`, or valgrind, in
order to diagnose crashes.


## Future directions

Aside from "continuous fuzzing" and "fuzzing for a certain amount of time as
part of staging-tests", here are some directions I think we should take fuzzing:

  - Enable AFL_HARDEN, adds some runtime memory checks that trap more errors.

  - Try AFL_NO_VAR_CHECK and see if it speeds things up.

  - Try limiting the instrumentation to `stellar-core` itself, not libsodium,
    soci, sqlite, medida, and so forth.

  - Measure and shave-down the startup path for fuzzing so it's as fast as
    possible. The fewer instructions there are from `main()` to "doing something
    with input", the better.

  - Try to use LibFuzzer or manual fork-mode to fork from an initialized state
    that is further along in memory; the difficult part is that `VirtualClock`
    and the associated IO loop is stateful and not friendly to forking, so
    we would need to tease apart portions of the program that can get their
    clock/IO service supplied late.

  - Make startup-modes at different points in the process: instantiate an
    application and feed it transactions to run directly, not full
    `StellarMessage`s. Let the fuzzer generte bucket ledger entries, and try to
    apply them to the database as one would during catchup. This sort of thing.

  - Add a mode -- with a giant red flashing TESTING_ONLY light on it -- that
    makes crypto signatures always pass. A lot of bad fuzzer-input will be
    caught and rejected by mismatched Ed25519 signatures; this is not ideal,
    as an attacker who figures out how to break `stellar-core` will be able
    to sign their bad input to make it past that check. So we should omit it
    in at least some, if not all, fuzzing modes.

  - Make a "postprocessor" using the AFL_POST_LIBRARY facility. This permits
    cleaning up or otherwise transforming fuzzer-generated data into data for
    the program, but it needs to be extremely robust.

  - Alternatively, make a "fuzzer-friendly" XDR message type that describes,
    using simple integer codes that the fuzzer will find it easy to perturb, a
    complete _test scenario_ or set of actions to apply to the program. In terms
    of setting up a network of nodes, a topology among them, a number of
    accounts and amounts, and a set of transactions to apply. Drive _that_ by
    the fuzzer, rather than single-action messages.


[0]: http://lcamtuf.coredump.cx/afl/
[1]: http://rabbitbreeders.us/american-fuzzy-lop-rabbits
[2]: http://lcamtuf.coredump.cx/afl/releases/afl-latest.tgz
