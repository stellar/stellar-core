# Purpose of this document

This document highlights the various ways a particular branch or changeset can be evaluated when it comes to performance characteristics.

When submitting changes that may impact the network's performance, we require to perform some level of comparison of the changes between that version and  latest master or release.

# Metrics being used for all scenarios

* Execution (real) time
* CPU utilization at various interval, and/or percentile
* Disk I/O at the operating system level (ie: unit is typically in blocks/s), both reads and writes of the stellar-core process
* SQL: rate of operations (read and writes), Disk I/O of the SQL process if available (*PostgreSQL*)
* Memory utilization

# High level scenarios

## Networks to test against

There are 3 types of network available for testing.

### Testnet
#### What it's good for
* Testnet provides a realistic and moderate size dataset (transactions and ledger), and a few validators.
* Load testing of simple scenarios.

#### What it's not so good for
* The ledger size may not be big enough depending on the test being performed.
* SCP and flooding related changes typically require all validators to run code compatible with a change.
* Load testing or stress testing are not going to yield meaningful results on testnet due to the centralized nature of the deployment and dependence on specific compute profile.

### Public network
#### What it's good for
* The public network has historical data dating back to when the network launched in 2015, the transaction history is as diverse as it can be.
* Validating the performance characteristics of old versions of the protocol - which may allow to quantify potential performance regressions in old versions (that can be acceptable).
* The public network has a much more diverse set of validators than testnet, which allows better evaluation of SCP related metrics.

#### What it's not good for
* SCP and flooding related changes typically require all validators to run code compatible with a change.
* The ledger may not be large enough when evaluating future trends.
* Stress testing is expensive on the network and may lead to unnecessary outage.

### Private network
#### What it's good for
* Injecting large amount of transactions in a network while controlling all parameters (can use some data from public network if using the same passphrase).
* Evaluating SCP and overlay for various network and trust topologies.
Load testing (observe baseline metrics) and stress testing (evaluating breaking points).

#### What it's not good for
* Evaluating heterogeneous environment (location of validators, connectivity, compute profile). This can be somewhat approximated by spinning up capacity in various datacenters (at a cost).

## Joining a network
* Join a network with an empty node, wait until it’s fully in sync.
    * this measures the overhead to get overlay up to speed as well as performing the various catchup tasks
* Catchup complete (replay entire history) vs recent (only replay around X ledgers from current).

## Steady state
* Observe a node for X (X=5/15/30) minutes in the “Synced!” state.

# Notable sub-scenarios

## Catch-up over specified ranges of ledgers

Performed using the command line catchup command (does not depend on overlay).

In order, this tests the following sequence:
1. Download data from history
2. Apply buckets from a fresh database
    * Variable here is size of each bucket (~number of ledger entries)
3. Apply transactions from N ledgers
    * Variables to look for are the composition of each ledger (transaction set size, some ranges are busier than others)

## Inject transactions
Inject transactions, wait until they are incorporated into a ledger.

This scenario looks at the overhead of flooding transactions and SCP messages (required for transactions to be included in a ledger).

### Built-in load generator
stellar-core has a built-in load generator that allows to inject transactions on private networks.
See the `generateload` [command](docs/software/commands.md) for more detail.

## Micro-benchmarks

Some tests (usually hidden, must be run directly) micro-benchmark (test tags contain `bench`, like `bucketbench`) or exercise certain parts of the code (for example `[tx]` runs tests for all transaction related code).

They can be used as a way to demonstrate specific improvements in a specific subsystem.

In some cases it may make sense to submit changes to those tests (or write new micro-benchmarks) with the pull request.

# Measuring metrics
## Built-in metrics
Calling the `metrics` [command](docs/software/commands.md) allows to gather the metrics at various intervals.

### Notable metrics

See [docs/metrics](docs/metrics.md) for information on metrics exposed by stellar-core.

## System metrics
Tools used to gather those metrics are O/S specific.

### General
#### Linux
`uptime` which reports the load average, can be a really good indicator of how a machine is performing in aggregate.

`top` is often a good enough tool to give an idea of what is going on in the system.

