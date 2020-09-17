# Quick reference guide

## Running an instance

### Production instances
The [admin guide](software/admin.md) covers this in great detail.

You can run production Watchers and Validators.

### Debian packages
On Linux, the easiest is to install core using a [pre-packaged debian release](https://github.com/stellar/packages).

### Quick Start Docker image
You can use the [quickstart](https://github.com/stellar/docker-stellar-core-horizon)
image to run the latest stable and release candidate stellar-core with Docker.

### Dev container
Using vscode and the [container support in Visual Studio Code](../.devcontainer/README.md)
you can quickly build and run a stellar-core instance from your machine.

## Builds with testing hooks enabled
Builds from the stable repos are built with `--disable-tests` which removes test hooks from the build.
You can check if the build that you're using has them if `stellar-core help` returns help for the `test` subcommand:
```
  test                              execute test suite
```

## Interacting with a running instance

You can invoke the http admin interface with either an http client `curl http://localhost:11626/peers`
or using the `http-command` sub-command `stellar-core http-command peers`

### Logging
You can change log level using the `ll` command.
Log level can be controlled at the partition level.

There is a special partition called `perf` that tries to give a good view of performance
critical tasks.

A special command `logrotate` can be used in conjunction with log rotation tools. 

### Metrics
Core keeps tracks of [internal metrics](metrics.md), which includes all sorts of data about
what core is actually doing.

The `metrics` command returns all those metrics in one json object.

The `clearmetrics` command clears all metrics, this is useful when performing tests and you
want to get a clean reading (post setup) of certain metrics.

### Overlay network

You can get information from the overlay network:
 * locally with the `peers` endpoint
 * globally using the [survey](docs/../software/admin.md#overlay-topology-survey) commands

You have some control over which peers you're connected to:
 * the `ban` command allows to ban peers (so that neither inbound nor outbound are allowed until unbanned).
 * the `connect` command asks the instance to connect to a specific peer.
 * the `droppeer` command asks the instance to drop the connection with a specific peer.

### Maintenance

Core keeps old meta around for Horizon and other systems. As cursors get updated, automatic
maintenance normally deletes more than enough for the node to use a constant amount of disk space.

Sometimes you need to clean up more than this (for example, if you have a large maintenance debt).
In this case running the command `maintenance?count=100000000` (integer is a large number, bigger than your max backlog) will perform the full maintenance.

Note that this may hang the instance for minutes (hours even).

After performing such maintenance, you may issue a postgres `VACUUM FULL;` that will reclaim all
disk space (note that this requires a lot of free disk space).

### Testing

The `manualclose` command allows to close a ledger, this is used when the instance is
configured with `MANUAL_CLOSE=true`.
With this you can test various scenario where ledger boundaries could be important.

If `RUN_STANDALONE=true` along with `MANUAL_CLOSE=true`, then
the `manualclose` command may also control some of the parameters of the
ledger that it causes to close.  (The `closeTime` parameter may be no later
than the first second of the year 2200, GMT.)

On private networks, you can use the `generateload` command to create accounts and generate
synthetic traffic.

## Getting the most out of the command line

### Playing with history

You can interact with history archives with [stellar-archivist](https://github.com/stellar/go/tree/master/tools/stellar-archivist).

#### Command line catchup

Command line catchup allows to replay arbitrary ranges from history archives without
connecting to a network.
Note that as it doesn't connect to the network, it cannot know if the archives
are indeed pointing to a ledger state that is consistent with the network that you care about.

You can invoke command line catchup with the `catchup` command.

Note that you *can* publish to archive yourself when using command line catchup.
In fact this will happen if you have a writeable archive configured, so make sure
to disable that if that's not what you want to do!

`catchup` may exit with a publish backlog (see below).

##### Trusted ledger hashes

By default the offline `catchup` command will trust and replay whatever it downloads from an
archive. If the archive contents are malformed or tampered with, or corrupted in transit over an
insecure connection, this will only be discovered after catchup is complete, when the node tries to
join a network and acquire consensus. Until then, the node risks being exposed to (and replaying)
malformed input from the archive.

In order to mitigate this risk, stellar-core can emit a "reference file" full of trusted hashes for
the ledgers of checkpoints, anchored in an SCP consensus ledger value observed on the network it is
configured to trust.

This reference file can then be reused by running `catchup` with the `--trusted-checkpoint-hashes`
argument, passing the reference filename. Catchup will then check the target checkpoint against
the reference file _before_ replaying them, and fail if there is a hash mismatch.

To construct such a reference file, run the `verify-checkpoints` command, passing a config file as
usual (to specify the trusted network and quorum slice) and an `--output-filename` argument
specifying the reference file to save the trusted hashes in.

The emitted content of the refernce file will be a single JSON array of pairs of checkpoint ledger
numbers and strings holding hashes of ledger headers. For example, it might read:

```
[
[30393791, "a691c7cb9ea89f2936ab4a7f717856b0ecd7fd4f9e13662c03b77db931d53583"],
[30393727, "b911ef040b3babd021c40548d9f4c47f07d4f94481d65d51356450ac4357e62a"],
[30393663, "6785f25a9357d73001d33ad1883867f68e88d83f5dddf699324ecc0fa60dfa10"],
[30393599, "87cbd968a01c92658224e518d7cc7e9ab421c21a969cc726a0d2cff3c4300e0a"],
...
]
```

The file will contain one line per checkpoint for the entire history of the archive. This may be
quite large: hundreds of thousands of lines. Furthermore, generating the reference file will take
some time, as the entire sequence of ledger _headers_ in the archive (though none of the
transactions or ledger states) must be downloaded and verified sequentially. It may therefore be
worthwhile to save and reuse such a trusted reference file multiple times before regenerating it.

##### Experimental fast "meta data generation"
`catchup` has a command line flag `--in-memory` that when combined with the
`METADATA_OUTPUT_STREAM` allows a stellar-core instance to stream meta data instead
of using a database as intermediate store.

This has been tested as being orders of magnitude faster for replaying large sections
of history.

If you don't specify any value for stream the command will just replay transactions
in memory and throw away all meta. This can be useful for performance testing the transaction processing subsystem.

The `--in-memory` flag is also supported by the `run` command, which can be used to
run a lightweight, stateless validator or watcher node, and this can be combined with
`METADATA_OUTPUT_STREAM` to stream network activity to another process.

By default, such a stateless node in `run` mode will catch up to the network starting from the
network's most recent checkpoint, but this behaviour can be further modified using two flags
(that must be used together) called `--start-at-ledger <N>` and `--start-at-hash <HEXHASH>`. These
cause the node to start with a fast in-memory catchup to ledger `N` with hash `HEXHASH`, and then
replay ledgers forward to the current state of the network.


#### Publish backlog
There is a command `publish` that allows to flush the publish backlog without starting
core. This can be useful to run to guarantee that certain tasks are done before moving
a node to production.

#### Querying archives

There is a command `report-last-history-checkpoint` that gather information about the
last published checkpoint found in archives.

This can be useful to automate processing of data from archives from shell scripts for example.

```
Last history checkpoint {
    "version": 1,
    "server": "stellar-core 12.3.0 (5a8f8b13af5f78704bf0e7b5de4314c16a3639a5)",
    "currentLedger": 89663,
    "currentBuckets": [
        {
            "curr": "7cfc8023b7bb9d543d99df7de45528388c28f983c87858d6796a9376a2a21139",
            "next": {
                "state": 0
            },
            "snap": "8b57d72c92ad53dc46d5adbc742d6bc6167d99b764b018758a28ba7e6f9fe90e"
        },
        ...
```
#### Verifying archives
You can use the `catchup --extra-verification --archive ARCHIVE_NAME` options to
verify the integrity of an archive while replaying.

This will verify that the ledger headers, transaction sets, transaction results are
consistent.

### Ledger state

#### Offline info

The command `offline-info` fetches information similar to what `info` returns but
without starting the node.

This allows to quickly see information about the last loaded ledger, supported protocol version etc.

#### Testing around a snapshot
If you want to replay transactions in a specific ledger, you can run a command
like `catchup 2000/1` (point of interest: ledger 2000).

The problem is that this will download the ledger state corresponding to the
checkpoint right before that ledger and replay transactions to get to the ledger
of interest which can be very slow.
```
[History INFO] Applying buckets
[Tx INFO] applying ledger 1984 (txs:1, ops:1)
[Tx INFO] applying ledger 1985 (txs:3, ops:3)
[Tx INFO] applying ledger 1986 (txs:1, ops:1)
[Tx INFO] applying ledger 1987 (txs:1, ops:1)
[Tx INFO] applying ledger 1988 (txs:1, ops:1)
[Tx INFO] applying ledger 1989 (txs:1, ops:1)
[Tx INFO] applying ledger 1990 (txs:1, ops:1)
[Tx INFO] applying ledger 1991 (txs:1, ops:1)
[Tx INFO] applying ledger 1992 (txs:1, ops:1)
[Tx INFO] applying ledger 1993 (txs:1, ops:1)
[Tx INFO] applying ledger 1994 (txs:2, ops:2)
[Tx INFO] applying ledger 1995 (txs:1, ops:1)
[Tx INFO] applying ledger 1996 (txs:3, ops:3)
[Tx INFO] applying ledger 1997 (txs:1, ops:1)
[Tx INFO] applying ledger 1998 (txs:2, ops:2)
[Tx INFO] applying ledger 1999 (txs:2, ops:2)
[Tx INFO] applying ledger 2000 (txs:1, ops:1)
```

It also makes it hard to analyze a very specific range.

The solution is to first catchup to the ledger right before the ledger of
interest: `catchup 1999/0`.

Then, take a snapshot of the database and bucket directory (if using sqlite,
this is trivial).

Later, you can restore that starting state and replay the actual ledger that
you're interested in:
`catchup 2000/1`.
This makes it possible to run a lot of tests very quickly for that one ledger
of interest.

#### Rebuilding the database
In some rare cases, you may have a corrupted SQL database, and all you got are
the buckets.

The `rebuild-ledger-from-buckets` allows to do just that.

### XDR

There are two commands that can parse XDR.

The `dump-xdr` command is designed to dump files that contain xdr stream like the
ones found in the archive subsystem.

The `print-xdr` command is designed to only parse a single XDR object. It can work
off binary or base64 encoded data.
By default it tries to automatically detect the XDR type.

```
stellar-core print-xdr --base64 tx-result.txt
TransactionResult = {
  feeCharged = 100,
  result = {
    code = txSUCCESS,
    results = [
      { code = opINNER,
        tr = {
          type = PAYMENT,
          paymentResult = {
            code = PAYMENT_SUCCESS
          }
        } }
    ]
  },
  ext = {
    v = 0
  }
}
```

### Keys

You can sign transactions with the `sign-transaction` command.

You can generate a new Stellar public/private key with the `gen-seed` command.

```
stellar-core.exe gen-seed
Secret seed: SAHP7BHVCCJ6BUYT56IMKQDMQT3HRGSRTEAQ2JAUXNQ7UQ7OFDN4Y2WS
Public: GDJLE5FAS4RVCWXAXUU226TGKXCAX7WFRNVCP75BNPWOUW7QSLKZZTUH
```

The command `convert-id` allows you to quickly see various representation of a given key.

```
stellar-core convert-id GDJLE5FAS4RVCWXAXUU226TGKXCAX7WFRNVCP75BNPWOUW7QSLKZZTUH
PublicKey:
  strKey: GDJLE5FAS4RVCWXAXUU226TGKXCAX7WFRNVCP75BNPWOUW7QSLKZZTUH
  hex: d2b274a09723515ae0bd29ad7a6655c40bfec58b6a27ffa16becea5bf092d59c
```

### Low privilege stellar-core
It's possible to perform the schema updates in isolation using
`  upgrade-db                        upgrade database schema to current version`

This allows to use credentials without schema update rights for stellar-core.
