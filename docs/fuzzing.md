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
heuristic mutations (bit flips and such). It thus takes an initial interesting
corpus of inputs to the victim program, which it expands as it finds inputs that
take the program into new portions of its control-flow space and/or trigger crashes.
It writes elements of its corpus to disk in an output directory any time it finds
one that crashes the program or causes the program to hang in a new control-flow path.

It identifies control-flow paths by instrumenting basic blocks and storing an
in-memory, very dense set of counters associated with control-flow tuples. When
the victim program takes a branch, it's instrumented to bump a counter based on
the (src, dst) pair of PC addresses making up a branch-edge, and the set of
branch-tuple counts produces a control-flow "signature" for a run, which is used
to differentiate runs / explore the control-flow space.

Every run of the program starts anew and is fed a small binary input from the
corpus. This initial corpus is the result of minimizing (using [afl-cmin][3])
a much larger, randomly generated series of inputs, taking the smallest subset
that results in unique control-flow tuples.

To make this all work the program has to (a) run quickly, (b) take small inputs
in binary form and (c) be guided toward interesting edge cases just enough. We
currently have two fuzz modes, `tx` and `overlay`, and they **do not** perform the
above equally well. For starters, we have enabled [llvm_mode][9]'s persistent mode
for a modest 10x-100x improvement in execution/sec for *both* fuzz modes. Thus both
perform well in area (a) and with simple modifications we made to stellar-core, (b)
too. However, we've spent more energy with (c) for the `tx` fuzz mode, modifying
stellar-core such that it is more deterministic -- caches are cleared and randomness
seeded -- and it skips wasteful and inconsequential processes -- anything related to
signatures. For both modes, there is still much room for improvement in regards to
(a) and (c), one example being an isolation of the subsystem.


## Installing AFL

### Packages

Pre-packaged versions of AFL may be available.

For example, Ubuntu provides `AFL++` that can be installed with
```
  apt-get install afl++
```

### From source

Go to the [AFL repo][0], download the [tarball][2], unpack and run `make`, `make -C llvm_mode`
then `sudo make install`. This will install both `afl-fuzz`, the fuzzer itself, and `afl-gcc`,
`afl-clang`, and `afl-clang-fast`, which are compiler-wrappers that instrument binaries they
compile with the fuzzer's branch-tracking machinery (the later being necessary for `llvm_mode`
improvements described above).

note: you may have to provide a default clang/clang++, this can be done in many ways.

For example, this makes clang-8 the default:

```
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-8   81 --slave /usr/bin/clang++ clang++ /usr/bin/clang++-8    --slave /usr/share/man/man1/clang.1.gz clang.1.gz /usr/share/man/man1/clang-8.1.gz --slave /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-8  --slave /usr/bin/clang-format clang-format /usr/bin/clang-format-8 --slave /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-8
```

## Building an instrumented stellar-core

Start with a clean workspace, `make clean` or cleaner; then just do

```
# or any compile flags that you like
export CFLAGS='-O3 -g1' ; export CXXFLAGS="$CFLAGS"

# or any compiler that you want
export CC='clang' ; export CXX='clang++'
./autogen.sh && ./configure --enable-extrachecks --disable-postgres --enable-afl && make
```

make sure you have not enabled `asan` and `ccache`;
the former is incompatible, and the latter doesn't interoperate with the
compiler wrappers.


## Running the fuzzer

The simplest way is to set the environment variable `FUZZER_MODE` to `tx` or
to `overlay` and then `make fuzz`; this will do the following:

  - Create a directory `fuzz-testcases` for storing the initial corpus input
  - Run `stellar-core gen-fuzz fuzz-testcases/fuzz$i.xdr` ten thousand times
    to produce some basic seed input for the corpus
  - Run `afl-cmin` on the generated `fuzz-testcases`, minimizing the corpus
    to a unique, interesting subset, which will be put into a directory named
    `min-testcases`
  - Create a directory `fuzz-findings` for storing crash-producing inputs
  - Run `afl-fuzz` on `stellar-core fuzz`, using those corpus directories

You should get a nice old-school textmode TUI to monitor the fuzzer's progress;
it might be partly hidden depending on the color scheme of your terminal, as it
makes use of bold color highlighting. There are a lot of [interesting statistics][12]
displayed here.

