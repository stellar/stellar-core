// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "xdr/Stellar-ledger.h"

namespace stellar
{

enum ErrorCode
{
    ERR_MISC = 0, // Unspecific error
    ERR_DATA = 1, // Malformed data
    ERR_CONF = 2, // Misconfiguration error
    ERR_AUTH = 3, // Authentication failure
    ERR_LOAD = 4  // System overloaded
};

struct Error
{
    ErrorCode code;
    string msg<100>;
};

struct SendMore
{
    uint32 numMessages;
};

struct AuthCert
{
    Curve25519Public pubkey;
    uint64 expiration;
    Signature sig;
};

struct Hello
{
    uint32 ledgerVersion;
    uint32 overlayVersion;
    uint32 overlayMinVersion;
    Hash networkID;
    string versionStr<100>;
    int listeningPort;
    NodeID peerID;
    AuthCert cert;
    uint256 nonce;
};


// During the roll-out phrase, pull mode will be optional.
// Therefore, we need a way to communicate with other nodes
// that we want/don't want pull mode.
// However, the goal is for everyone to enable it by default,
// so we don't want to introduce a new member variable.
// For now, we'll use the `flags` field (originally named
// `unused`) in `Auth`.
// 100 is just a number that is not 0.
const AUTH_MSG_FLAG_PULL_MODE_REQUESTED = 100;

struct Auth
{
    int flags;
};

enum IPAddrType
{
    IPv4 = 0,
    IPv6 = 1
};

struct PeerAddress
{
    union switch (IPAddrType type)
    {
    case IPv4:
        opaque ipv4[4];
    case IPv6:
        opaque ipv6[16];
    }
    ip;
    uint32 port;
    uint32 numFailures;
};

// Next ID: 18
enum MessageType
{
    ERROR_MSG = 0,
    AUTH = 2,
    DONT_HAVE = 3,

    GET_PEERS = 4, // gets a list of peers this guy knows about
    PEERS = 5,

    GET_TX_SET = 6, // gets a particular txset by hash
    TX_SET = 7,
    GENERALIZED_TX_SET = 17,

    TRANSACTION = 8, // pass on a tx you have heard about

    // SCP
    GET_SCP_QUORUMSET = 9,
    SCP_QUORUMSET = 10,
    SCP_MESSAGE = 11,
    GET_SCP_STATE = 12,

    // new messages
    HELLO = 13,

    SURVEY_REQUEST = 14,
    SURVEY_RESPONSE = 15,

    SEND_MORE = 16,
    FLOOD_ADVERT = 18,
    FLOOD_DEMAND = 19
};

struct DontHave
{
    MessageType type;
    uint256 reqHash;
};

enum SurveyMessageCommandType
{
    SURVEY_TOPOLOGY = 0
};

enum SurveyMessageResponseType
{
    SURVEY_TOPOLOGY_RESPONSE_V0 = 0,
    SURVEY_TOPOLOGY_RESPONSE_V1 = 1
};

struct SurveyRequestMessage
{
    NodeID surveyorPeerID;
    NodeID surveyedPeerID;
    uint32 ledgerNum;
    Curve25519Public encryptionKey;
    SurveyMessageCommandType commandType;
};

struct SignedSurveyRequestMessage
{
    Signature requestSignature;
    SurveyRequestMessage request;
};

typedef opaque EncryptedBody<64000>;
struct SurveyResponseMessage
{
    NodeID surveyorPeerID;
    NodeID surveyedPeerID;
    uint32 ledgerNum;
    SurveyMessageCommandType commandType;
    EncryptedBody encryptedBody;
};

struct SignedSurveyResponseMessage
{
    Signature responseSignature;
    SurveyResponseMessage response;
};

struct PeerStats
{
    NodeID id;
    string versionStr<100>;
    uint64 messagesRead;
    uint64 messagesWritten;
    uint64 bytesRead;
    uint64 bytesWritten;
    uint64 secondsConnected;

    uint64 uniqueFloodBytesRecv;
    uint64 duplicateFloodBytesRecv;
    uint64 uniqueFetchBytesRecv;
    uint64 duplicateFetchBytesRecv;

    uint64 uniqueFloodMessageRecv;
    uint64 duplicateFloodMessageRecv;
    uint64 uniqueFetchMessageRecv;
    uint64 duplicateFetchMessageRecv;
};

typedef PeerStats PeerStatList<25>;

struct TopologyResponseBodyV0
{
    PeerStatList inboundPeers;
    PeerStatList outboundPeers;

    uint32 totalInboundPeerCount;
    uint32 totalOutboundPeerCount;
};

struct TopologyResponseBodyV1
{
    PeerStatList inboundPeers;
    PeerStatList outboundPeers;

    uint32 totalInboundPeerCount;
    uint32 totalOutboundPeerCount;

    uint32 maxInboundPeerCount;
    uint32 maxOutboundPeerCount;
};

union SurveyResponseBody switch (SurveyMessageResponseType type)
{
case SURVEY_TOPOLOGY_RESPONSE_V0:
    TopologyResponseBodyV0 topologyResponseBodyV0;
case SURVEY_TOPOLOGY_RESPONSE_V1:
    TopologyResponseBodyV1 topologyResponseBodyV1;
};

const TX_ADVERT_VECTOR_MAX_SIZE = 1000;
typedef Hash TxAdvertVector<TX_ADVERT_VECTOR_MAX_SIZE>;

struct FloodAdvert
{
    TxAdvertVector txHashes;
};

const TX_DEMAND_VECTOR_MAX_SIZE = 1000;
typedef Hash TxDemandVector<TX_DEMAND_VECTOR_MAX_SIZE>;

struct FloodDemand
{
    TxDemandVector txHashes;
};

union StellarMessage switch (MessageType type)
{
case ERROR_MSG:
    Error error;
case HELLO:
    Hello hello;
case AUTH:
    Auth auth;
case DONT_HAVE:
    DontHave dontHave;
case GET_PEERS:
    void;
case PEERS:
    PeerAddress peers<100>;

case GET_TX_SET:
    uint256 txSetHash;
case TX_SET:
    TransactionSet txSet;
case GENERALIZED_TX_SET:
    GeneralizedTransactionSet generalizedTxSet;

case TRANSACTION:
    TransactionEnvelope transaction;

case SURVEY_REQUEST:
    SignedSurveyRequestMessage signedSurveyRequestMessage;

case SURVEY_RESPONSE:
    SignedSurveyResponseMessage signedSurveyResponseMessage;

// SCP
case GET_SCP_QUORUMSET:
    uint256 qSetHash;
case SCP_QUORUMSET:
    SCPQuorumSet qSet;
case SCP_MESSAGE:
    SCPEnvelope envelope;
case GET_SCP_STATE:
    uint32 getSCPLedgerSeq; // ledger seq requested ; if 0, requests the latest
case SEND_MORE:
    SendMore sendMoreMessage;

// Pull mode
case FLOOD_ADVERT:
     FloodAdvert floodAdvert;
case FLOOD_DEMAND:
     FloodDemand floodDemand;
};

union AuthenticatedMessage switch (uint32 v)
{
case 0:
    struct
    {
        uint64 sequence;
        StellarMessage message;
        HmacSha256Mac mac;
    } v0;
};
}
