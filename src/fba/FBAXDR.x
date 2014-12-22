namespace fbaxdr {

typedef opaque uint256[32];
typedef unsigned uint32;

struct Ballot
{
    int index;           // n
    uint256 valueHash;   // x
};

struct SlotBallot
{
    uint256 slotHash;
    Ballot ballot;
};

enum StatementType
{
    PREPARE,
    PREPARED,
    COMMIT,
    COMMITTED,
    UNKNOWN
};

struct Statement
{
    SlotBallot slotBallot;
    uint256 quorumSetHash;
	
    union switch (StatementType type)
    {
        case PREPARE:
            Ballot excepted<>;
        case PREPARED:
        case COMMIT:
        case COMMITTED:
        case UNKNOWN:
            void;		
    } body;
};

struct Envelope
{
    uint256 nodeID;
    uint256 signature;
    Statement statement;
};

struct QuorumSet
{
    uint32 threshold;
    uint256 validators<>;
};

}
