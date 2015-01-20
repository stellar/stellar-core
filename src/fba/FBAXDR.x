
namespace stellar {

typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque uint256[32];
typedef unsigned uint32;
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

struct FBAValuePart
{
    Value value;           // x
    Hash quorumSetHash;    // D
};

enum FBAEnvelopeType
{
    VALUE_PART,
    STATEMENT
};

struct FBAEnvelope
{
    uint256 nodeID;       // v
    uint64 slotIndex;     // i
    union switch (FBAEnvelopeType type)
    {
        case VALUE_PART:
            FBAValuePart part;
        case STATEMENT:
            FBAStatement statement;
    } payload;
    Signature signature;
};

struct FBAReadyEvidence
{
    FBAEnvelope parts<>;
};

struct FBAQuorumSet
{
    uint32 threshold;
    Hash validators<>;
};

}
