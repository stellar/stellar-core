
%#include "generated/Stellar-ledger.h"

namespace stellar {

struct StellarBallot
{
    Hash txSetHash;
    uint64 closeTime;
    int64 baseFee;
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
    int port;
};

struct PeerAddress
{
    opaque ip[4];
    uint32 port;
    uint32 numFailures;
};

enum MessageType
{
    ERROR_MSG,    
    HELLO,
    DONT_HAVE,

    GET_PEERS,   // gets a list of peers this guy knows about        
    PEERS,

    GET_TX_SET,  // gets a particular txset by hash        
    TX_SET,    

    GET_VALIDATIONS, // gets validations for a given ledger hash        
    VALIDATIONS,    

    TRANSACTION, //pass on a tx you have heard about        
    JSON_TRANSACTION,

    // FBA        
    GET_FBA_QUORUMSET,        
    FBA_QUORUMSET,    
    FBA_MESSAGE
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
