---
title: Commands
replacement: https://developers.stellar.org/docs/run-core-node/commands/
---

stellar-core can be controlled via the following commands.

## Common options
Common options can be placed at any place in the command line.

* **--conf <FILE-NAME>**: Specify a config file to use. You can use 'stdin' and
  provide the config file via STDIN. *default 'stellar-core.cfg'*
* **--ll <LEVEL>**: Set the log level. It is redundant with `http-command ll`
  but we need this form if you want to change the log level during test runs.
* **--metric <METRIC-NAME>**: Report metric METRIC on exit. Used for gathering
  a metric cumulatively during a test run.
* **--help**: Show help message for given command.

## Command line options
Command options can only by placed after command.

* **catchup <DESTINATION-LEDGER/LEDGER-COUNT>**: Perform catchup from history
  archives without connecting to network. For new instances (with empty history
  tables - only ledger 1 present in the database) it will respect LEDGER-COUNT
  configuration and it will perform bucket application on such a checkpoint
  that at least LEDGER-COUNT entries are present in history table afterwards.
  For instances that already have some history entries, all ledgers since last
  closed ledger will be replayed.<br>
  Option **--trusted-checkpoint-hashes <FILE-NAME>** checks the destination
  ledger hash against the provided reference list of trusted hashes. See the
  command verify-checkpoints for details.
* **convert-id <ID>**: Will output the passed ID in all known forms and then
  exit. Useful for determining the public key that corresponds to a given
  private key. For example:

`$ stellar-core convert-id SDQVDISRYN2JXBS7ICL7QJAEKB3HWBJFP2QECXG7GZICAHBK4UNJCWK2`
* **dump-ledger**: Dumps the current ledger state from bucket files into
    JSON **--output-file** with optional filtering. **--last-ledgers** option
    allows to only dump the ledger entries that were last modified within that
    many ledgers. **--limit** option limits the output to that many arbitrary
    records. **--filter-query** allows to specify a filtering expression over
    `LedgerEntry` XDR. Expression should evaluate to boolean and consist of
    field paths, comparisons, literals, boolean operators (`&&`, ` ||`) and
    parentheses. The field values are consistent with `print-xdr` JSON
    representation: enums are represented as their name strings, account ids as
    encoded strings, hashes as hex strings etc. Filtering is useful to minimize
    the output JSON size and then optionally process it further with tools like
    `jq`. Query filter examples:
    
    * `data.type == 'OFFER'` - dump only offers
    * `data.account.accountID == 'GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA' || 
       data.trustLine.accountID == "GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"`
       - dump only account and trustline entries for the specified account.
    * `data.account.inflationDest != NULL` - dump accounts that have an optional
      `inflationDest` field set.
    * `data.offer.selling.assetCode == 'FOOBAR' &&
       data.offer.selling.issuer == 'GAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'` -
       dump offers that are selling the specified asset.
    * `data.trustLine.ext.v1.liabilities.buying < data.trustLine.ext.v1.liabilities.selling` -
      dump trustlines that have buying liabilites less than selling liabilites
    * `(data.account.balance < 100000000 || data.account.balance >= 2000000000) 
       && data.account.numSubEntries > 2` - dump accounts with certain balance 
       and sub entries count, demonstrates more complex expression
   
   This command may also be used to aggregate ledger data to a CSV table using all 
   the above options in combination with **--agg** and (optionally) **--group-by**.
   **--agg** supports the following aggregation functions: `sum`, `avg` and `count`.
   For example:

   * `--group-by "data.type" --agg "count()"` - find the count of entries per type.
   * `--group-by "data.offer.selling.assetCode, data.offer.selling.issuer" 
      --agg "sum(data.offer.amount), avg(data.offer.amount)"` - find the total offer
      amount and average offer amount per selling offer asset name and issuer.
   
   See more examples in [ledger_query_examples.md](ledger_query_examples.md).

* **dump-xdr <FILE-NAME>**:  Dumps the given XDR file and then exits.
* **encode-asset --code <CODE> --issuer <ISSUER>**: Prints a base-64 encoded asset.
  Prints the native asset if neither `code` nor `issuer` is given.
