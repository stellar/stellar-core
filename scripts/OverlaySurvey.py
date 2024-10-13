#!/usr/bin/env python
"""
Script to trigger network survey and record results. Sample survey result:
{
  "backlog": [],
  "badResponseNodes": null,
  "surveyInProgress": true,
  "topology": {
    "GBBNXPPGDFDUQYH6RT5VGPDSOWLZEXXFD3ACUPG5YXRHLTATTUKY42CL": null,
    "GDEXJV6XKKLDUWKTSXOOYVOYWZGVNIKKQ7GVNR5FOV7VV5K4MGJT5US4": {
      "inboundPeers": [
        {
          "averageLatencyMs": 23,
          "bytesRead": 26392,
          "bytesWritten": 26960,
          "duplicateFetchBytesRecv": 0,
          "duplicateFetchMessageRecv": 0,
          "duplicateFloodBytesRecv": 10424,
          "duplicateFloodMessageRecv": 43,
          "messagesRead": 93,
          "messagesWritten": 96,
          "nodeId": "GBBNXPPGDFDUQYH6RT5VGPDSOWLZEXXFD3ACUPG5YXRHLTATTUKY42CL",
          "secondsConnected": 22,
          "uniqueFetchBytesRecv": 0,
          "uniqueFetchMessageRecv": 0,
          "uniqueFloodBytesRecv": 11200,
          "uniqueFloodMessageRecv": 46,
          "version": "v12.2.0-46-g61aadd29"
        },
        {
          "averageLatencyMs": 213,
          "bytesRead": 32204,
          "bytesWritten": 31212,
          "duplicateFetchBytesRecv": 0,
          "duplicateFetchMessageRecv": 0,
          "duplicateFloodBytesRecv": 11200,
          "duplicateFloodMessageRecv": 46,
          "messagesRead": 115,
          "messagesWritten": 112,
          "nodeId": "GBUICIITZTGKL7PUBHUPWD67GDRAIYUA4KCOH2PUIMMZ6JQLNVA7C4JL",
          "secondsConnected": 23,
          "uniqueFetchBytesRecv": 176,
          "uniqueFetchMessageRecv": 2,
          "uniqueFloodBytesRecv": 14968,
          "uniqueFloodMessageRecv": 62,
          "version": "v12.2.0-46-g61aadd29"
        }
      ],
      "numTotalInboundPeers": 2,
      "numTotalOutboundPeers": 0,
      "maxInboundPeerCount": 64,
      "maxOutboundPeerCount": 8,
      "addedAuthenticatedPeers" : 0,
      "droppedAuthenticatedPeers" : 0,
      "p75SCPFirstToSelfLatencyMs" : 121,
      "p75SCPSelfToOtherLatencyMs" : 112,
      "lostSyncCount" : 0,
      "isValidator" : false,
      "outboundPeers": null
    }
  }
}
"""

import argparse
from collections import defaultdict
import json
import logging
import networkx as nx
import random
import requests
import sys
import time

import overlay_survey.simulation as sim
import overlay_survey.util as util

logger = logging.getLogger(__name__)

# A SurveySimulation, if running in simulation mode, or None otherwise.
SIMULATION = None

# Maximum duration of collecting phase in minutes. This matches stellar-core's
# internal limit.
MAX_COLLECT_DURATION = 30

# Maximum number of consecutive rounds in which the surveyor does not receive
# responses from any nodes. A round contains a batch of requests sent to select
# nodes, followed by a wait period of 15 seconds, followed by checking for
# responses and building up the next batch of requests to send. Therefore, a
# setting of `8` is roughly 2 minutes of inactivity before the script considers
# the survey complete. This is necessary because it's very likely that not all
# surveyed nodes will respond to the survey.  Therefore, we need some cutoff
# after we which we assume those nodes will never respond.
MAX_INACTIVE_ROUNDS = 8

# Maximum number of nodes to request survey data from in a single batch.
MAX_BATCH_SIZE = 5

# Length of time stellar-core waits between sending out batches of requests.
BATCH_DURATION_SECONDS = 15

def get_request(url, params=None):
    """ Make a GET request, or simulate one if running in simulation mode. """
    logger.debug("Sending GET request for %s with params %s", url, params)
    if SIMULATION:
        res = SIMULATION.get(url=url, params=params)
    else:
        res = requests.get(url=url, params=params)
    logger.debug("Received response: %s", res.text)
    return res

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

