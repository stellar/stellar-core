
namespace stellar {

typedef opaque uint256[32];
typedef unsigned uint32;

struct FBABallot
{
    int index;           // n
    uint256 valueHash;   // x
};

struct FBASlotBallot
{
    uint256 slotHash;    // i
    FBABallot ballot;
};

enum FBAStatementType
{
    PREPARE,
    PREPARED,
    COMMIT,
    COMMITTED,
    UNKNOWN
};

struct FBAStatement
{
    FBASlotBallot slotBallot;
    uint256 quorumSetHash;
	
    union switch (FBAStatementType type)
    {
        case PREPARE:
            FBABallot excepted<>;  // B_c
        case PREPARED:
        case COMMIT:
        case COMMITTED:
        case UNKNOWN:
            void;		
    } body;
};

struct FBAEnvelope
{
    uint256 nodeID;
    uint256 signature;
    FBAStatement statement;
};

enum FBAQuorumSetType
{
    COMPACT,
    DETAILED,
    UNKNOWN
};

struct FBACompactQuorumSet 
{
    uint32 threshold;
    uint256 validators<>;
};

struct FBAQuorum
{
    uint256 validators<>;
};

struct FBAQuorumSet
{
    uint256 nodeID;
    union switch (FBAQuorumSetType type)
    {
        case COMPACT:
            FBACompactQuorumSet set;
        case DETAILED:
            FBAQuorum quorums<>;
        case UNKNOWN:
            void;
    } content;
};

}
