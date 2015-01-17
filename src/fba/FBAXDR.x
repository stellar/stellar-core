
namespace stellar {

typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque uint256[32];
typedef unsigned uint32;
typedef unsigned hyper uint64;
typedef opaque Value<>;

struct FBABallot
{
    uint32 counter;   // n
    Value value;      // x
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
    Hash quorumSetHash;
	
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
    } body;
};

struct FBAEnvelope
{
    uint256 nodeID;
    FBAStatement statement;
    Signature signature;
};

struct FBAQuorumSet
{
    uint32 threshold;
    Hash validators<>;
};

}
