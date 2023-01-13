#!/usr/bin/env python
"""
Script to trigger network survey and record results. Sample survey result:
{
    "topology" :
    {
      "GDZPYXO3HC3GC6CCNTJH7BNURRH5VJTPDP5YALGNDCTPNDCSR74QBH3H" : {
         "inboundPeers" : [
            {
               "bytesRead" : 218480,
               "bytesWritten" : 225188,
               "duplicateFetchBytesRecv" : 0,
               "duplicateFetchMessageRecv" : 0,
               "duplicateFloodBytesRecv" : 173296,
               "duplicateFloodMessageRecv" : 443,
               "messagesRead" : 511,
               "messagesWritten" : 522,
               "nodeId" : "GB4HZXA2IALAMH4CIA5CBLOITQT45CC3275MNRQ4VTYJCFKJW2AHHMXP",
               "secondsConnected" : 4,
               "uniqueFetchBytesRecv" : 0,
               "uniqueFetchMessageRecv" : 0,
               "uniqueFloodBytesRecv" : 10288,
               "uniqueFloodMessageRecv" : 26,
               "version" : "v19.5.0-126-gcdf8d018-dirty"
            }
         ],
         "maxInboundPeerCount" : 64,
         "maxOutboundPeerCount" : 8,
         "numTotalInboundPeers" : 6,
         "numTotalOutboundPeers" : 8,
         "outboundPeers" : [
            {
               "bytesRead" : 220540,
               "bytesWritten" : 224720,
               "duplicateFetchBytesRecv" : 0,
               "duplicateFetchMessageRecv" : 0,
               "duplicateFloodBytesRecv" : 186584,
               "duplicateFloodMessageRecv" : 473,
               "messagesRead" : 517,
               "messagesWritten" : 521,
               "nodeId" : "GARM4GDQIQ2Z4SN5OXPG4XVHWZMKYEYTHY2X4TTP2TD2FDRAQOZIVJVX",
               "secondsConnected" : 4,
               "uniqueFetchBytesRecv" : 0,
               "uniqueFetchMessageRecv" : 0,
               "uniqueFloodBytesRecv" : 10612,
               "uniqueFloodMessageRecv" : 27,
               "version" : "v19.5.0-126-gcdf8d018-dirty"
            }
         ]
      }
    }
}
"""

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

    if "maxInboundPeerCount" in parent_info:
        results["maxInboundPeerCount"] = parent_info["maxInboundPeerCount"]
        graph.add_node(parent_key,
                       maxInboundPeerCount=parent_info[
                           "maxInboundPeerCount"
                       ])

    if "maxOutboundPeerCount" in parent_info:
        results["maxOutboundPeerCount"] = parent_info["maxOutboundPeerCount"]
        graph.add_node(parent_key,
                       maxOutboundPeerCount=parent_info[
                           "maxOutboundPeerCount"
                       ])


request_count = 0


def send_requests(peer_list, params, request_url):
    print("Requesting %s for %s peers" % (request_url, len(peer_list)))
    limit = 10
    # Submit `limit` queries roughly every ledger
    for key in peer_list:
        params["node"] = key
        requests.get(url=request_url, params=params)
        print("Send request to %s" % key)
        global request_count
        request_count += 1
        if (request_count % limit) == 0:
            print("Submitted %i queries, sleep for ~1 ledger" % limit)
            time.sleep(5)

    print("Done")


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


def get_tier1_stats(augmented_directed_graph):
    '''
    Helper function to help analyze transitive quorum. Must only be called on a graph augmented with StellarBeat info
    '''
    graph = augmented_directed_graph.to_undirected()
    tier1_nodes = [node for node, attr in graph.nodes(
        data=True) if 'isTier1' in attr and attr['isTier1'] == True]

    all_node_average = []
    for node in tier1_nodes:
        distances = []
        for other_node in tier1_nodes:
            if node != other_node:
                dist = nx.shortest_path_length(graph, node, other_node)
                distances.append(dist)
        avg_for_one_node = sum(distances)/len(distances)
        print("Average distance from %s to everyone else in Tier1: %.2f" %
              (nx.get_node_attributes(graph, 'sb_name')[node], avg_for_one_node))
        all_node_average.append(avg_for_one_node)

    if len(tier1_nodes):
        print("Average distance between all Tier1 nodes %.2f" %
              (sum(all_node_average)/len(all_node_average)))

        # Get average degree among Tier1 nodes
        degrees = [degree for (node, degree) in graph.degree()
                   if node in tier1_nodes]
        print("Average degree among Tier1 nodes: %.2f" %
              (sum(degrees)/len(degrees)))