* **fuzz <FILE-NAME>**: Run a single fuzz input and exit.
* **gen-fuzz <FILE-NAME>**:  Generate a random fuzzer input file.
* **gen-seed**: Generate and print a random public/private key and then exit.
* **help**: Print the available command line options and then exit..
* **http-command <COMMAND>** Send an [HTTP command](#http-commands) to an
  already running local instance of stellar-core and then exit. For example: 

`$ stellar-core http-command info`

* **load-xdr <FILE-NAME>**:  Load an XDR bucket file, for testing.
* **new-db**: Clears the local database and resets it to the genesis ledger. If
  you connect to the network after that it will catch up from scratch.
* **new-hist <HISTORY-LABEL> ...**:  Initialize the named history archives
  HISTORY-LABEL. HISTORY-LABEL should be one of the history archives you have
  specified in the stellar-core.cfg. This will write a
  `.well-known/stellar-history.json` file in the archive root.
* **offline-info**: Returns an output similar to `--c info` for an offline
  instance, but written directly to standard output (ignoring log levels).
* **print-xdr <FILE-NAME>**:  Pretty-print a binary file containing an XDR
  object. If FILE-NAME is "stdin", the XDR object is read from standard input.<br>
  Option **--filetype [auto|ledgerheader|meta|result|resultpair|tx|txfee]**
  controls type used for printing (default: auto).<br>
  Option **--base64** alters the behavior to work on base64-encoded XDR rather than
  raw XDR, and converts a stream of encoded objects separated by space/newline.
* **publish**: Execute publish of all items remaining in publish queue without
  connecting to network. May not publish last checkpoint if last closed ledger
  is on checkpoint boundary.
* **report-last-history-checkpoint**: Download and report last history
  checkpoint from a history archive.
* **run**: Runs stellar-core service.<br>
  Option **--wait-for-consensus** lets validators wait to hear from the network
  before participating in consensus.<br>
  Option **--in-memory** stores the current ledger in memory rather than a
  database.<br>
  Option **--start-at-ledger <N>** starts **--in-memory** mode with a catchup to
  ledger **N** then replays to the current state of the network.<br>
  Option **--start-at-hash <HASH>** provides a (mandatory) hash for the ledger
  **N** specified by the **--start-at-ledger** option.
* **sec-to-pub**:  Reads a secret key on standard input and outputs the
  corresponding public key.  Both keys are in Stellar's standard
  base-32 ASCII format.
* **self-check**: Perform history-related sanity checks, and it is planned
  to support other kinds of sanity checks in the future.
* **sign-transaction <FILE-NAME>**:  Add a digital signature to a transaction
  envelope stored in binary format in <FILE-NAME>, and send the result to
  standard output (which should be redirected to a file or piped through a tool
  such as `base64`).  The private signing key is read from standard input,
  unless <FILE-NAME> is "stdin" in which case the transaction envelope is read from
  standard input and the signing key is read from `/dev/tty`.  In either event,
  if the signing key appears to be coming from a terminal, stellar-core
  disables echo. Note that if you do not have a STELLAR_NETWORK_ID environment
  variable, then before this argument you must specify the --netid option. For
  example, the production stellar network is "`Public Global Stellar Network ;
  September 2015`" while the test network is "`Test SDF Network ; September
  2015`".<br>
  Option --base64 alters the behavior to work on base64-encoded XDR rather than
  raw XDR.
* **test**: Run all the unit tests.
  * Suboptions specific to stellar-core:
      * `--all-versions` : run with all possible protocol versions
      * `--version <N>` : run tests for protocol version N, can be specified
      multiple times (default latest)
      * `--base-instance <N>` : run tests with instance numbers offset by N,
      used to run tests in parallel
  * For [further info](https://github.com/philsquared/Catch/blob/master/docs/command-line.md)
    on possible options for test.
  * For example this will run just the tests tagged with `[tx]` using protocol
    versions 9 and 10 and stop after the first failure:
    `stellar-core test -a --version 9 --version 10 "[tx]"`
* **upgrade-db**: Upgrades local database to current schema version. This is
  usually done automatically during stellar-core run or other command.
* **verify-checkpoints**: Listens to the network until it observes a consensus
  hash for a checkpoint ledger, and then verifies the entire earlier history
  of an archive that ends in that ledger hash, writing the output to a reference
  list of trusted checkpoint hashes.
  Option **--output-filename <FILE-NAME>** is mandatory and specifies the file
  to write the trusted checkpoint hashes to.
* **version**: Print version info and then exit.

## HTTP Commands
By default stellar-core listens for connections from localhost on port 11626. 
You can send commands to stellar-core via a web browser, curl, or using the --c 
command line option (see above). Most commands return their results in JSON
format.

* **self-check**: Perform history-related sanity checks, and it is planned
  to support other kinds of sanity checks in the future.

* **bans**
  List current active bans

* **checkdb**
  Triggers the instance to perform a background check of the database's state.

* **connect**
  `connect?peer=NAME&port=NNN`<br>
  Triggers the instance to connect to peer NAME at port NNN.

* **dropcursor**  
  `dropcursor?id=ID`<br>
  Deletes the tracking cursor identified by `id`. See `setcursor` for
  more information.

* **droppeer**
  `droppeer?node=NODE_ID[&ban=D]`<br>
  Drops peer identified by NODE_ID, when D is 1 the peer is also banned.

* **info[?compact=true]**
  Returns information about the server in JSON format (sync state, connected
  peers, etc). When `compact` is set to `false`, adds additional information

* **ll**  
  `ll?level=L[&partition=P]`<br>
  Adjust the log level for partition P where P is one of Bucket, Database, Fs,
  Herder, History, Ledger, Overlay, Process, SCP, Tx (or all if no partition is
  specified). Level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE,
  TRACE.

* **logrotate**
  Rotate log files.

* **maintenance**
  `maintenance?[queue=true]`<br>
  Performs maintenance tasks on the instance.
   * `queue` performs deletion of queue data. See `setcursor` for more information.

* **metrics**
  `metrics?[enable=PARTITION_1,PARTITION_2,...,PARTITION_N]`<br>
  Returns a snapshot of the metrics registry (for monitoring and debugging
  purpose).
  If `enable` is set, return only specified metric partitions. Partitions are either metric domain names (e.g. `scp`, `overlay`, etc) or individual metric names.

* **clearmetrics**
  `clearmetrics?[domain=DOMAIN]`<br>
  Clear metrics for a specified domain. If no domain specified, clear all
  metrics (for testing purposes).

* **peers?[&fullkeys=false&compact=true]**
  Returns the list of known peers in JSON format with some metrics.
  If `fullkeys` is set, outputs unshortened public keys.
  If `compact` is `false`, it will output extra metrics.

* **quorum**
  `quorum?[node=NODE_ID][&compact=false][&fullkeys=false][&transitive=false]`<br>
  Returns information about the quorum for `NODE_ID` (local node by default).
  If `transitive` is set, information is for the transitive quorum centered on `NODE_ID`, otherwise only for nodes in the quorum set of `NODE_ID`.

  `NODE_ID` is either a full key (`GABCD...`), an alias (`$name`) or an
  abbreviated ID (`@GABCD`).

  If `compact` is set, only returns a summary version.

  If `fullkeys` is set, outputs unshortened public keys.
  The quorum endpoint categorizes each node as following:
  * `missing`: didn't participate in the latest consensus rounds.
  * `disagree`: participating in the latest consensus rounds, but working on different values.
  * `delayed`: participating in the latest consensus rounds, but slower than others.
  * `agree`: running just fine.

* **setcursor**
  `setcursor?id=ID&cursor=N`<br>
  Sets or creates a cursor identified by `ID` with value `N`. ID is an
  uppercase AlphaNum, N is an uint32 that represents the last ledger sequence
  number that the instance ID processed. Cursors are used by dependent services
  to tell stellar-core which data can be safely deleted by the instance. The
  data is historical data stored in the SQL tables such as txhistory or
  ledgerheaders. When all consumers processed the data for ledger sequence N
  the data can be safely removed by the instance. The actual deletion is
  performed by invoking the `maintenance` endpoint or on startup. See also
  `dropcursor`.

* **getcursor**
  `getcursor?[id=ID]`<br>
  Gets the cursor identified by `ID`. If ID is not defined then all cursors
  will be returned.

* **scp**
  `scp?[limit=n][&fullkeys=false]`<br>
  Returns a JSON object with the internal state of the SCP engine for the last
  n (default 2) ledgers. Outputs unshortened public keys if fullkeys is set.

* **tx**
  `tx?blob=Base64`<br>
  Submit a transaction to the network.
  blob is a base64 encoded XDR serialized 'TransactionEnvelope', and it
  returns a JSON object with the following properties
  status:
    * "PENDING" - transaction is being considered by consensus
    * "DUPLICATE" - transaction is already PENDING
    * "ERROR" - transaction rejected by transaction engine
        error: set when status is "ERROR".
            Base64 encoded, XDR serialized 'TransactionResult'

* **upgrades**
  * `upgrades?mode=get`<br>
    Retrieves the currently configured upgrade settings.<br>
  * `upgrades?mode=clear`<br>
    Clears any upgrade settings.<br>
  * `upgrades?mode=set&upgradetime=DATETIME&[basefee=NUM]&[basereserve=NUM]&[maxtxsetsize=NUM]&[protocolversion=NUM]`<br>
    * `upgradetime` is a required date (UTC) in the form `1970-01-01T00:00:00Z`. 
        It is the time the upgrade will be scheduled for. If it is in the past
        by less than 12 hours, the upgrade will occur immediately. If it's more
        than 12 hours, then the upgrade will be ignored<br>
    * `basefee` (uint32) This is what you would prefer the base fee to be. It is in
        stroops<br>
    * `basereserve` (uint32) This is what you would prefer the base reserve to
        be. It is in stroops.<br>
    * `maxtxsetsize` (uint32) This defines the maximum number of operations in 
        the transaction set to include in a ledger. When too many transactions 
        are pending, surge pricing is applied. The instance picks the 
        transactions from the transaction queue locally to be considered in the 
        next ledger until at most `maxtxsetsize` operations are accumulated.
        Transactions are ordered by fee per operation (transactions with lower 
        operation fees are held for later)
        <br>
    * `protocolversion` (uint32) defines the protocol version to upgrade to.
        When specified it must match one of the protocol versions supported
        by the node and should be greater than ledgerVersion from the current
        ledger<br>

* **surveytopology**
  `surveytopology?duration=DURATION&node=NODE_ID`<br>
  Starts a survey that will request peer connectivity information from nodes
  in the backlog. `DURATION` is the number of seconds this survey will run
  for, and `NODE_ID` is the public key you will add to the backlog to survey.
  Running this command while the survey is running will add the node to the
  backlog and reset the timer to run for `DURATION` seconds. By default, this
  node will respond to/relay a survey message if the message originated 
  from a node in it's transitive quorum. This behaviour can be overridden by adding 
  keys to `SURVEYOR_KEYS` in the config file, which will be the set of keys to check
  instead of the transitive quorum. If you would like to opt-out of this survey mechanism,
  just set `SURVEYOR_KEYS` to `$self` or a bogus key

* **stopsurvey**
  `stopsurvey`<br>
  Will stop the survey if one is running. Noop if no survey is running

* **getsurveyresult**
  `getsurveyresult`<br>
  Returns the current survey results. The results will be reset every time a new survey
  is started

### The following HTTP commands are exposed on test instances
* **generateload** `generateload[?mode=
    (create|pay|pretend|mixed_txs)&accounts=N&offset=K&txs=M&txrate=R&batchsize=L&spikesize=S&spikeinterval=I&maxfeerate=F&skiplowfeetxs=(0|1)&dextxpercent=D]`

    Artificially generate load for testing; must be used with
    `ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING` set to true.
  * `create` mode creates new accounts. Additionally, allows batching up to 100
    account creations per transaction via `batchsize`.
  * `pay` mode generates `PaymentOp` transactions on accounts specified
    (where the number of accounts can be offset).
  * `pretend` mode generates transactions on accounts specified(where the number
    of accounts can be offset). Operations in `pretend` mode are designed to
    have a realistic size to help users "pretend" that they have real traffic.
    You can add optional configs `LOADGEN_OP_COUNT_FOR_TESTING` and
    `LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING` in the config file to specify
    the # of ops / tx and how often they appear. More specifically, the
    probability that a transaction contains `COUNT[i]` ops is `DISTRIBUTION
    [i] / (DISTRIBUTION[0] + DISTRIBUTION[1] + ...)`.
  * `mixed_txs` mode generates a mix of DEX and non-DEX transactions
    (containing `PaymentOp` and `ManageBuyOfferOp` operations respectively).
    The fraction of DEX transactions generated is defined by the `dextxpercent`
    parameter (accepts integer value from 0 to 100).

  Non-`create` load generation makes use of the additional parameters:
  * when a nonzero `spikeinterval` is given, a spike will occur every
    `spikeinterval` seconds injecting `spikesize` transactions on top of
    `txrate`
  * `maxfeerate` defines the maximum per-operation fee for generated
    transactions (when not specified only minimum base fee is used)
  * when `skiplowfeetxs` is set to `true` the transactions that are not accepted by
    the node due to having too low fee to pass the rate limiting are silently
    skipped. Otherwise (by default), such transactions would cause load generation to fail.

* **manualclose**
  If MANUAL_CLOSE is set to true in the .cfg file, this will cause the current
  ledger to close. If MANUAL_CLOSE is set to false, allows a validating node
  that is waiting to hear about consensus from the network to force ledger close,
  and start a new consensus round.

* **testacc**
  `testacc?name=N`<br>
  Returns basic information about the account identified by name. Note that N
  is a string used as seed, but "root" can be used as well to specify the root
  account used for the test instance.

* **testtx**
  `testtx?from=F&to=T&amount=N&[create=true]`<br>
  Injects a payment transaction (or a create transaction if "create" is
  specified) from the account F to the account T, sending N XLM to the account.
  Note that F and T are seed strings but can also be specified as "root" as
  shorthand for the root account for the test instance.
