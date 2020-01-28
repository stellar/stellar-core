# importing the requests library 
import requests 
import json
import time
import sys
import os
import argparse
import networkx as nx
from networkx import Graph
from collections import defaultdict

def add_new_node(label):
    if G.has_node(label):
        return
    G.add_node(label, label=label)

def add_new_edge(u, v):
    if G.has_edge(u, v):
        return
    G.add_edge(u, v)

def get_next_peers(topology):
    results = []
    for key in topology:
        if topology[key] is None:
                continue

        if "inboundPeers" in topology[key] and topology[key]["inboundPeers"]:
            for in_peers in topology[key]["inboundPeers"]:
                results.append(in_peers["nodeId"])
        if "outboundPeers" in topology[key] and topology[key]["outboundPeers"]:
            for out_peers in topology[key]["outboundPeers"]:
                results.append(out_peers["nodeId"])
    return results

def send_requests(peer_list):
    for key in peer_list:
        PARAMS["node"] = key
        requests.get(url = SURVEY_REQUEST, params = PARAMS) 

def check_results():
    r = requests.get(url = SURVEY_RESULT)
    data = r.json()

    if("topology" not in data):
        return []

    topology = data["topology"]

    for key in topology:
        if topology[key] is None:
            continue

        if "inboundPeers" in topology[key] and topology[key]["inboundPeers"]:
            for in_peers in topology[key]["inboundPeers"]:
                merged_results[key]["inboundPeers"][in_peers["nodeId"]] = in_peers
                add_new_node(key)
                add_new_node(in_peers["nodeId"])
                add_new_edge(in_peers["nodeId"], key)
            merged_results[key]["totalInbound"] = topology[key]["numTotalInboundPeers"]
        if "outboundPeers" in topology[key] and topology[key]["outboundPeers"]:
            for out_peers in topology[key]["outboundPeers"]:
                merged_results[key]["outboundPeers"][out_peers["nodeId"]] = out_peers
                add_new_node(key)
                add_new_node(out_peers["nodeId"])
                add_new_edge(key, out_peers["nodeId"])
            merged_results[key]["totalOutbound"] = topology[key]["numTotalOutboundPeers"]
    
        merged_results[key]["responseSeen"] = True

    return get_next_peers(topology)

def write_graph_stats():
    stats = {}
    stats["average_shortest_path_length"] = nx.average_shortest_path_length(G)
    stats["average_clustering"] = nx.average_clustering(G)
    stats["clustering"] = nx.clustering(G)
    stats["degree"] = dict(nx.degree(G))
    with open('graphStats.json', 'w') as outfile:
        json.dump(stats, outfile)

#not used yet
def get_node_aliases():
    r = requests.get(url = "https://api.stellarbeat.io/v1/nodes")
    nodeList = r.json()
    
    result = {}
    for node in nodeList:
        if "alias" in node:
            result[node["publicKey"]] = node["alias"]
    return result
    

# construct the argument parse and parse the arguments
ap = argparse.ArgumentParser()
ap.add_argument("-n", "--node", help="address of initial survey node")
ap.add_argument("-d", "--duration", help="duration of survey in seconds")
ap.add_argument("-nl", "--nodeList", help="optional list of seed nodes")
ap.add_argument("-gml", "--graphml", help="print stats for the graphml input graph")

try:
    args = ap.parse_args()
except:
    print("Error with input arguments!")
    ap.print_help()
    sys.exit(0)

#we just want to print stats for an existing graph
if args.graphml:
    G = nx.read_graphml(args.graphml)
    write_graph_stats()
    sys.exit(0)

if not args.node or not args.duration:
    print("--node and --duration are both required to run the survey!")
    sys.exit(0)

G = nx.Graph()
merged_results = defaultdict(lambda:{
                "totalInbound":0,
                "totalOutbound":0,
                "inboundPeers":{},
                "outboundPeers":{},
                "responseSeen":False
                })

URL = args.node

PEERS = URL + "/peers"
SURVEY_REQUEST = URL + "/surveytopology"
SURVEY_RESULT = URL + "/getsurveyresult"
STOP_SURVEY = URL + "/stopsurvey"

duration = int(args.duration)
PARAMS = {'duration':duration} 

#reset survey
r = requests.get(url = STOP_SURVEY)
  
peer_list = []
if args.nodeList:
    #include nodes from file
    f = open(args.nodeList, "r")
    for node in f:
        peer_list.append(node.rstrip('\n'))

PEERS_PARAMS = {'fullkeys':"true"}
r = requests.get(url = PEERS, params = PEERS_PARAMS)
  
data = r.json()
peers = data["authenticated_peers"]

#seed initial peers off of /peers endpoint
if peers["inbound"]:
    for peer in peers["inbound"]:
        peer_list.append(peer["id"])
if peers["outbound"]:
    for peer in peers["outbound"]:
        peer_list.append(peer["id"])

t_end = time.time() + duration
while time.time() < t_end:
    send_requests(peer_list)
    peer_list = []

    #allow time for results
    time.sleep(1)
    result_node_list = check_results()
    
    #try new nodes
    for key in result_node_list:
        if key not in merged_results:
            peer_list.append(key)

    #retry for incomplete nodes
    for key in merged_results:
        node = merged_results[key]
        if(node["totalInbound"] > len(node["inboundPeers"])):
            peer_list.append(key)
        if(node["totalOutbound"] > len(node["outboundPeers"])):
            peer_list.append(key)

if nx.is_empty(G):
    print "Graph is empty!"
    sys.exit(0)

write_graph_stats()

nx.write_graphml(G, "survey.graphml")  

with open('surveyResults.json', 'w') as outfile:
    json.dump(merged_results, outfile)
  

