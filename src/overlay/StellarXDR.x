// things we need to serialize
//   protocol messages
//   FBA messages for signing
//   tx for signing
//   tx sets for FBA
//   tx sets for history
//   Bucket list for hashing
//   array of ledgerentries for sending deltas

%#include "generated/FBAXDR.h"

namespace stellar {

// messages
typedef opaque uint512[64];
typedef opaque uint256[32];
typedef opaque uint160[20];
typedef unsigned hyper uint64;
typedef hyper int64;
typedef unsigned uint32;
typedef int int32;

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

struct CurrencyIssuer
{
	uint160 currencyCode;
	uint256 issuer;
};

union Currency switch(bool native)
{
	case TRUE: 
		void;
	case FALSE:
		CurrencyIssuer ci;
};
	


struct KeyValue
{
	uint32 key;
	opaque value<64>;
};

struct PaymentTx
{
	uint256 destination;  
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

	uint32 sequence;		// set if you want to change an existing offer
	bool passive;	// only take offers that cross this. not offers that match it
};

struct SetOptionsTx
{
	uint256* creditAuthKey;
	uint256* pubKey;
	uint256* inflationDest;
	uint32*	flags;
	uint32* transferRate;
	KeyValue* data;
};

struct ChangeTrustTx
{
	CurrencyIssuer line;
	int64 limit;
};

struct AllowTrustTx
{
	uint256 trustor;
	uint160 currencyCode;
	bool authorize;
};

struct Transaction
{
    uint256 account;
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
    uint256 signature;
};

struct TransactionSet
{
    uint256 previousLedgerHash;
    TransactionEnvelope txs<>;
};

struct LedgerHeader
{
	uint256 hash;
    uint256 previousLedgerHash;
    uint256 txSetHash;
	uint256 clfHash;
	
	int64 totalCoins;
	int64 feePool;
	uint64 ledgerSeq;
	uint32 inflationSeq;
	int32 baseFee;
	uint64 closeTime;       
};


struct LedgerHistory
{
    LedgerHeader header;
    TransactionSet txSet;
};

struct History
{
    int fromLedger;
    int toLedger;
    LedgerHistory ledgers<>;
};



enum LedgerTypes {
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
    uint32 transferRate;	// rate*10000000
    uint256 *pubKey;
	uint256 *inflationDest;
	uint256 *creditAuthKey;
	KeyValue data<>;

	uint32 flags; // disable master, require dt, require auth, 
};



struct TrustLineEntry
{
    uint256 accountID;
    uint256 issuer;
    uint160 currencyCode;
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
					// price*1,000,000,000
					// price=AmountB/AmountA
					// price is after fees
    bool passive;
};

union LedgerEntry switch (LedgerTypes type)
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

struct PeerAddress
{
    uint32 ip;
    uint32 port;
};

enum MessageType
{
	ERROR_MSG,	
	HELLO,
	DONT_HAVE,
	
	GET_PEERS,   // gets a list of peers this guy knows about		
	PEERS,		
		
	GET_HISTORY,  // gets a list of tx sets in the given range		
	HISTORY,		

	GET_DELTA,  // gets all the bucket list changes since a particular ledger index		
	DELTA,		
		
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

struct GetDelta
{
    uint256 oldLedgerHash;
    uint32 oldLedgerSeq;
};

struct Delta
{
	LedgerHeader ledgerHeader;
	LedgerEntry deltaEntries<>;
};

struct GetHistory
{
    uint256 a;
    uint256 b;
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
	case GET_HISTORY:
		GetHistory historyReq;
	case HISTORY:
		History history;
	case GET_DELTA:	
		GetDelta deltaReq;	
	case DELTA:	
		Delta delta;

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
		FBAEnvelope fbaMessage;
};


}
