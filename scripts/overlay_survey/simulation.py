"""
This module simulates the HTTP endpoints of stellar-core's overlay survey
"""

from enum import Enum
import logging
import networkx as nx
import random

import overlay_survey.util as util

# Max size of returned peer lists
PEER_LIST_SIZE = 25

logger = logging.getLogger(__name__)

class SimulationError(Exception):
    """An error that occurs during simulation"""

class SimulatedResponse:
    """Simulates a `requests.Response`. Either `json` xor `text` must be set."""
    def __init__(self, json=None, text=None):
        assert (json is not None) ^ (text is not None)
        self._json = json
        self.text = text if json is None else str(json)

    def json(self):
        """Simulates the `json` method of a `requests.Response`"""
        assert self._json is not None
        return self._json

def _add_v2_survey_data(node_json):
    """
    Augment a v1 survey result with the additional fields from a v2 survey.
    Does nothing if the node_json is already a v2 survey result or if the V1
    survey data indicates the node didn't respond to the survey.
    """
    if "numTotalInboundPeers" not in node_json:
        # Node did not respond to the survey. Nothing to do.
        return

    if "lostSyncCount" in node_json:
        # Already a V2 survey result
        return

    # Add node-level fields
    node_json["addedAuthenticatedPeers"] = random.randint(0, 2**32-1)
    node_json["droppedAuthenticatedPeers"] = random.randint(0, 2**32-1)
    node_json["p75SCPFirstToSelfLatencyMs"] = random.randint(0, 2**32-1)
    node_json["p75SCPSelfToOtherLatencyMs"] = random.randint(0, 2**32-1)
    node_json["lostSyncCount"] = random.randint(0, 2**32-1)
    node_json["isValidator"] = random.choice([True, False])

    # Add averageLatencyMs to each peer
    for peer in node_json["inboundPeers"]:
        peer["averageLatencyMs"] = random.randint(0, 2**32-1)
    for peer in node_json["outboundPeers"]:
        peer["averageLatencyMs"] = random.randint(0, 2**32-1)