def update_node(graph, node_info, node_key, results, field_names):
    """
    For each `field_name` in `field_names`, if `field_name` is in `node_info`,
    modify `graph` and `results` to contain the field.
    """
    for field_name in field_names:
        if field_name in node_info:
            val = node_info[field_name]
            results[field_name] = val
            graph.add_node(node_key, **{field_name: val})

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

    # Add survey results to parent node (if available)
    field_names = ["numTotalInboundPeers",
                   "numTotalOutboundPeers",
                   "maxInboundPeerCount",
                   "maxOutboundPeerCount",
                   "addedAuthenticatedPeers",
                   "droppedAuthenticatedPeers",
                   "p75SCPFirstToSelfLatencyMs",
                   "p75SCPSelfToOtherLatencyMs",
                   "lostSyncCount",
                   "isValidator"]
    update_node(graph, parent_info, parent_key, results, field_names)


def send_survey_requests(peer_list, url_base, skip_sleep):
    """
    Request survey data from a list of peers. `url_base` is the root HTTP
    endpoint to send requests to.
    """
    request_url = url_base + "/surveytopologytimesliced"
    logger.info("Requesting survey data from %s peers", len(peer_list))
    num_sent = 0
    for (nodeid, inbound_peer_index, outbound_peer_index) in peer_list:
        if num_sent != 0 and num_sent % MAX_BATCH_SIZE == 0:
            logger.info("Sent %i/%i requests", num_sent, len(peer_list))
            if not skip_sleep:
                logger.info("Waiting %i seconds before sending next batch",
                            BATCH_DURATION_SECONDS)
                time.sleep(BATCH_DURATION_SECONDS)
        params = { "node": nodeid,
                   "inboundpeerindex": inbound_peer_index,
                   "outboundpeerindex": outbound_peer_index }
        response = get_request(url=request_url, params=params)
        num_sent += 1
        if response.text.startswith(
            util.SURVEY_TOPOLOGY_TIME_SLICED_SUCCESS_START):
            logger.debug("Send request to %s", nodeid)
        else:
            try:
                exception = response.json()["exception"]
                if exception == \
                   util.SURVEY_TOPOLOGY_TIME_SLICED_ALREADY_IN_BACKLOG_OR_SELF:
                    logger.debug("Node %s is already in backlog or is self",
                                 nodeid)
                else:
                    logger.error("Failed to send survey request to %s: %s",
                                nodeid, exception)
            except (requests.exceptions.JSONDecodeError, KeyError):
                logger.error("Failed to send survey request to %s: %s",
                             nodeid, response.text)

    logger.info("Done sending survey requests")


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
    try:
        stats = {}
        stats[
            "average_shortest_path_length"
        ] = nx.average_shortest_path_length(graph)
        stats["average_clustering"] = nx.average_clustering(graph)
        stats["clustering"] = nx.clustering(graph)
        stats["degree"] = dict(nx.degree(graph))
        with open(output_file, 'w') as outfile:
            json.dump(stats, outfile)
    except nx.NetworkXException as e:
        logger.error("Error calculating graph stats: %s", e)


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
        logger.info("Average distance from %s to everyone else in Tier1: %.2f",
                    nx.get_node_attributes(graph, 'sb_name')[node],
                    avg_for_one_node)
        all_node_average.append(avg_for_one_node)

    if len(tier1_nodes):
        logger.info("Average distance between all Tier1 nodes %.2f",
              sum(all_node_average)/len(all_node_average))

        # Get average degree among Tier1 nodes
        degrees = [degree for (node, degree) in graph.degree()
                   if node in tier1_nodes]
        logger.info("Average degree among Tier1 nodes: %.2f",
              (sum(degrees)/len(degrees)))