For a list of performance related tool, see https://en.wikipedia.org/wiki/Load_(computing)


### CPU
#### Linux


Utilization per processor
```
$ mpstat -P ALL
06:16:11 PM  CPU    %usr   %nice    %sys %iowait    %irq   %soft  %steal  %guest  %gnice   %idle
06:16:11 PM  all    9.13    0.01    2.07    0.40    0.00    0.36    0.08    0.00    0.00   87.95
06:16:11 PM    0   10.41    0.01    2.42    0.46    0.00    0.73    0.12    0.00    0.00   85.86
06:16:11 PM    1    7.87    0.00    1.72    0.34    0.00    0.00    0.04    0.00    0.00   90.01
```

### Disk I/O
#### Linux

It is a best practice to clear all I/O caches between runs.

On Linux system, this can be done with a command like

    sync ; echo 3 > /proc/sys/vm/drop_caches

`iotop` is the equivalent of `top` for I/O; it also allows to aggregate data, which can be useful to identify small but steady utilization of I/O subsystems.

Basic view:
```
iostat -d
Linux 3.13.0-139-generic (core-live-005)        04/10/2018      _x86_64_        (2 CPU)

Device:            tps    kB_read/s    kB_wrtn/s    kB_read    kB_wrtn
xvda              0.82         0.23         8.53    1795356   66176300
xvdb              5.84         0.06        48.81     491097  378539212
xvdh              5.85         2.30        78.16   17845337  606107180
```

Detailed view:
```
$ iostat -d -x
Device:         rrqm/s   wrqm/s     r/s     w/s    rkB/s    wkB/s avgrq-sz avgqu-sz   await r_await w_await  svctm  %util
xvda              0.00     0.79    0.01    0.80     0.23     8.53    21.46     0.00    5.39    7.54    5.35   1.20   0.10
xvdb              0.00     6.49    0.01    5.83     0.06    48.81    16.75     0.00    0.18    0.31    0.18   0.12   0.07
xvdh              0.00     4.56    0.24    5.61     2.30    78.16    27.50     0.11   18.70   11.23   19.02   1.61   0.94
```

# Profiling
This section contains some examples on how to perform profiling on different platforms.

## Linux
`perf` is a good alternative to `gprof` for event based profiling. A good tutorial is https://perf.wiki.kernel.org/index.php/Tutorial

A very good source of how to use perf can be found at http://www.brendangregg.com/linuxperf.html

### Environment preparation

#### Kernel setup
If you want to profile as a regular user, with `sysctl` check the values of:

    kernel.perf_event_max_contexts_per_stack
    kernel.perf_event_max_stack
    kernel.perf_event_paranoid
    kernel.perf_event_max_sample_rate
    kernel.perf_event_mlock_kb

In particular
     * `kernel.perf_event_paranoid` that when above 1, disables CPU events.
     * `kernel.sched_schedstats` should be set to 1

If they are not what you want, you can set them, as root:

    echo 'kernel.perf_event_paranoid=0' '/etc/sysctl.d/51-enable-perf-events.conf'
    sysctl kernel.perf_event_paranoid=0


#### Compile flags
Preparing the binary to be "perf friendly" (run before running `configure`):
```
# whatever flags you want to set, -O2 is typical
export CFLAGS="-O2"
export CXXFLAGS="-O2"

# make things "perf friendly"
export CFLAGS="$CFLAGS -fno-omit-frame-pointer -ggdb"
export CXXFLAGS="$CXXFLAGS -fno-omit-frame-pointer -ggdb"
```

#### Reducing the dataset

A run can be scoped to
* only a subset of threads, see the `--tid` option
* a specific range, for example only capture data during "normal" operation,
excluding startup/shutdown.
   * this is achieved by invoking `perf` separately and using the `--pid` option
   combined with a command like ` -- sleep 30s` to capture events for a specific time.

#### Tips for getting the best stack traces

Note: on some systems, the quality of symbols differs depending on the tool-chain. If you don't get good stack traces, try switching to gcc/g++.