class SurveySimulation:
    """
    Simulates the HTTP endpoints of stellar-core's overlay survey. Raises
    SimulationError if `root_node` is not in the graph represented by
    `graph_path`.
    """
    def __init__(self, graph_path, root_node):
        # The graph of the network being simulated
        self._graph = nx.read_graphml(graph_path)
        if root_node not in self._graph.nodes:
            raise SimulationError(f"root node '{root_node}' not in graph")
        # The node the simulation is being performed from
        self._root_node = root_node
        # The set of requests that have not yet been simulated
        self._pending_requests = []
        # The results of the simulation
        self._results = {"topology" : {}}
        logger.info("simulating from %s", root_node)

    def _info(self, params):
        """
        Simulate the info endpoint. Only fills in the version info for the
        root node.
        """
        assert not params, f"Unsupported info invocation with params: {params}"
        version = self._graph.nodes[self._root_node]["version"]
        return SimulatedResponse(json={"info" : {"build" : version}})

    def _peers(self, params):
        """
        Simulate the peers endpoint. Only fills in the "id" field of each
        authenticated peer.
        """
        assert params == {"fullkeys": "true"}, \
               f"Unsupported peers invocation with params: {params}"
        json = {"authenticated_peers": {"inbound": [], "outbound": []}}
        for peer in self._graph.in_edges(self._root_node):
            json["authenticated_peers"]["inbound"].append({"id" : peer[0]})
        for peer in self._graph.out_edges(self._root_node):
            json["authenticated_peers"]["outbound"].append({"id" : peer[1]})
        return SimulatedResponse(json=json)

    def _scp(self, params):
        """Simulate the scp endpoint. Only fills in the "you" field"""
        assert params == {"fullkeys": "true", "limit": 0}, \
               f"Unsupported scp invocation with params: {params}"
        return SimulatedResponse(json={"you": self._root_node})

    def _startsurveycollecting(self, params):
        """
        Simulate the startsurveycollecting endpoint.
        """
        assert params.keys() == {"nonce"}
        return SimulatedResponse(util.START_SURVEY_COLLECTING_SUCCESS_TEXT)

    def _stopsurveycollecting(self, params):
        """
        Simulate the stopsurveycollecting endpoint.
        """
        assert not params
        return SimulatedResponse(text=util.STOP_SURVEY_COLLECTING_SUCCESS_TEXT)

    def _surveytopologytimesliced(self, params):
        """
        Simulate the surveytopologytimesliced endpoint.
        """
        assert params.keys() == {"node",
                                 "inboundpeerindex",
                                 "outboundpeerindex"}

        fail_response = SimulatedResponse(
            {"exception" :
                util.SURVEY_TOPOLOGY_TIME_SLICED_ALREADY_IN_BACKLOG_OR_SELF})
        node = params["node"]
        inbound_peer_idx = params["inboundpeerindex"]
        outbound_peer_idx = params["outboundpeerindex"]
        if node == self._root_node:
            # Nodes cannot survey themselves (yet)
            return fail_response

        if ((inbound_peer_idx > 0 or outbound_peer_idx > 0) and
            random.random() < 0.2):
            # Randomly indicate that node is already in backlog if it is being
            # resurveyed. Script should handle this by trying again later.
            return fail_response

        req = util.PendingRequest(node, inbound_peer_idx, outbound_peer_idx)
        self._pending_requests.append(req)
        return SimulatedResponse(
            text=util.SURVEY_TOPOLOGY_TIME_SLICED_SUCCESS_TEXT)

    def _addpeer(self, node_id, edge_data, peers):
        """
        Given data on a graph edge in `edge_data`, translate to the expected
        getsurveyresult json and add to `peers` list
        """
        # Start with data on the edge itself
        peer_json = edge_data.copy()
        # Add peer's node id and version
        peer_json["nodeId"] = node_id
        peer_json["version"] = self._graph.nodes[node_id]["version"]
        # Add to inboundPeers
        peers.append(peer_json)

    def _getsurveyresult(self, params):
        """Simulate the getsurveyresult endpoint"""
        assert not params, \
               f"Unsupported getsurveyresult invocation with params: {params}"

        # For simulation purposes, the survey is always in progress. This tests
        # the expected scenario in which the reporting phase is significantly
        # longer than it takes to survey all nodes. The script should gracefully
        # handle this and not stall
        self._results["surveyInProgress"] = True

        # Update results
        while self._pending_requests:
            node, inbound_peer_index, outbound_peer_index = \
                self._pending_requests.pop()

            # Start with info on the node itself
            node_json = self._graph.nodes[node].copy()

            # Remove "version" field, which is not part of stellar-core's
            # response
            del node_json["version"]

            # Generate inboundPeers list
            node_json["inboundPeers"] = []
            in_edges = list(self._graph.in_edges(node, True))
            inbound_slice = in_edges[
                inbound_peer_index : inbound_peer_index + PEER_LIST_SIZE
                ]
            for (node_id, _, data) in inbound_slice:
                self._addpeer(node_id, data, node_json["inboundPeers"])
            if ("numTotalInboundPeers" in node_json and
                node_json["numTotalInboundPeers"] != len(in_edges)):
                # The V1 survey contains a race condition in which the number of
                # peers can change between when a node reports its peer count
                # and when the surveyor requests the peers themselves. The V2
                # survey resolves this with time slicing. The different handling
                # of peer counts between the V1 and V2 surveys can cause issues
                # when simulating a V2 survey using V1 survey data, resulting in
                # an output graph that is not isomorphic to the input graph.
                # Therefore, we patch up the peer counts in the simulated survey
                # results with the real peer counts, rather than using what the
                # node reported during the V1 survey.
                logger.warning("Node %s has %s inbound peers, but the node "
                               "claims it has %s inbound peers. Replacing "
                               "survey results with actual inbound peer "
                               "count.",
                               node,
                               len(in_edges),
                               node_json["numTotalInboundPeers"])
                node_json["numTotalInboundPeers"] = len(in_edges)

            # Generate outboundPeers list
            node_json["outboundPeers"] = []
            out_edges = list(self._graph.out_edges(node, True))
            outbound_slice = out_edges[
                outbound_peer_index : outbound_peer_index + PEER_LIST_SIZE
                ]
            for (_, node_id, data) in outbound_slice:
                self._addpeer(node_id, data, node_json["outboundPeers"])
            if ("numTotalOutboundPeers" in node_json and
                node_json["numTotalOutboundPeers"] != len(out_edges)):
                # Patch up peer counts in simulated survey results with real
                # peer counts (see note on similar conditional for inbound peers
                # above)
                logger.warning("Node %s has %s outbound peers, but the node "
                               "claims it has %s outbound peers. Replacing "
                               "survey results with actual outbound peer "
                               "count.",
                               node,
                               len(out_edges),
                               node_json["numTotalOutboundPeers"])
                node_json["numTotalOutboundPeers"] = len(out_edges)

            _add_v2_survey_data(node_json)

            self._results["topology"][node] = node_json
        return SimulatedResponse(json=self._results)

    def get(self, url, params):
        """Simulate a GET request"""
        endpoint = url.split("/")[-1]
        if endpoint == "info":
            return self._info(params)
        if endpoint == "peers":
            return self._peers(params)
        if endpoint == "scp":
            return self._scp(params)
        if endpoint == "startsurveycollecting":
            return self._startsurveycollecting(params)
        if endpoint == "stopsurveycollecting":
            return self._stopsurveycollecting(params)
        if endpoint == "surveytopologytimesliced":
            return self._surveytopologytimesliced(params)
        if endpoint == "getsurveyresult":
            return self._getsurveyresult(params)
        if endpoint == "stopsurvey":
            # In stellar-core this has the effect of clearing the survey results
            # cache. No such thing exists in the simulator, so this is a no-op.
            return SimulatedResponse(text=util.STOP_SURVEY_SUCCESS_TEXT)

        raise SimulationError("Received GET request for unknown endpoint "
                              f"'{endpoint}' with params '{params}'")
