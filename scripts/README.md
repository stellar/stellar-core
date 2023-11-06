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
    - sub command `analyze` - analyze an existing graph
        - `-gmla GRAPHMLANALYZE`, `--graphmlAnalyze GRAPHMLANALYZE` - input graphml file
    - sub command `augment` - analyze an existing graph
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-gmlo GRAPHMLOUTPUT` - output graphml file
    - sub command `flatten` - Take a graphml file containing a bidrectional graph (possibly augmented with StellarBeat data) and flatten it into an undirected graph in JSON.
        - `-gmli GRAPHMLINPUT` - input graphml file
        - `-json JSONOUTPUT` - output json file

### Diff Tracy CSV
- Name - `DiffTracyCSV.py`
- Description - A Python script that compares two CSV files produced by `tracy-csvexport` (which in turn reads output from `tracy-capture`). The purpose of this script is to detect significant performance impacts of changes to stellar-core by capturing before-and-after traces.
- Usage - Ex. `tracy-capture -o old.tracy -s 10 -a 127.0.0.1` to capture a 10 second trace of stellar-core running on the local machine. Then run `tracy-csvexport -u old.tracy >old.csv`. Then make a change to stellar-core and repeat the process to capture `new.tracy` and `new.csv`. Finally, run `DiffTracyCSV.py --old old.csv --new new.csv` and inspect the differences.

### Soroban Settings Upgrade
- Directory - `soroban-settings`
- Name - `SorobanSettingsUpgrade.py`
- Description - A python script that can setup a setting upgrade or retrieve
  current settings for Futurenet through Soroban RPC. The next step is to submit all transactions directly to stellar-core's `tx` endpoint. Note that the actual upgrade command will have to be
  submitted manually on the core nodes. 
- Prerequisites
  - cd `soroban-settings/write_upgrade_bytes` and then run both `rustup target add wasm32-unknown-unknown` and `make build` to build the WASM contract used to write the proposed upgrade.
  - If the soroban branch of py-stellar-base
    (https://github.com/StellarCN/py-stellar-base/tree/soroban) has not been
    merged into main and released, then you'll have to install it Either
    checkout that branch locally and run `make install` in the `py-stellar-base`
    directory or run `pip install
    git+https://github.com/StellarCN/py-stellar-base.git@soroban`.
- Usage - Ex. `python3 SorobanSettingsUpgrade.py getSettings -id 10` to print out the
  current state archival settings. `SorobanSettingsUpgrade.py setupUpgrade`
  to setup the upgrade for the settings hardcoded in the script.
  -  `getSettings -id n` - Returns the `ConfigSettingEntry` for the `ConfigSettingID` enum that maps to `n`.
  - `setupUpgrade` - `python3 SorobanSettingsUpgrade.py setupUpgrade` to setup the upgrade for the settings hardcoded in the script.
    - 1. Include the settings you want to upgrade in the `ConfigUpgradeSet` that is returned in `get_upgrade_set`.
    - 2. Run `python3 SorobanSettingsUpgrade.py setupUpgrade`
    - 3. Take the URL encoded upgrade at the bottom of the output Ex. `url encoded upgrade: A2UbJ1lMHignaTyRtB9lTQT2DoN7zBUiepl68lfNdz5vy3TMBHuWLuVigdUG2XA/k1KxRH6RQ8WUKprvSRfm4A%3D%3D` and
    submit that to the core codes being upgraded. Make sure the UTC `upgradetime` is in the future so every validator has time to arm all nodes. Ex. `curl "http://localhost:11626/upgrades?mode=set&configupgradesetkey=A2UbJ1lMHignaTyRtB9lTQT2DoN7zBUiepl68lfNdz5vy3TMBHuWLuVigdUG2XA/k1KxRH6RQ8WUKprvSRfm4A%3D%3D&upgradetime=2023-07-19T21:59:00Z"`.

## Style guide
We follow [PEP-0008](https://www.python.org/dev/peps/pep-0008/).
