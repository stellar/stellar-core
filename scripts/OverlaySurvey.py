#!/usr/bin/env python

import argparse
from collections import defaultdict
import json
import networkx as nx
import requests
import sys
import time


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
        graph.add_node(other_key, version=peer["version"])
        # Adding an edge that already exists updates the edge data,
        # so we add everything except for nodeId and version
        # which are properties of nodes, not edges.
        edge_properties = peer.copy()
        edge_properties.pop("nodeId", None)
        edge_properties.pop("version", None)
        if is_inbound:
            graph.add_edge(other_key, parent_key, **edge_properties)
        else:
            graph.add_edge(parent_key, other_key, **edge_properties)

    if "numTotalInboundPeers" in parent_info:
        results["totalInbound"] = parent_info["numTotalInboundPeers"]
        graph.add_node(parent_key,
                       numTotalInboundPeers=parent_info[
                           "numTotalInboundPeers"
                       ])
    if "numTotalOutboundPeers" in parent_info:
        results["totalOutbound"] = parent_info["numTotalOutboundPeers"]
        graph.add_node(parent_key,
                       numTotalOutboundPeers=parent_info[
                           "numTotalOutboundPeers"
                       ])


def send_requests(peer_list, params, request_url):
    for key in peer_list:
        params["node"] = key
        requests.get(url=request_url, params=params)


def check_results(data, graph, merged_results):
    if "topology" not in data:
        raise ValueError("stellar-core is missing survey nodes."
                         "Are the public keys surveyed valid?")

    topology = data["topology"]

    for key in topology:

        curr = topology[key]
        if curr is None:
            continue

        merged = merged_results[key]

        update_results(graph, curr, key, merged, True)
        update_results(graph, curr, key, merged, False)

    return get_next_peers(topology)


def write_graph_stats(graph, output_file):
    stats = {}
    stats[
        "average_shortest_path_length"
    ] = nx.average_shortest_path_length(graph)
    stats["average_clustering"] = nx.average_clustering(graph)
    stats["clustering"] = nx.clustering(graph)
    stats["degree"] = dict(nx.degree(graph))
    with open(output_file, 'w') as outfile:
        json.dump(stats, outfile)


def analyze(args):
    graph = nx.read_graphml(args.graphmlAnalyze)
    if args.graphStats is not None:
        write_graph_stats(graph, args.graphStats)
    sys.exit(0)


def augment(args):
    graph = nx.read_graphml(args.graphmlInput)
    data = requests.get("https://api.stellarbeat.io/v1/nodes").json()
    for obj in data:
        if graph.has_node(obj["publicKey"]):
            desired_properties = ["quorumSet",
                                  "geoData",
                                  "isValidating",
                                  "name",
                                  "homeDomain",
                                  "organizationId",
                                  "index",
                                  "isp",
                                  "ip"]
            prop_dict = {}
            for prop in desired_properties:
                if prop in obj:
                    val = obj[prop]
                    if type(val) is dict:
                        val = json.dumps(val)
                    prop_dict['sb_{}'.format(prop)] = val
            graph.add_node(obj["publicKey"], **prop_dict)
    nx.write_graphml(graph, args.graphmlOutput)
    sys.exit(0)


