
namespace stellar {

typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque uint256[32];
typedef unsigned int uint32;
typedef unsigned hyper uint64;
typedef opaque Value<>;
typedef opaque Evidence<>;

struct FBABallot
{
    uint32 counter;     // n
    Value value;        // x
};

enum FBAStatementType
{
    PREPARE,
    PREPARED,
    COMMIT,
    COMMITTED
};

struct FBAStatement
{
    uint64 slotIndex;      // i
    FBABallot ballot;      // b
    Hash quorumSetHash;    // D
	
    union switch (FBAStatementType type)
    {
        case PREPARE:
            struct 
            {
                FBABallot excepted<>;  // B_c
                FBABallot* prepared;   // p
            } prepare;
        case PREPARED:
        case COMMIT:
        case COMMITTED:
            void;		
    } pledges;
};

struct FBAEnvelope
{
    uint256 nodeID;         // v
    FBAStatement statement;
    Signature signature;
};

struct FBAQuorumSet
{
    uint32 threshold;
    Hash validators<>;
};

}
