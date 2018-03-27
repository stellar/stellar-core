---
title: Commands
---

stellar-core can be controlled via the following commands.

## Command line options
* **--?** or **--help**: Print the available command line options and then exit..
* **--c** Send an [HTTP command](#http-commands) to an already running local instance of stellar-core and then exit. For example: 

`$ stellar-core -c info`

* **--conf FILE**: Specify a config file to use. You can use '-' and provide the config file via STDIN. *default 'stellar-core.cfg'*
* **--convertid ID**: Will output the passed ID in all known forms and then exit. Useful for determining the public key that corresponds to a given private key. For example:

`$ stellar-core --convertid SDQVDISRYN2JXBS7ICL7QJAEKB3HWBJFP2QECXG7GZICAHBK4UNJCWK2`

* **--dumpxdr FILE**:  Dumps the given XDR file and then exits.
* **--loadxdr FILE**:  Load an XDR bucket file, for testing.
* **--forcescp**: This command is used to start a network from scratch or when a 
network has lost quorum because of failed nodes or otherwise. It sets a flag in 
the database. The next time stellar-core is run, stellar-core will start 
emitting SCP messages based on its last known ledger. Without this flag stellar-core waits to hear
a ledger close from the network before starting SCP.<br>
forcescp doesn't change the requirements for quorum so although this node will emit SCP messages SCP won't complete until there are also a quorum of other nodes also emitting SCP messages on this same ledger.
* **--fuzz FILE**: Run a single fuzz input and exit.
* **--genfuzz FILE**:  Generate a random fuzzer input file.
* **--genseed**: Generate and print a random public/private key and then exit.
* **--inferquorum**:   Print a potential quorum set inferred from history.
* **--checkquorum**:   Check quorum intersection from history to ensure there is closure over all the validators in the network.
* **--graphquorum**:   Print a quorum set graph from history.
* **--offlineinfo**: Returns an output similar to `--c info` for an offline instance
* **--ll LEVEL**: Set the log level. It is redundant with `--c ll` but we need this form if you want to change the log level during test runs.
* **--metric METRIC**: Report metric METRIC on exit. Used for gathering a metric cumulatively during a test run.
* **--newdb**: Clears the local database and resets it to the genesis ledger. If you connect to the network after that it will catch up from scratch. 
* **--newhist ARCH**:  Initialize the named history archive ARCH. ARCH should be one of the history archives you have specified in the stellar-core.cfg. This will write a `.well-known/stellar-history.json` file in the archive root.
* **--printtxn FILE**:  Pretty-print a binary file containing a
  `TransactionEnvelope`.  If FILE is "-", the transaction is read from
  standard input.
* **--signtxn FILE**:  Add a digital signature to a transaction
  envelope stored in binary format in FILE, and send the result to
  standard output (which should be redirected to a file or piped
  through a tool such as `base64`).  The private signing key is read
  from standard input, unless FILE is "-" in which case the
  transaction envelope is read from standard input and the signing key
  is read from `/dev/tty`.  In either event, if the signing key
  appears to be coming from a terminal, stellar-core disables echo.
  Note that if you do not have a STELLAR_NETWORK_ID environment
  variable, then before this argument you must specify the --netid
  option.
* **--base64**: When preceding --printtxn or --signtxn, alters the
  behavior of the option to work on base64-encoded XDR rather than
  raw XDR.
* **--sec2pub**:  Reads a secret key on standard input and outputs the
  corresponding public key.  Both keys are in Stellar's standard
  base-32 ASCII format.
* **--netid STRING**:  The --signtxn option requires a particular
  network to sign for.  For example, the production stellar network is
  "`Public Global Stellar Network ; September 2015`" while the test
  network is "`Test SDF Network ; September 2015`".
* **--test**: Run all the unit tests. For [further info](https://github.com/philsquared/Catch/blob/master/docs/command-line.md) on possible options for test. For example this will run just the "Herder" tests and stop after the first failure: `stellar-core --test -a [Herder]` 
* **--version**: Print version info and then exit.


## HTTP Commands
By default stellar-core listens for connections from localhost on port 11626. 
You can send commands to stellar-core via a web browser, curl, or using the --c 
command line option (see above). Most commands return their results in JSON format.

* **help**
  Prints a list of currently supported commands.

* **catchup** 
  `/catchup?ledger=NNN[&mode=MODE]`<br>
  Triggers the instance to catch up to ledger NNN from history;
  Mode is either 'minimal' (the default, if omitted) or 'complete'.

* **checkdb**
  Triggers the instance to perform a background check of the database's state.

* **checkpoint**
  Triggers the instance to write an immediate history checkpoint. And uploads it to the archive.

* **connect**
  `/connect?peer=NAME&port=NNN`<br>
  Triggers the instance to connect to peer NAME at port NNN.

* **dropcursor**  
  `/dropcursor?id=XYZ`<br>
   deletes the tracking cursor with identified by `id`. See `setcursor` for more information.

* **info**
  Returns information about the server in JSON format (sync
  state, connected peers, etc).

* **ll**  
  `/ll?level=L[&partition=P]`<br>
  Adjust the log level for partition P where P is one of Bucket, Database, Fs, Herder, History, Ledger, Overlay, Process, SCP, Tx (or all if no partition is specified).
  Level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE

* **maintenance**
 `/maintenance?[queue=true]`<br>
  Performs maintenance tasks on the instance.
   * `queue` performs deletion of queue data. See `setcursor` for more information.

* **metrics**
 Returns a snapshot of the metrics registry (for monitoring and
debugging purpose).

* **clearmetrics**
 `/clearmetrics?[domain=DOMAIN]`<br>
  Clear metrics for a specified domain. If no domain specified, clear all metrics (for testing purposes).

* **peers**
  Returns the list of known peers in JSON format.

* **quorum**
  `/quorum?[node=NODE_ID][&compact=true]`<br>
  returns information about the quorum for node NODE_ID (this node by default).
  NODE_ID is either a full key (`GABCD...`), an alias (`$name`) or
  an abbreviated ID (`@GABCD`).
  If compact is set, only returns a summary version.

* **setcursor**
 `/setcursor?id=ID&cursor=N`<br>
  sets or creates a cursor identified by `ID` with value `N`. ID is an uppercase AlphaNum, N is an uint32 that represents the last ledger sequence number that the instance ID processed.
  Cursors are used by dependent services to tell stellar-core which data can be safely deleted by the instance.
  The data is historical data stored in the SQL tables such as txhistory or ledgerheaders. When all consumers processed the data for ledger sequence N the data can be safely removed by the instance.
  The actual deletion is performed by invoking the `maintenance` endpoint or on startup.
  See also `dropcursor`.

* **getcursor**
 `/getcursor?[id=ID]`<br>
 gets the cursor identified by `ID`. If ID is not defined then all cursors will be returned.

* **scp**
  `/scp?[limit=n]
  Returns a JSON object with the internal state of the SCP engine for the last n (default 2) ledgers.

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
  upgradetime is a required date (UTC) in the form 1970-01-01T00:00:00Z.<br>
    * fee (uint32) This is what you would prefer the base fee to be. It is
        in stroops<br>
    * basereserve (uint32) This is what you would prefer the base reserve 
        to be. It is in stroops.<br>
    * maxtxsize (uint32) This defines the maximum number of transactions 
        to include in a ledger. When too many transactions are pending, 
        surge pricing is applied. The instance picks the top maxtxsize
         transactions locally to be considered in the next ledger. Where 
        transactions are ordered by transaction fee(lower fee transactions
         are held for later).<br>
    * protocolversion (uint32) defines the protocol version to upgrade to.
         When specified it must match the protocol version supported by the
        node<br>

### The following HTTP commands are exposed on test instances
* **generateload**
  `/generateload[?accounts=N&txs=M&txrate=(R|auto)]`<br>
  Artificially generate load for testing; must be used with `ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING` set to true.

* **manualclose**
  If MANUAL_CLOSE is set to true in the .cfg file. This will cause the current ledger to close.

* **testacc**
 `/testacc?name=N`<br>
 Returns basic information about the account identified by name. Note that N is a string used as seed, but "root" can be used as well to specify the root account used for the test instance.

* **testtx**
 `/testtx?from=F&to=T&amount=N&[create=true]`<br>
  Injects a payment transaction (or a create transaction if "create" is specified) from the account F to the account T, sending N XLM to the account.
  Note that F and T are seed strings but can also be specified as "root" as a shorthand for the root account for the test instance.