When evaluating changes or posting a PR, include screenshots of the TUI
after similar runtimes from before and after the changes.  We've seen
~15% variability in metrics such as "total paths" after 10 minutes, so
you should choose a time significantly longer than that.  The [user guide][13]
documents interpretation of the many metrics.  At a minimum, we're interested
in:

- Exec speed
- Total paths
- Stability
- Whether the TUI displays any metrics in red (indicating values it considers
undesirable)

While it runs, it will write new crashes to files in `fuzz-findings`; before
pouncing on these as definite evidence of a flaw, you should confirm the crash
by feeding it to an instance of `stellar-core fuzz` run by hand, elsewhere (in
particular, not fighting for control over tmpdirs with the fuzzer's
`stellar-core` instances). Often a fuzzer "crash" is just the subprocess hitting
a ulimit; by default we use an 250mb virtual-address ulimit, thus it is
possible to exceed this. It is also useful to keep a separate build of
`stellar-core` in a different directory with `--enable-asan`, or valgrind, in
order to diagnose crashes. For more information on the [output][4] or [triaging
crashes][5] click the hyperlinks. 

It is also possible to run fuzzers in parallel across a [local machine][6] or
even across [separate machines][7] using ssh. This is a good way to significantly
improve the executions/sec, as the master-slave setup here utilizes, by default,
*dirty and quick mode*, a mode more *"akin to zzuf"* which skips deterministic
mutations for all the slaves. Scripts for bootstrapping are generally easy to write.
For a good place to start, check out some of the existing [AFL scripts and libraries][8]
on Github.


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

  - Try to use [LibFuzzer][11] or manual fork-mode to fork from an initialized state
    that is further along in memory; the difficult part is that `VirtualClock`
    and the associated IO loop is stateful and not friendly to forking, so
    we would need to tease apart portions of the program that can get their
    clock/IO service supplied late.

  - Consider using [DeepState][10], *"a framework that provides C and C++
    developers with a common interface to various symbolic execution and
    fuzzing engines*" for exposure to multiple fuzzing backends (with a single
    fuzz harness!).

  - Make startup-modes at different points in the process: instantiate an
    application and feed it transactions to run directly, not full
    `StellarMessage`s. Let the fuzzer generte bucket ledger entries, and try to
    apply them to the database as one would during catchup. This sort of thing.

  - Make a "postprocessor" using the AFL_POST_LIBRARY facility. This permits
    cleaning up or otherwise transforming fuzzer-generated data into data for
    the program, but it needs to be extremely robust.

  - Alternatively, make a "fuzzer-friendly" XDR message type that describes,
    using simple integer codes that the fuzzer will find it easy to perturb, a
    complete _test scenario_ or set of actions to apply to the program. In terms
    of setting up a network of nodes, a topology among them, a number of
    accounts and amounts, and a set of transactions to apply. Drive _that_ by
    the fuzzer, rather than single-action messages.


[0]: https://github.com/google/afl
[1]: http://rabbitbreeders.us/american-fuzzy-lop-rabbits
[2]: https://github.com/google/AFL/releases
[3]: https://github.com/google/AFL/blob/fb1f87177b78bc166dbbc6ffb8f3f8eb276c36cc/README.md#5-choosing-initial-test-cases
[4]: https://github.com/google/afl#7-interpreting-output
[5]: https://github.com/google/afl#10-crash-triage
[6]: https://github.com/google/AFL/blob/master/docs/parallel_fuzzing.txt#L29
[7]: https://github.com/google/AFL/blob/master/docs/parallel_fuzzing.txt#L89
[8]: https://github.com/google/AFL/blob/master/docs/sister_projects.txt#L106
[9]: https://github.com/google/AFL/tree/fb1f87177b78bc166dbbc6ffb8f3f8eb276c36cc/llvm_mode
[10]: https://github.com/trailofbits/deepstate
[11]: https://llvm.org/docs/LibFuzzer.html
[12]: https://github.com/google/AFL/blob/master/docs/status_screen.txt
[13]: https://afl-1.readthedocs.io/en/latest/user_guide.html

