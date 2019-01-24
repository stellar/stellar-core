---
title: Commands
---

stellar-core can be controlled via the following commands.

## Common options
Common options can be placed at any place in the command line.

* **--conf <FILE-NAME>**: Specify a config file to use. You can use '-' and
  provide the config file via STDIN. *default 'stellar-core.cfg'*
* **--ll <LEVEL>**: Set the log level. It is redundant with `http-command ll`
  but we need this form if you want to change the log level during test runs.
* **--metric <METRIC-NAME>**: Report metric METRIC on exit. Used for gathering
  a metric cumulatively during a test run.
* **--help**: Show help message for given command.

## Command line options
Command options can only by placed after command.

* **catchup <DESTINATION-LEDGER/LEDGER-COUNT>**: Perform catchup from history
  archives without connecting to network. This option will catchup to
  DESTINATION-LEDGER keeping history from DESTINATION-LEDGER-LEDGER-COUNT or
  from the last closed ledger (whichever value is biggest).
* **check-quorum**:   Check quorum intersection from history to ensure there is
  closure over all the validators in the network.
* **convert-id <ID>**: Will output the passed ID in all known forms and then
  exit. Useful for determining the public key that corresponds to a given
  private key. For example:

`$ stellar-core convert-id SDQVDISRYN2JXBS7ICL7QJAEKB3HWBJFP2QECXG7GZICAHBK4UNJCWK2`

* **dump-xdr <FILE-NAME>**:  Dumps the given XDR file and then exits.
* **force-scp**: This command is used to start a network from scratch or when a
  network has lost quorum because of failed nodes or otherwise. It sets a flag
  in the database. The next time stellar-core is run, stellar-core will start
  emitting SCP messages based on its last known ledger. Without this flag
  stellar-core waits to hear a ledger close from the network before starting
  SCP.<br> force-scp doesn't change the requirements for quorum so although
  this node will emit SCP messages SCP won't complete until there are also a
  quorum of other nodes also emitting SCP messages on this same ledger. Value
  of force-scp can be reset with --reset flag.