def run_survey(args):
    graph = nx.DiGraph()
    merged_results = defaultdict(lambda: {
        "totalInbound": 0,
        "totalOutbound": 0,
        "inboundPeers": {},
        "outboundPeers": {}
    })

    url = args.node

    peers = url + "/peers"
    survey_request = url + "/surveytopology"
    survey_result = url + "/getsurveyresult"
    stop_survey = url + "/stopsurvey"

    duration = int(args.duration)
    params = {'duration': duration}

    # reset survey
    requests.get(url=stop_survey)

    peer_list = []
    if args.nodeList:
        # include nodes from file
        f = open(args.nodeList, "r")
        for node in f:
            peer_list.append(node.rstrip('\n'))

    peers_params = {'fullkeys': "true"}

    peers = requests.get(url=peers, params=peers_params).json()[
        "authenticated_peers"]

    # seed initial peers off of /peers endpoint
    if peers["inbound"]:
        for peer in peers["inbound"]:
            peer_list.append(peer["id"])
    if peers["outbound"]:
        for peer in peers["outbound"]:
            peer_list.append(peer["id"])

    graph.add_node(requests
                   .get(url + "/scp?limit=0&fullkeys=true")
                   .json()
                   ["you"],
                   version=requests.get(url + "/info").json()["info"]["build"],
                   numTotalInboundPeers=len(peers["inbound"] or []),
                   numTotalOutboundPeers=len(peers["outbound"] or []))

    sent_requests = set()

    while True:
        send_requests(peer_list, params, survey_request)

        for peer in peer_list:
            sent_requests.add(peer)

        peer_list = []

        # allow time for results
        time.sleep(1)

        data = requests.get(url=survey_result).json()

        result_node_list = check_results(data, graph, merged_results)

        if "surveyInProgress" in data and data["surveyInProgress"] is False:
            break

        # try new nodes
        for key in result_node_list:
            if key not in sent_requests:
                peer_list.append(key)

        # retry for incomplete nodes
        for key in merged_results:
            node = merged_results[key]
            if node["totalInbound"] > len(node["inboundPeers"]):
                peer_list.append(key)
            if node["totalOutbound"] > len(node["outboundPeers"]):
                peer_list.append(key)

    if nx.is_empty(graph):
        print("Graph is empty!")
        sys.exit(0)

    if args.graphStats is not None:
        write_graph_stats(graph, args.graphStats)

    nx.write_graphml(graph, args.graphmlWrite)

    with open(args.surveyResult, 'w') as outfile:
        json.dump(merged_results, outfile)


def flatten(args):
    output_graph = []
    graph = nx.read_graphml(args.graphmlInput).to_undirected()
    for node, attr in graph.nodes(data=True):
        new_attr = {"publicKey": node, "peers": list(map(str, graph.adj[node]))}
        for key in attr:
            try:
                new_attr[key] = json.loads(attr[key])
            except (json.JSONDecodeError, TypeError):
                new_attr[key] = attr[key]
        output_graph.append(new_attr)
    with open(args.jsonOutput, 'w') as output_file:
        json.dump(output_graph, output_file)
    sys.exit(0)


def main():
    # construct the argument parse and parse the arguments
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("-gs",
                                 "--graphStats",
                                 help="output file for graph stats")

    subparsers = argument_parser.add_subparsers()

    parser_survey = subparsers.add_parser('survey',
                                          help="run survey and "
                                               "analyze results")
    parser_survey.add_argument("-n",
                               "--node",
                               required=True,
                               help="address of initial survey node")
    parser_survey.add_argument("-d",
                               "--duration",
                               required=True,
                               help="duration of survey in seconds")
    parser_survey.add_argument("-sr",
                               "--surveyResult",
                               required=True,
                               help="output file for survey results")
    parser_survey.add_argument("-gmlw",
                               "--graphmlWrite",
                               required=True,
                               help="output file for graphml file")
    parser_survey.add_argument("-nl",
                               "--nodeList",
                               help="optional list of seed nodes")
    parser_survey.set_defaults(func=run_survey)

    parser_analyze = subparsers.add_parser('analyze',
                                           help="write stats for "
                                                "the graphml input graph")
    parser_analyze.add_argument("-gmla",
                                "--graphmlAnalyze",
                                help="input graphml file")
    parser_analyze.set_defaults(func=analyze)

    parser_augment = subparsers.add_parser('augment',
                                           help="augment the master graph "
                                                "with stellarbeat data")
    parser_augment.add_argument("-gmli",
                                "--graphmlInput",
                                help="input master graph")
    parser_augment.add_argument("-gmlo",
                                "--graphmlOutput",
                                required=True,
                                help="output file for the augmented graph")
    parser_augment.set_defaults(func=augment)

    parser_flatten = subparsers.add_parser("flatten",
                                            help="Flatten a directed graph into "
                                            "an undirected graph in JSON")
    parser_flatten.add_argument("-gmli",
                                 "--graphmlInput",
                                 required=True,
                                 help="input file containing a directed graph")
    parser_flatten.add_argument("-json",
                                 "--jsonOutput",
                                 required=True,
                                 help="output JSON file for the flattened graph")
    parser_flatten.set_defaults(func=flatten)

    args = argument_parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