In order to improve the quality of stack traces:
* Use your own version of dependencies (sqlite, etc) with the same compile options (done for you by stellar-core's configure script) 
* Use alternate libraries such as google-tcmalloc
```
# you may need to install a package such as libtcmalloc-minimal4 for this to work
export LDFLAGS=-ltcmalloc_minimal
# alternatively, you can start "stellar-core", with something like
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so ./stellar-core ...
```
* use clang/clang++ and a custom `libc++` compiled with the same compilation options that you use.
    * See [building a custom libc++](CONTRIBUTING.md#building-a-custom-libc)
    * Do not enable memory sanitizer for that version, instead run something like
```
    svn co http://llvm.org/svn/llvm-project/llvm/trunk llvm
    (cd llvm/projects && svn co http://llvm.org/svn/llvm-project/libcxx/trunk libcxx)
    (cd llvm/projects && svn co http://llvm.org/svn/llvm-project/libcxxabi/trunk libcxxabi)
    mkdir lbcxx_perf && cd libcxx_perf
    cmake ../llvm -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS='-O2 -g1 -fno-omit-frame-pointer' -DCMAKE_CXX_FLAGS='-O2 -g1 -fno-omit-frame-pointer'
    make cxx -j7
    export LIBCXX_PATH=`pwd`/lib
```

### Flame graphs

A very good way to summarize data is to use flame graphs.

A good tool for this is located at https://github.com/brendangregg/FlameGraph

You can just pull a local copy with

    git clone https://github.com/brendangregg/FlameGraph

### Example perf sessions
#### Sampling session

This example grabs 60 seconds of data from a running stellar-core instance.

    perf record -F 99 -p $(pgrep stellar-core) --call-graph dwarf,20000 -o perf-cpu.data -- sleep 60

To view the report: `perf report -n  -F +period -i perf-cpu.data`

To generate a flame graph:

    perf report --stdio -i perf-cpu.data --no-children -n -g folded,0,caller,count -s comm | awk '/^ / { comm = $3 } /^[0-9]/ { print comm ";" $2, $1 }'  > perf-cpu.folded
    ~/src/FlameGraph/flamegraph.pl perf-cpu.folded > ~/reports/folded-cpu.svg

This generates a report that looks like [this](sample-reports/folded-cpu.svg)

#### "Off CPU" session

Records for 60 seconds events related to when a running stellar-core process is waiting on something.

As most threads "sleep" (waiting for some work), most of the interesting data in this report is in the smaller parts of the report (as the cummulative time of threads waiting for work is dominant).

Note: "counts" in this report represent time in milliseconds.

    perf record -e 'sched:sched_stat_sleep,sched:sched_switch,sched:sched_process_exit' -p $(pgrep stellar-core) --call-graph dwarf -o perf-offcpu-raw.data  -- sleep 60
    sudo perf inject -f -v -s -i perf-offcpu-raw.data -o perf-offcpu.data
    sudo chown user.user perf-offcpu.data

To view the report: `perf report -n  -F +period -i perf-offcpu.data`

To generate a flame graph:

    perf report --stdio -i perf-offcpu.data --no-children -n -g folded,0,caller,period -s comm | awk '/^ / { comm = $3 } /^[0-9]/ { print comm ";" $2, $1/1000000 }'  > perf-offcpu.folded
    ~/src/FlameGraph/flamegraph.pl perf-offcpu.folded > ~/reports/folded-offcpu.svg

This generates a report that looks like [this](sample-reports/folded-offcpu.svg)

#### common issues

```
event syntax error: 'sched:sched_stat_sleep,sched:sched_switch,sched:sched_process_exit'
                     \___ can't access trace events

Error:  No permissions to read /sys/kernel/debug/tracing/events/sched/sched_stat_sleep
Hint:   Try 'sudo mount -o remount,mode=755 /sys/kernel/debug/tracing'
```

solution is, as root to run

    sysctl kernel.perf_event_paranoid=-1
    sysctl kernel.sched_schedstats=1

## Windows
The main page for the profiler built into Visual Studio Community Edition is located there:  https://docs.microsoft.com/en-us/visualstudio/profiling/index

## All platforms

Intel V-Tune (free, unlimited license 90 days renewal) https://software.intel.com/en-us/system-studio/choose-download

