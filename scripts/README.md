## Description
This folder is for storing any scripts and files that may be helpful for building or using stellar-core.

## List of files
- [Overlay survey](#overlay-survey)
- Dockerfile: a Dockerfile used to build "production" docker images of stellar-core
- setup: used the by production Dockerfile
- Dockerfile.testing: a Dockerfile for one-off builds of stellar-core during development
- Makefile: convenience Makefile with `docker-build` and `docker-build-testing` targets

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

## Style guide
We follow [PEP-0008](https://www.python.org/dev/peps/pep-0008/).
