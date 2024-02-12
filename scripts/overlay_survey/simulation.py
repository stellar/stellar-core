"""
This module simulates the HTTP endpoints of stellar-core's overlay survey
"""

import networkx as nx

class SimulationError(Exception):
    """An error that occurs during simulation"""

class SimulatedResponse:
    """Simulates a `requests.Response`"""
    def __init__(self, json):
        self._json = json

    def json(self):
        """Simulates the `json` method of a `requests.Response`"""
        return self._json

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
        print(f"simulating from {root_node}")

    def _info(self, params):
        """
        Simulate the info endpoint. Only fills in the version info for the
        root node.
        """
        assert not params, f"Unsupported info invocation with params: {params}"
        version = self._graph.nodes[self._root_node]["version"]
        return SimulatedResponse({"info" : {"build" : version}})

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
        return SimulatedResponse(json)

    def _scp(self, params):
        """Simulate the scp endpoint. Only fills in the "you" field"""
        assert params == {"fullkeys": "true", "limit": 0}, \
               f"Unsupported scp invocation with params: {params}"
        return SimulatedResponse({"you": self._root_node})

    def _surveytopology(self, params):
        """
        Simulate the surveytopology endpoint. This endpoint currently ignores
        the `duration` parameter
        """
        assert params.keys() == {"node", "duration"}, \
               f"Unsupported surveytopology invocation with params: {params}"
        if params["node"] != self._root_node:
            self._pending_requests.append(params["node"])

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

        # For simulation purposes, the survey is in progress so long as there
        # are still pending requests to simulate.
        self._results["surveyInProgress"] = bool(self._pending_requests)

        # Update results
        while self._pending_requests:
            node = self._pending_requests.pop()

            # Start with info on the node itself
            node_json = self._graph.nodes[node].copy()

            # Remove "version" field, which is not part of stellar-core's
            # response
            del node_json["version"]

            # Generate inboundPeers list
            node_json["inboundPeers"] = []
            for (node_id, _, data) in self._graph.in_edges(node, True):
                self._addpeer(node_id, data, node_json["inboundPeers"])

            # Generate outboundPeers list
            node_json["outboundPeers"] = []
            for (_, node_id, data) in self._graph.out_edges(node, True):
                self._addpeer(node_id, data, node_json["outboundPeers"])

            self._results["topology"][node] = node_json
        return SimulatedResponse(self._results)

    def get(self, url, params):
        """Simulate a GET request"""
        endpoint = url.split("/")[-1]
        if endpoint == "stopsurvey":
            # Do nothing
            return
        if endpoint == "info":
            return self._info(params)
        if endpoint == "peers":
            return self._peers(params)
        if endpoint == "scp":
            return self._scp(params)
        if endpoint == "surveytopology":
            return self._surveytopology(params)
        if endpoint == "getsurveyresult":
            return self._getsurveyresult(params)
        raise SimulationError("Received GET request for unknown endpoint "
                              f"'{endpoint}' with params '{params}'")
