
namespace stellar
{

typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque uint256[32];
typedef unsigned int uint32;
typedef unsigned hyper uint64;
typedef opaque Value<>;
typedef opaque Evidence<>;

struct SCPBallot
{
    uint32 counter; // n
    Value value;    // x
};

enum SCPStatementType
{
    PREPARING = 0,
    PREPARED = 1,
    COMMITTING = 2,
    COMMITTED = 3
};

struct SCPStatement
{
    uint64 slotIndex;   // i
    SCPBallot ballot;   // b
    Hash quorumSetHash; // D

    union switch (SCPStatementType type)
    {
    case PREPARING:
        struct
        {
            SCPBallot excepted<>; // B_c
            SCPBallot* prepared;  // p
        } prepare;
    case PREPARED:
    case COMMITTING:
    case COMMITTED:
        void;
    }
    pledges;
};

struct SCPEnvelope
{
    uint256 nodeID; // v
    SCPStatement statement;
    Signature signature;
};


// supports things like: A,B,C,(D,E,F),(G,H,(I,J,K,L))
// only allows 2 levels of nesting
struct SCPQuorumSet
{
    uint32 threshold;
	Hash validators<>;
    SCPQuorumSet innerSets<>;
};


}
