"""
This module contains useful definitions for both the simulator and the main
script.
"""

from collections import namedtuple

# A survey request that has not yet been serviced
PendingRequest = namedtuple("PendingRequest",
                            ["node",
                             "inbound_peer_index",
                             "outbound_peer_index"])

# The success response from the startsurveycollecting endpoint.
START_SURVEY_COLLECTING_SUCCESS_TEXT = \
    "Requested network to start survey collecting."

# The success response from the stopsurveycollecting endpoint.
STOP_SURVEY_COLLECTING_SUCCESS_TEXT = \
    "Requested network to stop survey collecting."

# An example success response from the surveytopologytimesliced endpoint.
# NOTE: There are two different success messages the endpoint can return.
# Clients should check for success by checking that the message starts with
# "Adding node."
SURVEY_TOPOLOGY_TIME_SLICED_SUCCESS_START = "Adding node."
SURVEY_TOPOLOGY_TIME_SLICED_SUCCESS_TEXT = \
    SURVEY_TOPOLOGY_TIME_SLICED_SUCCESS_START + "Survey already running!"