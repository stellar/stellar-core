## Description
This folder is for storing any scripts that may be helpful for using stellar-core.

## List of scripts
- [Overlay survey](#overlay-survey)

### Overlay survey 
- Name - `OverlaySurvey.py`
- Description - A Python script that will walk the network using the Overlay survey mechanism to gather connection information. See [admin](./../docs/software/admin.md#overlay-topology-survey) for more information on the overlay survey. The survey will use the peers of the initial node to seed the survey.
- Usage - Ex. `python OverlaySurvey.py -n http://127.0.0.1:8080 -d 45 -nl temp.txt` to run the survey or `python OverlaySurvey.py -gml survey.graphml` to analyze an existing graph.
    - `-n NODE`, `--node NODE` - address of initial survey node (required if not using -gml)
    - `-d DURATION`, `--duration DURATION` - duration of survey in seconds (required if not using -gml)
    - `-nl NODELIST`, `--nodeList NODELIST` - list of seed nodes. One node per line. (Optional)
    - `-gml GRAPHML`, `--graphml GRAPHML` - print stats for the graphml input graph. Does not run survey.
- Output files
    - `graphStats.json` - networkx stats about graph
    - `surveyResults.json` - connection information gathered from survey
    - `survey.graphml` - networkx graph in graphml format