def augment(args):
    graph = nx.read_graphml(args.graphmlInput)
    data = get_request("https://api.stellarbeat.io/v1/nodes").json()
    transitive_quorum = get_request(
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
            logger.warning("Tier1 node %s is not found in the survey data", key)

    # Print a little more info about the quorum
    get_tier1_stats(graph)
    nx.write_graphml(graph, args.graphmlOutput)
    sys.exit(0)

def start_survey_collecting(url, skip_sleep, collect_duration):
    """
    Start the survey collecting phase. This function blocks for the duration of
    the collecting phase. It occasionally pings the surveyor to keep any SSH
    tunnel alive.

    Arguments:
        url -- the base URL of the surveyor node
        skip_sleep -- if True, skip the sleep period. Should only be used when
                      simulating.
        collect_duration -- duration of the collecting phase in minutes
    """

    start_collecting = url + "/startsurveycollecting"
    info = url + "/info"
    nonce = random.randint(0, 2**32-1)
    logger.info("Starting survey with nonce %s", nonce)
    response = get_request(url=start_collecting, params={'nonce': nonce})
    if response.text != util.START_SURVEY_COLLECTING_SUCCESS_TEXT:
        logger.critical("Failed to start survey: %s", response.text)
        sys.exit(1)

    for i in range(collect_duration, 0, -1):
        logger.info("%i minutes remaining in collecting phase", i)
        if not skip_sleep:
            time.sleep(60)
        # Keep the SSH tunnel alive by hitting surveyor's /info endpoint
        get_request(url=info)

def stop_survey_collecting(url, skip_sleep):
    """
    Stop the survey collecting phase.

    Arguments:
        url -- the base URL of the surveyor node
        skip_sleep -- if True, skip the sleep period. Should only be used when
                      simulating.
    """
    stop_collecting = url + "/stopsurveycollecting"
    logger.info("Stopping survey collecting")
    response = get_request(url=stop_collecting)
    if response.text != util.STOP_SURVEY_COLLECTING_SUCCESS_TEXT:
        logger.critical("Failed to stop survey: %s", response.text)
        sys.exit(1)

    if not skip_sleep:
        # Allow time for stop message to propagate
        sleep_time = 60
        logger.info(
            "Waiting %i seconds for 'stop collecting' message to propagate",
            sleep_time)
        time.sleep(sleep_time)

def run_survey(args):
    if args.simulate:
        global SIMULATION
        try:
            SIMULATION = sim.SurveySimulation(args.simGraph, args.simRoot)
        except sim.SimulationError as e:
            logger.critical("%s", e)
            sys.exit(1)

    skip_sleep = args.simulate and args.fast
    url = args.node

    if args.startPhase == "startCollecting":
        start_survey_collecting(url, skip_sleep, args.collectDuration)

    if (args.startPhase == "startCollecting" or
        args.startPhase == "stopCollecting"):
        stop_survey_collecting(url, skip_sleep)

    if args.startPhase == "surveyResults":
        # Script is being run partway through an existing survey. To keep
        # everything in sync, clear survey results cache before surveying nodes.
        response = get_request(url + "/stopsurvey")
        if response.text != util.STOP_SURVEY_SUCCESS_TEXT:
            logger.critical("Failed to clear survey cache: %s", response.text)
            sys.exit(1)

    graph = nx.DiGraph()
    merged_results = defaultdict(lambda: {
        "numTotalInboundPeers": 0,
        "numTotalOutboundPeers": 0,
        "maxInboundPeerCount": 0,
        "maxOutboundPeerCount": 0,
        "inboundPeers": {},
        "outboundPeers": {}
    })

    peers = url + "/peers"
    survey_result = url + "/getsurveyresult"

    peer_list = set()
    if args.nodeList:
        # include nodes from file
        with open(args.nodeList, "r") as f:
            for node in f:
                peer_list.add(node.rstrip('\n'))

    peers_params = {'fullkeys': "true"}

    peers = get_request(url=peers, params=peers_params).json()[
        "authenticated_peers"]

    # seed initial peers off of /peers endpoint
    if peers["inbound"]:
        for peer in peers["inbound"]:
            peer_list.add(util.PendingRequest(peer["id"], 0, 0))
    if peers["outbound"]:
        for peer in peers["outbound"]:
            peer_list.add(util.PendingRequest(peer["id"], 0, 0))

    scp_params = {'fullkeys': "true", 'limit': 0}
    self_name = get_request(url + "/scp", scp_params).json()["you"]
    graph.add_node(self_name,
                   version=get_request(url + "/info").json()["info"]["build"],
                   numTotalInboundPeers=len(peers["inbound"] or []),
                   numTotalOutboundPeers=len(peers["outbound"] or []))

    sent_requests = set()
    heard_from = set()
    incomplete_responses = set()

    # Number of consecutive rounds in which surveyor neither sent requests nor
    # received responses
    inactive_rounds = 0

    while True:
        inactive_rounds += 1

        send_survey_requests(peer_list, url, skip_sleep)

        for peer in peer_list:
            sent_requests.add(peer.node)

        peer_list = set()

        if not skip_sleep:
            # allow time for results. Stellar-core sends out a batch of requests
            # every BATCH_DURATION_SECONDS seconds, so there's not much benefit
            # in checking more frequently than that
            logger.info("Waiting %i seconds for survey results",
                        BATCH_DURATION_SECONDS)
            time.sleep(BATCH_DURATION_SECONDS)

        logger.info("Fetching survey result")
        data = get_request(url=survey_result).json()
        logger.info("Done fetching result")

        if "topology" in data:
            for key in data["topology"]:
                node_data = data["topology"][key]
                if node_data is not None:
                    if key not in heard_from:
                        # Received a new response!
                        logger.debug("Received response from %s", key)
                        inactive_rounds = 0
                        heard_from.add(key)
                    elif key in incomplete_responses and len(node_data) > 0:
                        # Received additional data for a node that previously
                        # responded
                        logger.debug("Received additional data for %s", key)
                        inactive_rounds = 0
                        incomplete_responses.remove(key)

        waiting_to_hear = set()
        for node in sent_requests:
            if node not in heard_from and node != self_name:
                waiting_to_hear.add(node)
                logger.debug("Have not received response from %s", node)

        logger.info("Still waiting for survey results from %i nodes",
              len(waiting_to_hear))

        result_node_list = check_results(data, graph, merged_results)

        if inactive_rounds >= MAX_INACTIVE_ROUNDS:
            logger.info("Survey complete")
            break

        if inactive_rounds > 0:
            logger.info("No activity for %i rounds. %i rounds remaining",
                        inactive_rounds,
                        MAX_INACTIVE_ROUNDS - inactive_rounds)

        # try new nodes
        for key in result_node_list:
            if key not in sent_requests:
                peer_list.add(util.PendingRequest(key, 0, 0))
        new_peers = len(peer_list)
        # Gather additional peers for incomplete nodes
        for key in merged_results:
            node = merged_results[key]
            have_inbound = len(node["inboundPeers"])
            have_outbound = len(node["outboundPeers"])
            if (node["numTotalInboundPeers"] > have_inbound or
                node["numTotalOutboundPeers"] > have_outbound):
                incomplete_responses.add(key)
                req = util.PendingRequest(key, have_inbound, have_outbound)
                peer_list.add(req)
        logger.info("New nodes: %s  Gathering additional peer data: %s",
              new_peers, len(peer_list)-new_peers)

    nx.write_graphml(graph, args.graphmlWrite)

    with open(args.surveyResult, 'w') as outfile:
        json.dump(merged_results, outfile)

    # sanity check that simulation produced a graph isomorphic to the input
    assert (not args.simulate or
            nx.is_isomorphic(graph, nx.read_graphml(args.simGraph))), \
           ("Simulation produced a graph that is not isomorphic to the input "
            "graph")

    if nx.is_empty(graph):
        logger.warning("Graph is empty!")
        sys.exit(0)

    if args.graphStats is not None:
        write_graph_stats(graph, args.graphStats)


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

def init_parser_survey(parser_survey):
    """Initialize the `survey` subcommand"""
    parser_survey.add_argument("-n",
                               "--node",
                               required=True,
                               help="address of initial survey node")
    parser_survey.add_argument("-c",
                               "--collectDuration",
                               required=True,
                               type=int,
                               choices=range(1, MAX_COLLECT_DURATION + 1),
                               help="Duration of collecting phase in minutes. " "Must be between 1 and 30.")
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
    parser_survey.add_argument("-p",
                               "--startPhase",
                               help="Survey phase to start from. "
                                    "Defaults to 'startCollecting'.",
                               choices=["startCollecting",
                                        "stopCollecting",
                                        "surveyResults"],
                               default="startCollecting")
    parser_survey.set_defaults(func=run_survey)

def main():
    # construct the argument parse and parse the arguments
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("-gs",
                                 "--graphStats",
                                 help="output file for graph stats")
    argument_parser.add_argument("-v",
                                 "--verbose",
                                 help="increase output verbosity",
                                 action="store_true")

    subparsers = argument_parser.add_subparsers()

    parser_survey = subparsers.add_parser('survey',
                                          help="run survey and "
                                               "analyze results")
    parser_survey.set_defaults(simulate=False)
    init_parser_survey(parser_survey)
    parser_simulate = subparsers.add_parser('simulate',
                                             help="simulate survey run")
    # `simulate` supports all arguments that `survey` does, plus some additional
    # arguments for the simulation itself.
    init_parser_survey(parser_simulate)
    parser_simulate.add_argument("-s",
                                 "--simGraph",
                                 required=True,
                                 help="graphml file to simulate network from")
    parser_simulate.add_argument("-r",
                                 "--simRoot",
                                 required=True,
                                 help="node to start simulation from")
    parser_simulate.add_argument("-f",
                                 "--fast",
                                 action="store_true",
                                 help="Skip sleep calls during simulation.")
    parser_simulate.set_defaults(simulate=True)

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

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="[%(asctime)s %(levelname)8s] %(message)s")

    args.func(args)


if __name__ == "__main__":
    main()
