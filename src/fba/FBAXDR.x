
namespace stellar {

typedef opaque uint256[32];
typedef unsigned uint32;

struct FBABallot
{
    uint32 counter;      // n
    uint256 valueHash;   // x
};

enum FBAStatementType
{
    PREPARE,
    PREPARED,
    COMMIT,
    COMMITTED,
    INVALID
};

struct FBAStatement
{
    uint32 slotIndex;      // i
    FBABallot ballot;      // b
    uint256 quorumSetHash;
	
    union switch (FBAStatementType type)
    {
        case PREPARE:
            FBABallot excepted<>;  // B_c
        case PREPARED:
        case COMMIT:
        case COMMITTED:
        case INVALID:
            void;		
    } body;
};

struct FBAEnvelope
{
    uint256 nodeID;
    uint256 signature;
    FBAStatement statement;
};

struct FBAQuorumSet
{
    uint256 nodeID;
    uint32 threshold;
    uint256 validators<>;
};

}
