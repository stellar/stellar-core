
%#include "generated/Stellar-ledger.h"

namespace stellar {

struct StellarBallotValue
{
	Hash txSetHash;
    uint64 closeTime;
    int32 baseFee;
};

struct StellarBallot
{
    uint256 nodeID;
    Signature signature;
    StellarBallotValue value;
};

struct Error
{
    int code;
    string msg<100>;
};

struct Hello
{
    int protocolVersion;
    string versionStr<100>;
    int listeningPort;
	opaque peerID[32];
};

struct PeerAddress
{
    opaque ip[4];
    uint32 port;
    uint32 numFailures;
};

enum MessageType
{
    ERROR_MSG=0,    
    HELLO=1,
    DONT_HAVE=2,

    GET_PEERS=3,   // gets a list of peers this guy knows about        
    PEERS=4,

    GET_TX_SET=5,  // gets a particular txset by hash        
    TX_SET=6,    

    GET_VALIDATIONS=7, // gets validations for a given ledger hash        
    VALIDATIONS=8,    

    TRANSACTION=9, //pass on a tx you have heard about        
    JSON_TRANSACTION=10,

    // FBA        
    GET_FBA_QUORUMSET=11,        
    FBA_QUORUMSET=12,    
    FBA_MESSAGE=13
};

struct DontHave
{
    MessageType type;
    uint256 reqHash;
};

union StellarMessage switch (MessageType type) {
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

    case GET_VALIDATIONS:    
        uint256 ledgerHash;    
    case VALIDATIONS:
        FBAEnvelope validations<>;

    case TRANSACTION:
        TransactionEnvelope transaction;

    // FBA
    case GET_FBA_QUORUMSET:
        uint256 qSetHash;
    case FBA_QUORUMSET:
        FBAQuorumSet qSet;
    case FBA_MESSAGE:
        FBAEnvelope envelope;
};

}
