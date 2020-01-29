#!/usr/bin/env python

import argparse
from collections import defaultdict
import json
import networkx as nx
import requests
import sys
import time


def add_new_node(graph, label):
    if graph.has_node(label):
        return
    graph.add_node(label, label=label)


def add_new_edge(graph, u, v):
    if graph.has_edge(u, v):
        return
    graph.add_edge(u, v)


def next_peer(direction_tag, node_info):
    if direction_tag in node_info and node_info[direction_tag]:
        for peer in node_info[direction_tag]:
            yield peer


def get_next_peers(topology):
    results = []
    for key in topology:

        curr = topology[key]
        if curr is None:
            continue

        for peer in next_peer("inboundPeers", curr):
            results.append(peer["nodeId"])

        for peer in next_peer("outboundPeers", curr):
            results.append(peer["nodeId"])

    return results


def update_results(graph, parent_info, parent_key, results, is_inbound):
    direction_tag = "inboundPeers" if is_inbound else "outboundPeers"
    for peer in next_peer(direction_tag, parent_info):
        other_key = peer["nodeId"]

        results[direction_tag][other_key] = peer
        add_new_node(graph, parent_key)
        add_new_node(graph, other_key)
        add_new_edge(graph, other_key, parent_key)

    if "numTotalInboundPeers" in parent_info:
        results["totalInbound"] = parent_info["numTotalInboundPeers"]
    if "numTotalOutboundPeers" in parent_info:
        results["totalOutbound"] = parent_info["numTotalOutboundPeers"]


def send_requests(peer_list, params, requestUrl):
    for key in peer_list:
        params["node"] = key
        requests.get(url=requestUrl, params=params)


def check_results(graph, merged_results, resultUrl):
    r = requests.get(url=resultUrl)
    data = r.json()

    if "topology" not in data:
        raise ValueError("stellar-core is missing survey nodes. Are the public keys surveyed valid?")

    topology = data["topology"]

    for key in topology:

        curr = topology[key]
        if curr is None:
            continue

        merged = merged_results[key]

        update_results(graph, curr, key, merged, True)
        update_results(graph, curr, key, merged, False)

        merged["responseSeen"] = True

    return get_next_peers(topology)


def write_graph_stats(graph, outputFile):
    stats = {}
    stats["average_shortest_path_length"] = nx.average_shortest_path_length(graph)
    stats["average_clustering"] = nx.average_clustering(graph)
    stats["clustering"] = nx.clustering(graph)
    stats["degree"] = dict(nx.degree(graph))
    with open(outputFile, 'w') as outfile:
        json.dump(stats, outfile)


def analyze(args):
    G = nx.read_graphml(args.graphmlAnalyze)
    write_graph_stats(G, args.graphStats)
    sys.exit(0)


def run_survey(args):
    G = nx.Graph()
    merged_results = defaultdict(lambda: {
                    "totalInbound": 0,
                    "totalOutbound": 0,
                    "inboundPeers": {},
                    "outboundPeers": {},
                    "responseSeen": False
                    })

    URL = args.node

    PEERS = URL + "/peers"
    SURVEY_REQUEST = URL + "/surveytopology"
    SURVEY_RESULT = URL + "/getsurveyresult"
    STOP_SURVEY = URL + "/stopsurvey"

    duration = int(args.duration)
    PARAMS = {'duration': duration}

    # reset survey
    r = requests.get(url=STOP_SURVEY)

    peer_list = []
    if args.nodeList:
        # include nodes from file
        f = open(args.nodeList, "r")
        for node in f:
            peer_list.append(node.rstrip('\n'))

    PEERS_PARAMS = {'fullkeys': "true"}
    r = requests.get(url=PEERS, params=PEERS_PARAMS)

    data = r.json()
    peers = data["authenticated_peers"]

    # seed initial peers off of /peers endpoint
    if peers["inbound"]:
        for peer in peers["inbound"]:
            peer_list.append(peer["id"])
    if peers["outbound"]:
        for peer in peers["outbound"]:
            peer_list.append(peer["id"])

    t_end = time.time() + duration
    while time.time() < t_end:
        send_requests(peer_list, PARAMS, SURVEY_REQUEST)
        peer_list = []

        # allow time for results
        time.sleep(1)
        result_node_list = check_results(G, merged_results, SURVEY_RESULT)

        # try new nodes
        for key in result_node_list:
            if key not in merged_results:
                peer_list.append(key)

        # retry for incomplete nodes
        for key in merged_results:
            node = merged_results[key]
            if(node["totalInbound"] > len(node["inboundPeers"])):
                peer_list.append(key)
            if(node["totalOutbound"] > len(node["outboundPeers"])):
                peer_list.append(key)

    if nx.is_empty(G):
        print "Graph is empty!"
        sys.exit(0)

    write_graph_stats(G, args.graphStats)

    nx.write_graphml(G, args.graphmlWrite)

    with open(args.surveyResult, 'w') as outfile:
        json.dump(merged_results, outfile)


def main():
    # construct the argument parse and parse the arguments
    ap = argparse.ArgumentParser()
    ap.add_argument("-gs", "--graphStats", required=True, help="output file for graph stats")

    subparsers = ap.add_subparsers()

    parser_survey = subparsers.add_parser('survey', help="run survey and analyze results")
    parser_survey.add_argument("-n", "--node", required=True, help="address of initial survey node")
    parser_survey.add_argument("-d", "--duration", required=True, help="duration of survey in seconds")
    parser_survey.add_argument("-sr", "--surveyResult", required=True, help="output file for survey results")
    parser_survey.add_argument("-gmlw", "--graphmlWrite", required=True, help="output file for graphml file")
    parser_survey.add_argument("-nl", "--nodeList", help="optional list of seed nodes")
    parser_survey.set_defaults(func=run_survey)

    parser_analyze = subparsers.add_parser('analyze', help="write stats for the graphml input graph")
    parser_analyze.add_argument("-gmla", "--graphmlAnalyze", help="input graphml file")
    parser_analyze.set_defaults(func=analyze)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
