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

* **apply-load**: Applies Soroban transactions by repeatedly generating transactions and closing
them directly through the LedgerManager. This command will generate enough transactions to fill up a synthetic transaction queue (it's just a list of transactions with the same limits as the real queue), and then create a transaction set off of that to
apply.

* At the moment, the Soroban transactions are generated using some of the same config parameters as the **generateload** command. Specifically,
    `ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true`,
    `LOADGEN_INSTRUCTIONS_FOR_TESTING`, and
    `LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING`. In addition to those, you must also set the
    limit related settings - `APPLY_LOAD_LEDGER_MAX_INSTRUCTIONS`, `APPLY_LOAD_TX_MAX_INSTRUCTIONS`, `APPLY_LOAD_LEDGER_MAX_READ_LEDGER_ENTRIES`, `APPLY_LOAD_TX_MAX_READ_LEDGER_ENTRIES`, `APPLY_LOAD_LEDGER_MAX_WRITE_LEDGER_ENTRIES`, `APPLY_LOAD_TX_MAX_WRITE_LEDGER_ENTRIES`, `APPLY_LOAD_LEDGER_MAX_READ_BYTES`, `APPLY_LOAD_TX_MAX_READ_BYTES`, `APPLY_LOAD_LEDGER_MAX_WRITE_BYTES`, `APPLY_LOAD_TX_MAX_WRITE_BYTES`, `APPLY_LOAD_MAX_TX_SIZE_BYTES`, `APPLY_LOAD_MAX_LEDGER_TX_SIZE_BYTES`, `APPLY_LOAD_MAX_CONTRACT_EVENT_SIZE_BYTES`, `APPLY_LOAD_MAX_TX_COUNT`.
* `apply-load` will also generate a synthetic bucket list using `APPLY_LOAD_BL_SIMULATED_LEDGERS`, `APPLY_LOAD_BL_WRITE_FREQUENCY`, `APPLY_LOAD_BL_BATCH_SIZE`, `APPLY_LOAD_BL_LAST_BATCH_LEDGERS`, `APPLY_LOAD_BL_LAST_BATCH_SIZE`. These have default values set in `Config.h`.
* There are additional `APPLY_LOAD_*` related config settings that can be used to configure
`apply-load`, and you can learn more about these from the comments in `Config.h`.

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
* **check-quorum-intersection <FILE-NAME>** checks that a given network
  specified as a JSON file enjoys a quorum intersection. The JSON file must
  match the output format of the `quorum` HTTP endpoint with the `transitive`
  and `fullkeys` flags set to `true`. Unlike many other commands, omitting
  `--conf` specifies that a configuration file should not be used (that is,
  `--conf` does not default to `stellar-core.cfg`). `check-quorum-intersection`
  uses the config file only to produce human readable node names in its output,
  so the option can be safely omitted if human readable node names are not
  necessary.
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
* **dump-archival-stats**:  Logs state archival statistics about the BucketList.
* **encode-asset**: Prints a base-64 encoded asset built from  `--code <CODE>` and `--issuer <ISSUER>`. Prints the native asset if neither `--code` nor `--issuer` is given.
* **fuzz <FILE-NAME>**: Run a single fuzz input and exit.
* **gen-fuzz <FILE-NAME>**:  Generate a random fuzzer input file.
* **gen-seed**: Generate and print a random public/private key and then exit.
* **get-settings-upgrade-txs <PUBLIC-KEY> <SEQ-NUM> <NETWORK-PASSPHRASE>**: Generates the three transactions needed to propose
  a Soroban Settings upgrade from scratch, as will as the XDR `ConfigUpgradeSetKey` to submit to the `upgrades` endpoint. The results will be dumped to standard output. <PUBLIC-KEY> is the key that will be used as the source account on the transactions.
  <SEQ-NUM> is the current sequence number of the Stellar account corresponding to <PUBLIC-KEY>. 

  Option (required) **--xdr** takes a base64 encoded XDR serialized `ConfigUpgradeSet`.
    Example:
  `stellar-core get-settings-upgrade-txs GAUQW73V52I2WLIPKCKYXZBHIYFTECS7UPSG4OSVUHNDXEZJJWFXZG56 73014444032  "Standalone Network ; February 2017" --xdr AAAAAQAAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE0gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA= --signtxs`.<br>

  Option **--signtxs** will prompt for a secret key and sign the TransactionEnvelopes.<br>

  Output format by line - 
  1. Base64 wasm restore tx envelope XDR
  2. Hex tx ID for the wasm restore tx.
  3. Base64 upload tx envelope XDR
  4. Hex tx ID for the upload tx.
  5. Base 64 create tx envelope XDR
  6. Hex tx ID for the create tx.
  7. Base64 invoke tx envelope XDR
  8. Hex tx ID for the invoke tx.
  9. Base64 ConfigUpgradeSetKey XDR.

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
* **replay-debug-meta**: Replay state from debug meta available. This requires
  core to be at an appropriate LCL to replay debug ledgers on top.
  Option **--history-ledger** allows to specify target ledger.
  Option **--meta-dir** is a (required) path to `meta-debug` directory, which
  contains meta to replay by this command.
* **report-last-history-checkpoint**: Download and report last history
  checkpoint from a history archive.
* **run**: Runs stellar-core service.<br>
  Option **--wait-for-consensus** lets validators wait to hear from the network
  before participating in consensus.<br>
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
* **soroban-info**: Returns Soroban specific settings. If the **--format** option
  is not specified, then the command will return JSON by default.
  Option **--format [basic|detailed]** : The `basic` option will return a subset of
  the settings (no cost types or non-configurable settings) in a readable JSON format. The `detailed`
  option will return every stored `ConfigSettingEntry` XDR struct in `cereal` deserialized JSON.

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
  * Option **--history-hash <HASH>** is optional and specifies the hash of the ledger
  at the end of the verification range. When provided, `stellar-core` will use the history
  hash to verify the range, rather than the latest checkpoint hash obtained from consensus.
  Used in conjunction with `--history-ledger`.
  * Option **--history-ledger <LEDGER-NUMBER>** is optional and specifies the ledger
  number to end the verification at.  Used in conjunction with `--history-hash`.
  * Option **--output-filename <FILE-NAME>** is mandatory and specifies the file
  to write the trusted checkpoint hashes to. The file will contain a JSON array 
  of arrays, where each inner array contains the ledger number and the corresponding
  checkpoint hash of the form `[[999, "hash-abc"], [935, "hash-def"], ... [0, "hash-xyz]]`.
  * Option **--trusted-hash-file <FILE-NAME>** is optional. If provided,
  stellar-core will parse the latest checkpoint ledger number and hash from the file and verify from this ledger to the latest checkpoint ledger obtained from the network.
  * Option **--from-ledger <LEDGER-NUMBER>** is optional and specifies the ledger
  number to start the verification from.

> Note: It is an error to provide both the `--trusted-hash-file` and `--from-ledger` options.

* **version**: Print version info and then exit.

## HTTP Commands
Stellar-core maintains two HTTP servers, a command server and a query server.
The command endpoint listens for operator commands listed below, while the query
server is appropriate for high throughput queries regarding current ledger state.
By default the stellar-core command endpoint listens for connections from localhost
on port 11626. By default the query endpoint is disabled. You can send commands to
stellar-core via a web browser, curl, or using the --c command line option (see above).
Most commands return their results in JSON format.

* **self-check**: Perform history-related sanity checks, and it is planned
  to support other kinds of sanity checks in the future.

* **bans**
  List current active bans

* **checkdb**
  Triggers the instance to perform a background check of the database's state.

* **connect**
  `connect?peer=NAME&port=NNN`<br>
  Triggers the instance to connect to peer NAME at port NNN.

* **droppeer**
  `droppeer?node=NODE_ID[&ban=D]`<br>
  Drops peer identified by NODE_ID, when D is 1 the peer is also banned.

* **unban**
  `unban?node=NODE_ID[&ban=D]`<br>
  Unban banned peer identified by NODE_ID.

* **info[?compact=true]**
  Returns information about the server in JSON format (sync state, connected
  peers, etc). When `compact` is set to `false`, adds additional information

* **ll**
  `ll?level=L[&partition=P]`<br>
  Adjust the log level for partition P where P is one of Fs, SCP, Bucket, Database, History, Process, Ledger, Overlay, Herder, Tx, LoadGen, Work, Invariant, Perf (or all if no partition is
  specified). Level is one of FATAL, ERROR, WARNING, INFO, DEBUG, TRACE.

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
    * "TRY_AGAIN_LATER" - transaction rejected but can be retried later. This can happen due to several reasons:
        There is another transaction from same source account in PENDING state
        The network is under high load and the fee is too low.
    * "FILTERED" - transaction rejected because it contains an operation type that Stellar Core filters out. See Stellar Core configuration `EXCLUDE_TRANSACTIONS_CONTAINING_OPERATION_TYPE` for more details.

* **upgrades**
  * `upgrades?mode=get`<br>
    Retrieves the currently configured upgrade settings.<br>
  * `upgrades?mode=clear`<br>
    Clears any upgrade settings.<br>
  * `upgrades?mode=set&upgradetime=DATETIME&[basefee=NUM]&[basereserve=NUM]&[maxtxsetsize=NUM]&[protocolversion=NUM]&[configupgradesetkey=ConfigUpgradeSetKey]`<br>
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
    * `maxsorobantxsetsize` (uint32) This defines the maximum number of Soroban
       operations in the transaction set to include in a ledger. The semantics is
       the same as for `maxtxsetsize`, but this affects the Soroban slice of traffic.
       <br>
    * `protocolversion` (uint32) defines the protocol version to upgrade to.
        When specified it must match one of the protocol versions supported
        by the node and should be greater than ledgerVersion from the current
        ledger<br>
    * `configupgradesetkey` (base64 encoded XDR serialized `ConfigUpgradeSetKey`)
        this key will be converted to a ContractData LedgerKey, and the
        ContractData LedgerEntry retrieved with that will have a val of SCV_BYTES
        containing a serialized ConfigUpgradeSet. Each ConfigSettingEntry in the
        ConfigUpgradeSet will be used to update the existing network ConfigSettingEntry
        that exists at the corresponding CONFIG_SETTING LedgerKey.

* **sorobaninfo**
  `sorobaninfo?[format=basic,detailed,upgrade_xdr]`
    Retrieves the current Soroban settings in different formats.

    * `basic` is the default if the `format` parameter is not specified. It
      will dump a subset of the Soroban settings in an easy to read format.
    * `detailed` will insert every setting into a `ConfigUpgradeSet` and dump
      it in the same format as the **dumpproposedsettings** command, which lets
      a user easily compare the existing settings against a proposal.
    * `upgrade_xdr` will insert the current upgradeable settings into a `ConfigUpgradeSet`
      and dump it as base64 xdr. This can be used along with the `stellar-xdr` command line tool
      to dump the current settings in the same format as the JSON file we use for upgrades. This
      is helpful if you want to make settings changes off of the current settings.
      Ex. `curl -s "127.0.0.1:11626/sorobaninfo?format=upgrade_xdr" | stellar-xdr decode --type ConfigUpgradeSet --output json-formatted`

* **dumpproposedsettings**
  `dumpproposedsettings?blob=Base64`<br>
  blob is a base64 encoded XDR serialized `ConfigUpgradeSetKey`.
  This command outputs the `ConfigUpgradeSet` (if it's valid and it exists) in a readable format
  that corresponds to the `ConfigUpgradeSetKey` passed in. This can be used by validators
  to verify Soroban Settings upgrades before voting on them. Use this along with the 
  `sorobaninfo?format=detailed` command to compare against the existing settings to see exactly
  what is changing.

* **surveytopology**
  `surveytopology?duration=DURATION&node=NODE_ID`<br>
  **This command is deprecated and will be removed in a future release. Use the
  new time sliced survey interface instead (`startsurveycollecting`,
  `stopsurveycollecting`, `surveytopologytimesliced`, and `getsurveyresults`).**
  Starts a survey that will request peer connectivity information from nodes
  in the backlog. `DURATION` is the number of seconds this survey will run
  for, and `NODE_ID` is the public key you will add to the backlog to survey.
  Running this command while the survey is running will add the node to the
  backlog and reset the timer to run for `DURATION` seconds.  See [Changing
  default survey behavior](#changing-default-survey-behavior) for details about
  the default survey behavior, as well as how to change that behavior or opt-out
  entirely.

* **stopsurvey**
  `stopsurvey`<br>
  **This command is deprecated and will be removed in a future release. It is no
  longer necessary to explicitly stop a survey in the new time sliced survey
  interface as these surveys expire automatically.**
  Will stop the survey if one is running. Noop if no survey is running

* **startsurveycollecting**
  `startsurveycollecting?nonce=NONCE`<br>
  Start a survey in the collecting phase with a given nonce. Does nothing if a
  survey is already running on the network as only one survey may run at a time.
  See [Changing default survey behavior](#changing-default-survey-behavior) for
  details about the default survey behavior, as well as how to change that
  behavior or opt-out entirely.

* **stopsurveycollecting**
  `stopsurveycollecting`<br>
  Stop the collecting phase of the survey started in the previous
  `startsurveycollecting` command. Moves the survey into the reporting phase.
  Does nothing if no survey is running, or if a different node is running the
  active survey.

* **surveytopologytimesliced**
  `surveytopologytimesliced?node=NODE_ID&inboundpeerindex=INBOUND_INDEX&outboundpeerindex=OUTBOUND_INDEX`<br>
  During the reporting phase of a survey, invoke this command to request
  information recorded during the collecting phase from `NODE_ID`. This command
  adds the survey request to a backlog; it does not immediately send the
  request. Use `getsurveyresult` to see the response. A response will include
  information about up to 25 inbound and outbound peers respectively. If a node
  has more than 25 inbound and/or outbound peers, you will need to survey the
  node multiple times to get the complete peer list. You can request peers
  starting from a specific index in each peer list by setting `INBOUND_INDEX`
  and `OUTBOUND_INDEX` appropriately.  See [Changing default survey
  behavior](#changing-default-survey-behavior) for details about the default
  survey behavior, as well as how to change that behavior or opt-out entirely.

* **getsurveyresult**
  `getsurveyresult`<br>
  Returns the current survey results. The results will be reset every time a new survey
  is started. Use this command for both the time sliced survey interface as well
  as the old deprecated survey interface.

#### Changing default survey behavior
By default, this node will respond to/relay a survey message if the message
originated from a node in its transitive quorum. This behavior can be overridden
by adding keys to `SURVEYOR_KEYS` in the config file, which will be the set of
keys to check instead of the transitive quorum. If you would like to opt-out of
this survey mechanism, just set `SURVEYOR_KEYS` to `$self` or a bogus key

### The following HTTP commands are exposed on test instances
* **generateload** `generateload[?mode=
    (create|pay|pretend|mixed_classic|soroban_upload|soroban_invoke_setup|soroban_invoke|upgrade_setup|create_upgrade|mixed_classic_soroban|stop)&accounts=N&offset=K&txs=M&txrate=R&spikesize=S&spikeinterval=I&maxfeerate=F&skiplowfeetxs=(0|1)&dextxpercent=D&minpercentsuccess=S&instances=Y&wasms=Z&payweight=P&sorobanuploadweight=Q&sorobaninvokeweight=R]`

    Artificially generate load for testing; must be used with
    `ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING` set to true.
  * `create` mode creates new accounts. Batches 100 creation operations per transaction.
  * `pay` mode generates `PaymentOp` transactions on accounts specified
    (where the number of accounts can be offset).
  * `pretend` mode generates transactions on accounts specified(where the number
    of accounts can be offset). Operations in `pretend` mode are designed to
    have a realistic size to help users "pretend" that they have real traffic.
    You can add optional configs `LOADGEN_OP_COUNT_FOR_TESTING` and
    `LOADGEN_OP_COUNT_DISTRIBUTION_FOR_TESTING` in the config file to specify
    the # of ops / tx and how often they appear. See the section on [specifying
    discrete distributions](#specifying-discrete-distributions) for more info
    on how to set these options.
  * `mixed_classic` mode generates a mix of DEX and non-DEX transactions
    (containing `PaymentOp` and `ManageBuyOfferOp` operations respectively).
    The fraction of DEX transactions generated is defined by the `dextxpercent`
    parameter (accepts integer value from 0 to 100).
  * `soroban_upload` mode generates soroban TXs that upload random wasm blobs.
    Many of these TXs are invalid and not applied, so this test is appropriate
    for herder and overlay tests. This mode allows specification of the
    distribution it samples wasm sizes from via the
    `LOADGEN_WASM_BYTES_FOR_TESTING` and
    `LOADGEN_WASM_BYTES_DISTRIBUTION_FOR_TESTING` config file options.  See the
    section on [specifying discrete
    distributions](#specifying-discrete-distributions) for more info on how to
    set these parameters.
  * `soroban_invoke_setup` mode create soroban contract instances to be used by
    `soroban_invoke`. This mode must be run before `soroban_invoke` or
    `mixed_classic_soroban`.
  * `soroban_invoke` mode generates valid soroban TXs that invoke a resource
    intensive contract. Each invocation picks a random amount of resources
    between some bound. Resource distributions can be set with the
    `LOADGEN_NUM_DATA_ENTRIES_FOR_TESTING`,
    `LOADGEN_NUM_DATA_ENTRIES_DISTRIBUTION_FOR_TESTING`,
    `LOADGEN_IO_KILOBYTES_FOR_TESTING`,
    `LOADGEN_IO_KILOBYTES_DISTRIBUTION_FOR_TESTING`,
    `LOADGEN_TX_SIZE_BYTES_FOR_TESTING`,
    `LOADGEN_TX_SIZE_BYTES_DISTRIBUTION_FOR_TESTING`,
    `LOADGEN_INSTRUCTIONS_FOR_TESTING`, and
    `LOADGEN_INSTRUCTIONS_DISTRIBUTION_FOR_TESTING` config file options.
    `*KILOBYTES*` values indicate the total amount of disk IO generated TXs
    require.  See the section on [specifying discrete
    distributions](#specifying-discrete-distributions) for more info on how to
    set these parameters.  `instances` and `wasms` parameters determine how
    many unique contract instances and wasm entries will be used.
  * `upgrade_setup` mode create soroban contract instance to be used by
    `create_upgrade`. This mode must be run before `create_upgrade`.
  * `create_upgrade` mode write a soroban upgrade set and returns the
    ConfigUpgradeSetKey. Most network config settings are supported. If a given
    setting is omitted or set to 0, it is not upgraded and maintains the current
    value. To not exceed HTTP string limits, the names are very short. See
    `CommandHandler::generateLoad` for available options.
  * `mixed_classic_soroban` mode creates a mix of `pay`, `soroban_upload`,
    and `soroban_invoke` load. It accepts all of the options those modes
    accept, plus `payweight`, `sorobanuploadweight`, and `sorobaninvokeweight`.
    These `weight` parameters determine the distribution of `pay`,
    `soroban_upload`, and `soroban_invoke` load with the likelihood of any
    generated transaction falling into each mode being determined by the mode's
    weight divided by the sum of all weights.
  * `stop` mode stops any existing load generation run and marks it as "failed".

  Non-`create` load generation makes use of the additional parameters:
  * when a nonzero `spikeinterval` is given, a spike will occur every
    `spikeinterval` seconds injecting `spikesize` transactions on top of
    `txrate`
  * `maxfeerate` defines the maximum per-operation fee for generated
    transactions (when not specified only minimum base fee is used)
  * when `skiplowfeetxs` is set to `true` the transactions that are not accepted by
    the node due to having too low fee to pass the rate limiting are silently
    skipped. Otherwise (by default), such transactions would cause load generation to fail.

  Soroban load generation also makes use of the `minpercentsuccess` parameter,
  which determines the minimum percentage of Soroban transactions that must
  succeed at apply time for load generation to be considered successful. This
  parameter defaults to `0`. Note that a `0` value does not mean that load
  generation performs no checks whatsoever; load generation will still check
  that generated transactions make it into a block by checking sequence
  numbers, but will not check that those transactions successfully apply. There
  are two main reasons a generated transaction may not succeed at apply time:
  1. The provided distributions generate transactions that are over the
     network transaction or ledger limits.
  2. Load generation may underestimate the resources needed to apply the
     transaction. This happens occasionally as load generation does not
     preflight transactions, but rather uses heuristics to estimate resource
     requirements.

  When distributions are well within network limits and load is composed
  primarily of invoke transactions, `minpercentsuccess` should be set fairly
  high (upwards of 80%). Otherwise, it should be set low (0-50%).

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

#### Specifying Discrete Distributions

Certain config file options for `generateload` mode support the specification
of discrete distributions to sample from. For each parameter `X`, there are two
options `LOADGEN_X_FOR_TESTING` and `LOADGEN_X_DISTRIBUTION_FOR_TESTING` that
describe the shape of the distribution for `X`. Each of these options takes a
list of values where `LOADGEN_X_FOR_TESTING` holds the values that may be
sampled and `LOADGEN_X_DISTRIBUTION_FOR_TESTING` holds the weights of each
value.  The probability that `LOADGEN_X_FOR_TESTING[i]` is sampled is
`LOADGEN_X_DISTRIBUTION_FOR_TESTING[i]/sum(LOADGEN_X_DISTRIBUTION_FOR_TESTING)`
for each `i`.

## HTTP Query Server

The query server is appropriate for high throughput queries regarding current
ledger state. By default the stellar-core command endpoint listens for connections
from localhost on port 11626. By default the query endpoint is disabled, but can be
enabled by specifying a port via the HTTP_QUERY_PORT config setting.

### Operator Config Settings

* **HTTP_QUERY_PORT**<br>
  The port on which stellar-core will listen for query requests. By default, this
  port is set to 0, which disables the query server.
* **QUERY_THREAD_POOL_SIZE**<br>
  Specifies the number of threads to dedicate to servicing queries. This is dependent
  on what hardwarde stellar-core is running on, as well as the latency and throughput
  requirements of downstream systems using this endpoint.
* **QUERY_SNAPSHOT_LEDGERS**<br>
  The `getledgerentryraw` endpoint is capable of returning state based on historical
  ledger snapshots. In particular, the query server will retain ledger snapshots from
  `[currLedgerSeq - QUERY_SNAPSHOT_LEDGERS, currLedgerSeq]`. Note that storing many
  snapshots will lead to decreased performance and increased memory usage. Additionally,
  these snapshots are "best effort" and not persisted on restart. If a downstream system
  requires large ammounts of historical state, or requires persistent historical state,
  it is recommended that a Horizon instance is used instead.

### Query Commands

* **`getledgerentryraw`**<br>
  A POST request with the following body:<br>

  ```
  ledgerSeq=NUM&key=Base64&key=Base64...
  ```

  * `ledgerSeq`: An optional parameter, specifying the ledger snapshot to base the query on.
  If the specified ledger in not available, a 404 error will be returned. If this parameter
  is not set, the current ledger is used.
  * `key`: A series of Base64 encoded XDR strings specifying the `LedgerKey` to query.

  A JSON payload is returned as follows:

  ```
  {
    "entries": [
      {"le": "Base64-LedgerEntry"},
      {"le": "Base64-LedgerEntry"},
      ...
    ],
    "ledgerSeq": ledgerSeq
  }
  ```

  For each `LedgerKey` that exists in the current ledger, the corresponding `LedgerEntry` Base64
  XDR string is returned ("le"). If a given `LedgerKey` does not exist in the BucketList, it is
  omitted from the return body. Note that this is a "raw" ledger interface that does not reason about
  State Archival. In order to determine if a given entry is live or archived, it is necessary to
  load both the `CONTRACT_DATA` entry as well as it's associated `TTL` entry, then compare the
  value of the `TTL` entry to the current ledger sequence number.

  `ledgerSeq` gives the ledger number on which the query was performed.
