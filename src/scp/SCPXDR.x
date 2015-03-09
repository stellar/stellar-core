
namespace stellar {

typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque uint256[32];
typedef unsigned int uint32;
typedef unsigned hyper uint64;
typedef opaque Value<>;
typedef opaque Evidence<>;

struct SCPBallot
{
    uint32 counter;     // n
    Value value;        // x
};

enum SCPStatementType
{
    PREPARE,
    PREPARED,
    COMMIT,
    COMMITTED
};

struct SCPStatement
{
    uint64 slotIndex;      // i
    SCPBallot ballot;      // b
    Hash quorumSetHash;    // D
	
    union switch (SCPStatementType type)
    {
        case PREPARE:
            struct 
            {
                SCPBallot excepted<>;  // B_c
                SCPBallot* prepared;   // p
            } prepare;
        case PREPARED:
        case COMMIT:
        case COMMITTED:
            void;		
    } pledges;
};

struct SCPEnvelope
{
    uint256 nodeID;         // v
    SCPStatement statement;
    Signature signature;
};

struct SCPQuorumSet
{
    uint32 threshold;
    Hash validators<>;
};

}