* **fuzz <FILE-NAME>**: Run a single fuzz input and exit.
* **gen-fuzz <FILE-NAME>**:  Generate a random fuzzer input file.
* **gen-seed**: Generate and print a random public/private key and then exit.
* **help**: Print the available command line options and then exit..
* **http-command <COMMAND>** Send an [HTTP command](#http-commands) to an
  already running local instance of stellar-core and then exit. For example: 

`$ stellar-core http-command info`

* **infer-quorum**:   Print a potential quorum set inferred from history.
* **load-xdr <FILE-NAME>**:  Load an XDR bucket file, for testing.
* **new-db**: Clears the local database and resets it to the genesis ledger. If
  you connect to the network after that it will catch up from scratch.
* **new-hist <HISTORY-LABEL> ...**:  Initialize the named history archives
  HISTORY-LABEL. HISTORY-LABEL should be one of the history archives you have
  specified in the stellar-core.cfg. This will write a
  `.well-known/stellar-history.json` file in the archive root.
* **offline-info**: Returns an output similar to `--c info` for an offline
  instance
* **print-xdr <FILE-NAME>**:  Pretty-print a binary file containing an XDR
  object. If FILE-NAME is "-", the XDR object is read from standard input.<br>
  Option --filetype [auto|ledgerheader|meta|result|resultpair|tx|txfee]**
  controls type used for printing (default: auto).<br>
  Option --base64 alters the behavior to work on base64-encoded XDR rather than
  raw XDR.
* **publish**: Execute publish of all items remaining in publish queue without
  connecting to network. May not publish last checkpoint if last closed ledger
  is on checkpoint boundary.
* **report-last-history-checkpoint**: Download and report last history
  checkpoint from a history archive.
* **run**: Runs stellar-core service.
* **sec-to-pub**:  Reads a secret key on standard input and outputs the
  corresponding public key.  Both keys are in Stellar's standard
  base-32 ASCII format. 
* **sign-transaction <FILE-NAME>**:  Add a digital signature to a transaction
  envelope stored in binary format in <FILE-NAME>, and send the result to
  standard output (which should be redirected to a file or piped through a tool
  such as `base64`).  The private signing key is read from standard input,
  unless <FILE-NAME> is "-" in which case the transaction envelope is read from
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
    `stellar-core --test -a --version 9 --version 10 "[tx]"`
* **version**: Print version info and then exit.
* **write-quorum**: Print a quorum set graph from history.

## HTTP Commands
By default stellar-core listens for connections from localhost on port 11626. 
You can send commands to stellar-core via a web browser, curl, or using the --c 
command line option (see above). Most commands return their results in JSON
format.

* **help**
  Prints a list of currently supported commands.

* **catchup** 
  `/catchup?ledger=NNN[&mode=MODE]`<br>
  Triggers the instance to catch up to ledger NNN from history;
  Mode is either 'minimal' (the default, if omitted) or 'complete'.

* **checkdb**
  Triggers the instance to perform a background check of the database's state.

* **checkpoint**
  Triggers the instance to write an immediate history checkpoint. And uploads
  it to the archive.

* **connect**
  `/connect?peer=NAME&port=NNN`<br>
  Triggers the instance to connect to peer NAME at port NNN.

* **dropcursor**  
  `/dropcursor?id=XYZ`<br>
  deletes the tracking cursor with identified by `id`. See `setcursor` for
  more information.

* **info**
  Returns information about the server in JSON format (sync state, connected
  peers, etc).

* **ll**  
  `/ll?level=L[&partition=P]`<br>
  Adjust the log level for partition P where P is one of Bucket, Database, Fs,
  Herder, History, Ledger, Overlay, Process, SCP, Tx (or all if no partition is
  specified). Level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE,
  TRACE.

* **maintenance**
  `/maintenance?[queue=true]`<br>
  Performs maintenance tasks on the instance.
   * `queue` performs deletion of queue data. See `setcursor` for more information.

* **metrics**
  Returns a snapshot of the metrics registry (for monitoring and debugging
  purpose).

* **clearmetrics**
  `/clearmetrics?[domain=DOMAIN]`<br>
  Clear metrics for a specified domain. If no domain specified, clear all
  metrics (for testing purposes).

* **peers**
  Returns the list of known peers in JSON format.

* **quorum**
  `/quorum?[node=NODE_ID][&compact=true]`<br>
  returns information about the quorum for node NODE_ID (this node by default).
  NODE_ID is either a full key (`GABCD...`), an alias (`$name`) or an
  abbreviated ID (`@GABCD`). If compact is set, only returns a summary version.

* **setcursor**
  `/setcursor?id=ID&cursor=N`<br>
  sets or creates a cursor identified by `ID` with value `N`. ID is an
  uppercase AlphaNum, N is an uint32 that represents the last ledger sequence
  number that the instance ID processed. Cursors are used by dependent services
  to tell stellar-core which data can be safely deleted by the instance. The
  data is historical data stored in the SQL tables such as txhistory or
  ledgerheaders. When all consumers processed the data for ledger sequence N
  the data can be safely removed by the instance. The actual deletion is
  performed by invoking the `maintenance` endpoint or on startup. See also
  `dropcursor`.

* **getcursor**
  `/getcursor?[id=ID]`<br>
  gets the cursor identified by `ID`. If ID is not defined then all cursors
  will be returned.

* **scp**
  `/scp?[limit=n]`<br>
  Returns a JSON object with the internal state of the SCP engine for the last
  n (default 2) ledgers.

* **tx**
  `/tx?blob=Base64`<br>
  submit a [transaction](../../learn/concepts/transactions.md) to the network.
  blob is a base64 encoded XDR serialized 'TransactionEnvelope', and it
  returns a JSON object with the following properties
  status:
    * "PENDING" - transaction is being considered by consensus
    * "DUPLICATE" - transaction is already PENDING
    * "ERROR" - transaction rejected by transaction engine
        error: set when status is "ERROR".
            Base64 encoded, XDR serialized 'TransactionResult'

* **upgrades**
  * `/upgrades?mode=get`<br>
    retrieves the currently configured upgrade settings<br>
  * `/upgrades?mode=clear`<br>
    clears any upgrade settings<br>
  * `/upgrades?mode=set&upgradetime=DATETIME&[basefee=NUM]&[basereserve=NUM]&[maxtxsize=NUM]&[protocolversion=NUM]`<br>
    * upgradetime is a required date (UTC) in the form `1970-01-01T00:00:00Z`. 
        It is the time the upgrade will be scheduled for. If it is in the past,
        the upgrade will occur immediately.<br>
    * fee (uint32) This is what you would prefer the base fee to be. It is in
        stroops<br>
    * basereserve (uint32) This is what you would prefer the base reserve to
        be. It is in stroops.<br>
    * maxtxsize (uint32) This defines the maximum number of transactions to
        include in a ledger. When too many transactions are pending, surge
        pricing is applied. The instance picks the top maxtxsize transactions
        locally to be considered in the next ledger. Where transactions are
        ordered by transaction fee(lower fee transactions are held for later).
        <br>
    * protocolversion (uint32) defines the protocol version to upgrade to.
        When specified it must match one of the protocol versions supported
        by the node and should be greater than ledgerVersion from the current
        ledger<br>

### The following HTTP commands are exposed on test instances
* **generateload**
  `/generateload[?mode=(create|pay)&accounts=N&offset=K&txs=M&txrate=(R|auto)&batchsize=L]`<br>
  Artificially generate load for testing; must be used with
  `ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING` set to true. Depending on the mode,
  either creates new accounts or generates payments on accounts specified
  (where number of accounts can be offset). Additionally, allows batching up to
  100 account creations per transaction via 'batchsize'.

* **manualclose**
  If MANUAL_CLOSE is set to true in the .cfg file. This will cause the current
  ledger to close.

* **testacc**
  `/testacc?name=N`<br>
  Returns basic information about the account identified by name. Note that N
  is a string used as seed, but "root" can be used as well to specify the root
  account used for the test instance.

* **testtx**
  `/testtx?from=F&to=T&amount=N&[create=true]`<br>
  Injects a payment transaction (or a create transaction if "create" is
  specified) from the account F to the account T, sending N XLM to the account.
  Note that F and T are seed strings but can also be specified as "root" as
  shorthand for the root account for the test instance.
