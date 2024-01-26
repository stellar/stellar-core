## Description
This folder is for storing any scripts that may be helpful for using stellar-core.

## List of scripts
- [Overlay survey](#overlay-survey)
- [Diff Tracy CSV](#diff-tracy-csv)

### Overlay survey 
- Name - `OverlaySurvey.py`
- Description - A Python script that will walk the network using the Overlay survey mechanism to gather connection information. See [admin](./../docs/software/admin.md#overlay-topology-survey) for more information on the overlay survey. The survey will use the peers of the initial node to seed the survey.
- Usage - Ex. `python3 OverlaySurvey.py -gs gs.json survey -n http://127.0.0.1:11626 -d 50 -sr sr.json -gmlw gmlw.graphml` to run the survey, `python3 OverlaySurvey.py -gs gs.json analyze -gmla gmla.graphml` to analyze an existing graph, or `python3 OverlaySurvey.py -gs gs.json augment -gmli gmlw.graphml -gmlo augmented.graphml` to augment the existing graph with data from StellarBeat.

    - `-gs GRAPHSTATS`, `--graphStats GRAPHSTATS` - output file for graph stats (Optional)
    - sub command `survey` - run survey and analyze
        - `-n NODE`, `--node NODE` - address of initial survey node
        - `-d DURATION`, `--duration DURATION` - duration of survey in seconds
        - `-nl NODELIST`, `--nodeList NODELIST` - list of seed nodes. One node per line. (Optional)
        - `-gmlw GRAPHMLWRITE`, `--graphmlWrite GRAPHMLWRITE` - output file for graphml file
        - `-sr SURVEYRESULT`, `--surveyResult SURVEYRESULT` - output file for survey results
    - sub command `simulate` - simulate a run of the `survey` subcommand without any network calls. Takes the same arguments as `survey`, plus the following:
        - `-s SIMGRAPH`, `--simGraph SIMGRAPH` - Network topology to simulate in graphml format.
        - `r SIMROOT`, `--simRoot SIMROOT` - Node in graph to start simulation from.
    - sub command `analyze` - analyze an existing graph
        - `-gmla GRAPHMLANALYZE`, `--graphmlAnalyze GRAPHMLANALYZE` - input graphml file
    - sub command `augment` - augment an existing graph with information from  stellarbeat.io. Currently, only Public Network graphs are supported.
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-gmlo GRAPHMLOUTPUT` - output graphml file
    - sub command `flatten` - Take a graphml file containing a bidrectional graph (possibly augmented with StellarBeat data) and flatten it into an undirected graph in JSON.
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-json JSONOUTPUT` - output json file

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

## Style guide
We follow [PEP-0008](https://www.python.org/dev/peps/pep-0008/).
