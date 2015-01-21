// things we need to serialize
//   protocol messages
//   FBA messages for signing
//   tx for signing
//   tx sets for FBA
//   tx sets for history
//   State snapshots for hashing

%#include "generated/FBAXDR.h"

namespace stellar {

// messages
typedef opaque uint512[64];
typedef opaque uint256[32];
typedef unsigned hyper uint64;
typedef hyper int64;
typedef unsigned uint32;
typedef int int32;
typedef opaque AccountID[32];
typedef opaque Signature[64];
typedef opaque Hash[32];
typedef opaque Thresholds[4];

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

enum TransactionType
{
	PAYMENT,
	CREATE_OFFER,
	CANCEL_OFFER,
	SET_OPTIONS,
	CHANGE_TRUST,
	ALLOW_TRUST,
	ACCOUNT_MERGE,
	INFLATION
};

enum CurrencyType
{
	NATIVE,
	ISO4217,
	CUSTOM_TEXT,
	CUSTOM_HASH,
	DEMURRAGE
};

struct HashCurrencyIssuer
{
	uint256 currencyCode;
	AccountID issuer;
};

struct ISOCurrencyIssuer
{
	opaque currencyCode[4];
	AccountID issuer;
};

union Currency switch(CurrencyType type)
{
	case NATIVE: 
		void;

	case ISO4217: 
		ISOCurrencyIssuer isoCI;

	// add other currency types here in the future
};
	

struct Signer
{
	uint256 pubKey;
	uint32 weight;  // really only need 1byte
};

struct KeyValue
{
	uint32 key;
	opaque value<64>;
};

struct PaymentTx
{
	AccountID destination;  
	Currency currency;			// what they end up with
	int64 amount;				// amount they end up with
	Currency path<>;			// what hops it must go through to get there
	int64 sendMax;				// the maximum amount of the source currency this
								// will send. The tx will fail if can't be met
	opaque memo<32>;
	opaque sourceMemo<32>;		// used to return a payment
};

struct CreateOfferTx
{
	Currency takerGets;
	Currency takerPays;
	int64 amount;		// amount taker gets
	int64 price;		// =takerPaysAmount/takerGetsAmount

	uint32 sequence;	// set if you want to change an existing offer
	uint32 flags;	// passive: only take offers that cross this. not offers that match it
};

struct SetOptionsTx
{
	AccountID* inflationDest;
	uint32*	flags;
	uint32* transferRate;
	KeyValue* data;
	Thresholds* thresholds;
	Signer* signer;
};

struct ChangeTrustTx
{
	Currency line;
	int64 limit;
};

struct AllowTrustTx
{
	AccountID trustor;
	union switch(CurrencyType type)
	{
		case NATIVE:
			void;

		case ISO4217:
			opaque currencyCode[4];

		// add other currency types here in the future
	} code;

	bool authorize;
};



struct Transaction
{
    AccountID account;
	uint32 maxFee;
	uint32 seqNum;
	uint32 maxLedger;	// maximum ledger this tx is valid to be applied in
	uint32 minLedger;   // minimum ledger this tx is valid to be applied in

	union switch (TransactionType type)
	{
		case PAYMENT:
			PaymentTx paymentTx;
		case CREATE_OFFER:
			CreateOfferTx createOfferTx;
		case CANCEL_OFFER:
			uint32 offerSeqNum;
		case SET_OPTIONS:
			SetOptionsTx setOptionsTx;
		case CHANGE_TRUST:
			ChangeTrustTx changeTrustTx;
		case ALLOW_TRUST:
			AllowTrustTx allowTrustTx;
		case ACCOUNT_MERGE:
			uint256 destination;
		case INFLATION:
			uint32 inflationSeq;
	} body;
};

struct TransactionEnvelope
{
    Transaction tx;
    uint512 signatures<>;
};

struct TransactionSet
{
    uint256 previousLedgerHash;
    TransactionEnvelope txs<>;
};

struct CLFBucketHeader
{
    uint64 ledgerSeq;
    uint32 ledgerCount;
    Hash hash;
};

struct CLFLevel
{
    CLFBucketHeader curr;
    CLFBucketHeader snap;
};

struct LedgerHeader
{
	Hash hash;
    Hash previousLedgerHash;
	Hash txSetHash;			// the tx set that was FBA confirmed
	Hash clfHash;
    CLFLevel clfLevels[5];
	
	int64 totalCoins;
	int64 feePool;
	uint64 ledgerSeq;
	uint32 inflationSeq;
	int32 baseFee;
	int32 baseReserve;
	uint64 closeTime;       
};


struct HistoryEntry
{
    LedgerHeader header;
    TransactionSet txSet;
};

struct History
{
    uint64 fromLedger;
    uint64 toLedger;
    HistoryEntry entries<>;
};



enum LedgerType {
  NONE,
  ACCOUNT,
  TRUSTLINE,
  OFFER
};



struct AccountEntry
{
	uint256 accountID;
	int64 balance;
	uint32 sequence;
	uint32 ownerCount;
	uint32 transferRate;	// amountsent/transferrate is how much you charge
	uint256 *inflationDest;
	opaque thresholds[4]; // weight of master/threshold1/threshold2/threshold3
	Signer signers<>; // do we want some max or just increase the min balance
	KeyValue data<>;

	uint32 flags; // require dt, require auth, 
};



struct TrustLineEntry
{
    uint256 accountID;
    Currency currency;
    int64 limit;
    int64 balance;
    bool authorized;  // if the issuer has authorized this guy to hold its credit
};


// selling 10A @ 2B/A
struct OfferEntry
{
    uint256 accountID;
    uint32 sequence;
	Currency takerGets;  // A
	Currency takerPays;  // B
	int64 amount;	// amount of A
	int64 price;	// price of A in terms of B
					// price*10,000,000
					// price=AmountB/AmountA
					// price is after fees
    int32 flags;
};

union LedgerEntry switch (LedgerType type)
{
 case NONE:
	void;
 case ACCOUNT:
   AccountEntry account;
 case TRUSTLINE:
   TrustLineEntry trustLine;
 case OFFER:
   OfferEntry offer;
};

struct CLFEntry
{
    LedgerEntry entry;
    Hash hash;
};

struct CLFBucket
{
    CLFBucketHeader header;
    CLFEntry entries<>;
};

struct StellarBallot
{
    Hash txSetHash;
    uint64 closeTime;
    int64 baseFee;
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
