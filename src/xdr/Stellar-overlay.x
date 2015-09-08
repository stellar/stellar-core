// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

%#include "xdr/Stellar-ledger.h"

namespace stellar
{

struct Error
{
    int code;
    string msg<100>;
};

struct Hello
{
    uint32 ledgerVersion;
    uint32 overlayVersion;
    Hash networkID;
    string versionStr<100>;
    int listeningPort;
    NodeID peerID;
};

struct PeerAddress
{
    opaque ip[4];
    uint32 port;
    uint32 numFailures;
};

enum MessageType
{
    ERROR_MSG = 0,
    HELLO = 1,
    DONT_HAVE = 2,

    GET_PEERS = 3, // gets a list of peers this guy knows about
    PEERS = 4,

    GET_TX_SET = 5, // gets a particular txset by hash
    TX_SET = 6,

    TRANSACTION = 7, // pass on a tx you have heard about

    // SCP
    GET_SCP_QUORUMSET = 8,
    SCP_QUORUMSET = 9,
    SCP_MESSAGE = 10
};

struct DontHave
{
    MessageType type;
    uint256 reqHash;
};

union StellarMessage switch (MessageType type)
{
case ERROR_MSG:
    Error error;
case HELLO:
    Hello hello;
case DONT_HAVE:
    DontHave dontHave;
case GET_PEERS:
    void;
case PEERS:
    PeerAddress peers<>;

case GET_TX_SET:
    uint256 txSetHash;
case TX_SET:
    TransactionSet txSet;

case TRANSACTION:
    TransactionEnvelope transaction;

// SCP
case GET_SCP_QUORUMSET:
    uint256 qSetHash;
case SCP_QUORUMSET:
    SCPQuorumSet qSet;
case SCP_MESSAGE:
    SCPEnvelope envelope;
};
}
