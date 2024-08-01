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

# The error response from the surveytopologytimesliced endpoint when the survey
# backlog already contains the node requested to be surveyed, or the requested
# node is the surveyor. stellar-core returns this error JSON object where the
# error text is contained in the "exception" field.
SURVEY_TOPOLOGY_TIME_SLICED_ALREADY_IN_BACKLOG_OR_SELF = (
        "addPeerToBacklog failed: Peer is already in the backlog, or peer "
        "is self."
        )

# Response from the stopsurvey endpoint. This is the response regardless of
# whether or not a survey was running prior to calling this endpoint.
STOP_SURVEY_SUCCESS_TEXT = "survey stopped"