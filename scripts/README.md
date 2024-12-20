## Description
This folder is for storing any scripts that may be helpful for using stellar-core.

## List of scripts
- [Overlay survey](#overlay-survey)
- [Diff Tracy CSV](#diff-tracy-csv)
- [Parse Backtrace Dump](#parse-backtrace-dump)
- [Histogram Generator](#histogram-generator)

### Overlay survey 
- Name - `OverlaySurvey.py`
- Description - A Python script that will walk the network using the Overlay survey mechanism to gather connection information. See [the admin guide](https://developers.stellar.org/docs/validators/admin-guide/monitoring#overlay-topology-survey) for more information on the overlay survey. The survey will use the peers of the initial node to seed the survey.
- Usage - Ex. `python3 OverlaySurvey.py -gs gs.json survey -n http://127.0.0.1:11626 -c 20 -sr sr.json -gmlw gmlw.graphml` to run the survey, `python3 OverlaySurvey.py -gs gs.json analyze -gmla gmla.graphml` to analyze an existing graph, or `python3 OverlaySurvey.py -gs gs.json augment -gmli gmlw.graphml -gmlo augmented.graphml` to augment the existing graph with data from StellarBeat.

    - `-gs GRAPHSTATS`, `--graphStats GRAPHSTATS` - output file for graph stats (Optional)
    - `-v`, `--verbose` - increase log verbosity (Optional)
    - sub command `survey` - run survey and analyze
        - `-n NODE`, `--node NODE` - address of initial survey node
        - `-c DURATION`, `--collectDuration DURATION` - duration of survey collecting phase in minutes
        - `-nl NODELIST`, `--nodeList NODELIST` - list of seed nodes. One node per line. (Optional)
        - `-gmlw GRAPHMLWRITE`, `--graphmlWrite GRAPHMLWRITE` - output file for graphml file
        - `-sr SURVEYRESULT`, `--surveyResult SURVEYRESULT` - output file for survey results
        - `-p`, `--startPhase` - Survey phase to begin from. One of `startCollecting`, `stopCollecting`, or `surveyResults`. See [Attaching to a Running Survey](#attaching-to-a-running-survey) for more info. (Optional)
    - sub command `simulate` - simulate a run of the `survey` subcommand without any network calls. Takes the same arguments as `survey`, plus the following:
        - `-s SIMGRAPH`, `--simGraph SIMGRAPH` - Network topology to simulate in graphml format.
        - `-r SIMROOT`, `--simRoot SIMROOT` - Node in graph to start simulation from.
    - sub command `analyze` - analyze an existing graph
        - `-gmla GRAPHMLANALYZE`, `--graphmlAnalyze GRAPHMLANALYZE` - input graphml file
    - sub command `augment` - augment an existing graph with information from  stellarbeat.io. Currently, only Public Network graphs are supported.
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-gmlo GRAPHMLOUTPUT` - output graphml file
    - sub command `flatten` - Take a graphml file containing a bidrectional graph (possibly augmented with StellarBeat data) and flatten it into an undirected graph in JSON.
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-json JSONOUTPUT` - output json file

#### Attaching to a Running Survey

Use the `--startPhase` option to attach the script to an already running survey. This may be necessary if something happened during the running of the script that caused the script to terminate early (such as losing connection with the surveyor node). `--startPhase` has three possible values:

- `startCollecting`: Start a survey from the beginning of the collecting phase. This is the default value when `--startPhase` is unspecified. It indicates you would like to start a new survey from the beginning (that is, you are not attaching the script to an existing survey).
- `stopCollecting`: Immediately broadcast a `TimeSlicedSurveyStopCollectingMessage` for the currently running survey and begin surveying individual nodes for results. Use this option if your survey is currently in the collecting phase and you'd like to move it to the reporting phase.
- `surveyResults`: Begin surveying individual nodes for results. Use this option if your survey is in the reporting phase.

### Diff Tracy CSV
- Name - `DiffTracyCSV.py`
- Description - A Python script that compares two CSV files produced by `tracy-csvexport` (which in turn reads output from `tracy-capture`). The purpose of this script is to detect significant performance impacts of changes to stellar-core by capturing before-and-after traces.
- Usage - Ex. `tracy-capture -o old.tracy -s 10 -a 127.0.0.1` to capture a 10 second trace of stellar-core running on the local machine. Then run `tracy-csvexport -u old.tracy >old.csv`. Then make a change to stellar-core and repeat the process to capture `new.tracy` and `new.csv`. Finally, run `DiffTracyCSV.py --old old.csv --new new.csv` and inspect the differences.

### Parse Backtrace Dump

- Name - `ParseDump.py`
- Description - A Python script that translates raw backtrace dumps to a human-readable format. If core crashes, on most compiler/OSes, a human readable stack trace is logged. This is not possible on linux clang. Instead, linux clang logs a raw stack trace that must be processed by the script.
- Usage - Ex. `ParseDump.py ./src/stellar-core "./src/stellar-core(+0xd9f6d5) [0x55c7cdb506d5]"`. The first argument is the path to a `stellar-core` executable with debug symbols. This exe must be the same version of `stellar-core` that produced the stack trace. However, only the exe argument for `ParseDump.py` needs debug symbols. The `stellar-core` exe that produced the backtrace does not need debug symbols. The second argument is a single string containing the backtrace to process. This string should contain a series of new-line delimited raw traces as follows:

```
"./src/stellar-core(+0x1987a5) [0x55c7ccf497a5]
./src/stellar-core(+0x4f538b) [0x55c7cd2a638b]
./src/stellar-core(+0x8ace59) [0x55c7cd65de59]
./src/stellar-core(+0x8a36d2) [0x55c7cd6546d2]
./src/stellar-core(+0x9b1916) [0x55c7cd762916]
./src/stellar-core(+0x9d0c10) [0x55c7cd781c10]
./src/stellar-core(+0xa413ec) [0x55c7cd7f23ec]
./src/stellar-core(+0xa3081a) [0x55c7cd7e181a]
./src/stellar-core(+0xa3a0e4) [0x55c7cd7eb0e4]
./src/stellar-core(+0xa44fe7) [0x55c7cd7f5fe7]
./src/stellar-core(+0x34f0c1) [0x55c7cd1000c1]"
```

### Stellar Core Debug Info

- Name - `stellar-core-debug-info`
- Description - Gathers useful information about core state in order to help debug crashes. This includes collecting log files, bucket directories,
SQL DB state, status reported by `offline-info`, and OS information for the given node.
- Usage - Ex. `stellar-core-debug-info /tmp/stellarCoreDumpOutputDirectory`. This script requires a destination directory to write temporary files to and the resulting
zip file of the collected debug information. Note that secret seeds from config files are automatically redacted.
If the given output directory does not exist, the script will attempt to create it. By default, the script checks
the `stellar-core.service` file to determine correct paths of the stellar-core executable and config file. From the config file, the script will
then parse the path of log files, bucket directory, and SQL DB. All these fields can be manually overridden as well, see
`stellar-core-debug-info --help` for specific flags.

### Soroban Settings Helper
- Name - `settings-helper.sh`
- Prequisites - `stellar-xdr` and `stellar-core`
- Description - This is a script to help with the [Soroban Settings Upgrade](../docs/software/soroban-settings.md). It's important to be aware of how the underlying process works
in case the script has some issues, so please read through that doc before attempting to use this script. This script should be run from the directory that contains your stellar-core binary. This script also queries the SDF Horizon for the sequence number of the account, so if the SDF Horizon instance is unavailable, then you'll need to provide the account's sequence number manually.
- Usage - Ex. `sh ../scripts/settings-helper.sh SCSQHJIUGUGTH2P4K6AOFTEW4HUMI2BRTUBBDDXMQ4FLHXCX25W3PGBJ testnet ../soroban-settings/testnet_settings_phase2.json`. The first argument is the secret key of the source account that will be used for the transactions to set up the upgrade, the second argument is the network passphrase, and the third is 
the path to the JSON file with the proposed settings.

### Histogram Generator
- Name - `histogram-generator.py`
- Description - A Python script that takes Hubble transaction data as input and outputs histograms containing information about the resource usage of those transactions. This script enables easy updating of transaction resource distributions in [Supercluster](https://github.com/stellar/supercluster), but some may find it more broadly helpful for understanding real-world usage of the Stellar network.
- Usage - `./HistogramGenerator <history_transactions_data> <history_contract_events_data>`, where `<history_transactions_data>` and `<history_contract_events_data>` are CSV files containing query results from Hubble's tables by the same names. You can use the following sample queries as a jumping off point for writing your own queries to generate these CSV files:
  - Sample query to gather `history_transactions_data` from a specific date range:
    ```lang=SQL
    SELECT soroban_resources_instructions, soroban_resources_write_bytes, tx_envelope FROM `crypto-stellar.crypto_stellar.history_transactions` WHERE batch_run_date BETWEEN DATETIME("2024-06-24") AND DATETIME("2024-09-24") AND soroban_resources_instructions > 0
    ```
  - Sample query to gather `history_contract_events_data` from a specific date range:
    ```lang=SQL
    SELECT topics_decoded, data_decoded FROM `crypto-stellar.crypto_stellar.history_contract_events` WHERE type = 2 AND TIMESTAMP_TRUNC(closed_at, MONTH) between TIMESTAMP("2024-06-27") AND TIMESTAMP("2024-09-27") AND contains_substr(topics_decoded, "write_entry")
    ```
     - NOTE: this query filters out anything that isn't a `write_entry`. This is required for the script to work correctly!

## Style guide
We follow [PEP-0008](https://www.python.org/dev/peps/pep-0008/).