def augment(args):
    graph = nx.read_graphml(args.graphmlInput)
    data = requests.get("https://api.stellarbeat.io/v1/nodes").json()
    transitive_quorum = requests.get(
        "https://api.stellarbeat.io/v1/").json()["transitiveQuorumSet"]

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
                    if val is None:
                        continue
                    if type(val) is dict:
                        val = json.dumps(val)
                    prop_dict['sb_{}'.format(prop)] = val
            graph.add_node(obj["publicKey"], **prop_dict)

    # Record Tier1 nodes
    for key in transitive_quorum:
        if graph.has_node(key):
            graph.add_node(key, isTier1=True)
        else:
            print("Warning: Tier1 node %s is not found in the survey data" % key)

    # Print a little more info about the quorum
    get_tier1_stats(graph)
    nx.write_graphml(graph, args.graphmlOutput)
    sys.exit(0)


def run_survey(args):
    graph = nx.DiGraph()
    merged_results = defaultdict(lambda: {
        "totalInbound": 0,
        "totalOutbound": 0,
        "maxInboundPeerCount": 0,
        "maxOutboundPeerCount": 0,
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

    peer_list = set()
    if args.nodeList:
        # include nodes from file
        f = open(args.nodeList, "r")
        for node in f:
            peer_list.add(node.rstrip('\n'))

    peers_params = {'fullkeys': "true"}

    peers = requests.get(url=peers, params=peers_params).json()[
        "authenticated_peers"]

    # seed initial peers off of /peers endpoint
    if peers["inbound"]:
        for peer in peers["inbound"]:
            peer_list.add(peer["id"])
    if peers["outbound"]:
        for peer in peers["outbound"]:
            peer_list.add(peer["id"])

    self_name = requests.get(url + "/scp?limit=0&fullkeys=true").json()["you"]
    graph.add_node(self_name,
                   version=requests.get(url + "/info").json()["info"]["build"],
                   numTotalInboundPeers=len(peers["inbound"] or []),
                   numTotalOutboundPeers=len(peers["outbound"] or []))

    sent_requests = set()
    heard_from = set()

    while True:
        send_requests(peer_list, params, survey_request)

        for peer in peer_list:
            sent_requests.add(peer)

        peer_list = set()

        # allow time for results
        time.sleep(1)

        print("Fetching survey result")
        data = requests.get(url=survey_result).json()
        print("Done")

        if "topology" in data:
            for key in data["topology"]:
                if data["topology"][key] is not None:
                    heard_from.add(key)

        waiting_to_hear = set()
        for node in sent_requests:
            if node not in heard_from and node != self_name:
                waiting_to_hear.add(node)
                print("Have not received response from %s" % node)

        print("Still waiting for survey results from %i nodes" %
              len(waiting_to_hear))

        result_node_list = check_results(data, graph, merged_results)

        if "surveyInProgress" in data and data["surveyInProgress"] is False:
            print("Survey complete")
            break

        # try new nodes
        for key in result_node_list:
            if key not in sent_requests:
                peer_list.add(key)
        new_peers = len(peer_list)
        # retry for incomplete nodes
        for key in merged_results:
            node = merged_results[key]
            if node["totalInbound"] > len(node["inboundPeers"]):
                peer_list.add(key)
            if node["totalOutbound"] > len(node["outboundPeers"]):
                peer_list.add(key)
        print("New peers: %s  Retrying: %s" %
              (new_peers, len(peer_list)-new_peers))

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
        new_attr = {"publicKey": node, "peers": list(
            map(str, graph.adj[node]))}
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
