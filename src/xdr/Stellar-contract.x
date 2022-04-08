%#include "xdr/Stellar-types.h"
%#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

/*
* Smart Contracts deal in SCVals. These are a (dynamic) disjoint union between
* several possible variants, to allow storing generic SCVals in generic data
* structures and passing them in and out of languages that have simple or
* dynamic type systems.
*
* SCVals are (in WASM's case) stored in a tagged 64-bit word encoding. Most
* signed 64-bit values in Stellar are actually signed positive values (sequence
* numbers, timestamps, amounts), so we don't need the high bit and can get away
* with 1-bit tagging and store them as "u63", separate from everything else.
*
* We actually reserve the first _four_ bits, leaving 3 bits for 8 cases of
* "non u63 values", some of which has some substructure of their own.
*
*    0x_nnnn_nnnn_nnnn_nnnX  - u63 for any even X
*    0x_0000_000n_nnnn_nnn1  - u32
*    0x_0000_000n_nnnn_nnn3  - i32
*    0x_nnnn_nnnn_nnnn_nnn5  - static values: void, true, false, ...
*    0x_IIII_IIII_IIII_SST7  - object: with 16 object types, 256 subtypes each, and 48-bit index
*    0x_nnnn_nnnn_nnnn_nnn9  - symbol: up to 10 6-bit identifier characters
*    0x_nnnn_nnnn_nnnn_nnnb  - small bitset (up to 60 bits)
*    0x_CCCC_CCCC_TTTT_TTTd  - 32-bit status code C, and 28-bit type code T
*    0x_nnnn_nnnn_nnnn_nnnf  - reserved
*
* Up here in XDR we have variable-length tagged disjoint unions but no bit-level
* packing, so we can be more explicit in their structure, at the cost of spending more
* than 64 bits to encode many cases, and also having to convert. It's a little
* non-obvious at the XDR level why there's a split between SCVal and SCObject given
* that they are both immutable types with value semantics; but the split reflects the
* split that happens in the implementation, and marks a place where different
* implementations of immutability (CoW, structural sharing, etc.) will likely occur.
*
* (We may also change semantics in the future to have SCObjects be mutable after all)
*/

// A symbol is up to 10 chars drawn from [a-zA-Z0-9_], which can be packed into
// 60 bits with a 6-bit-per-character code, usable as a small key type to
// specify function, argument, tx-local environment and map entries efficiently.
typedef string SCSymbol<10>;

// SCLedgerValType enumerates all the stellar-ledger-specific types that
// are likely to be used as single scalar values (namely: map keys) in smart
// contracts but aren't LedgerKeys themselves.
enum SCLedgerValType
{
    SCLV_MUXEDACCOUNT = 0,
    SCLV_ASSET = 1,
    SCLV_AMOUNT = 2,
    SCLV_ASSET_AMOUNT = 3,
    SCLV_PRICE = 4,
    SCLV_TIMEPOINT = 5
    // TODO: add more
};

union SCLedgerVal switch (SCLedgerValType type) {
    case SCLV_MUXEDACCOUNT:
        MuxedAccount muxedAccountVal;
    case SCLV_ASSET:
        Asset assetVal;
    case SCLV_AMOUNT:
        int64 amountVal;
    case SCLV_ASSET_AMOUNT:
        struct {
            Asset assetVal;
            int64 amountVal;
        } assetAmountVal;
    case SCLV_PRICE:
        Price priceVal;
    case SCLV_TIMEPOINT:
        TimePoint timeVal;
};

enum SCValType
{
    SCV_U63 = 0,
    SCV_U32 = 1,
    SCV_I32 = 2,
    SCV_STATIC = 3,
    SCV_OBJECT = 4,
    SCV_SYMBOL = 5,
    SCV_BITSET = 6,
    SCV_STATUS = 7
 };

%struct SCObject;

enum SCStatic
{
    SCS_VOID = 0,
    SCS_TRUE = 1,
    SCS_FALSE = 2
};

enum SCStatusType
{
    SST_OK = 0,
    SST_UNKNOWN = 1,
    SST_WASM_TRAP_CODE = 2,
    SST_HOST_TRAP_CODE = 3,
    SST_PAYMENT_RESULT = 4,
    SST_INVOKE_CONTRACT_RESULT = 5
    // TODO: add more
};

%enum WasmTrapCode : int32_t;
%enum HostTrapCode : int32_t;
%enum PaymentResultCode : int32_t;
%enum InvokeContractResultCode : int32_t;

union SCStatus switch (SCStatusType type) {
    case SST_OK:
        void;
    case SST_WASM_TRAP_CODE:
        WasmTrapCode wasmTrap;
    case SST_HOST_TRAP_CODE:
        HostTrapCode hostTrap;
    case SST_PAYMENT_RESULT:
        PaymentResultCode paymentResult;
    case SST_INVOKE_CONTRACT_RESULT:
        InvokeContractResultCode invokeResult;
    case SST_UNKNOWN:
        void;
};

union SCVal switch (SCValType type) {
    case SCV_U63:
        uint64 u63;
    case SCV_U32:
        uint32 u32;
    case SCV_I32:
        int32 i32;
    case SCV_STATIC:
        SCStatic ic;
    case SCV_OBJECT:
        SCObject *obj;
    case SCV_SYMBOL:
        SCSymbol sym;
    case SCV_BITSET:
        uint64 bits;
    case SCV_STATUS:
        SCStatus status;
};

enum SCObjectType {
    SCO_BOX = 0,
    SCO_VEC = 1,
    SCO_MAP = 2,
    SCO_U64 = 3,
    SCO_I64 = 4,
    SCO_STRING = 5,
    SCO_BINARY = 6,
    SCO_LEDGERKEY = 7,
    SCO_LEDGERVAL = 8,
    SCO_OPERATION = 9,
    SCO_OPERATION_RESULT = 10,
    SCO_TRANSACTION = 11,
    SCO_BIGNUM = 12
};

struct SCMapEntry {
    SCVal key;
    SCVal val;
};


typedef SCVal SCVec<>;
typedef SCMapEntry SCMap<>;

struct SCBigNum {
    bool positive;
    opaque magnitude<>;
};

%struct Transaction;
%struct Operation;
%struct OperationResult;

union SCObject switch (SCObjectType type) {
    case SCO_BOX:
        SCVal box;
    case SCO_VEC:
        SCVec vec;
    case SCO_MAP:
        SCMap map;
    case SCO_U64:
        uint64 u64;
    case SCO_I64:
        int64 i64;
    case SCO_STRING:
        string str<>;
    case SCO_BINARY:
        opaque bin<>;
    case SCO_LEDGERKEY:
        LedgerKey lkey;
    case SCO_LEDGERVAL:
        SCLedgerVal lval;
    case SCO_OPERATION:
        Operation *op;
    case SCO_OPERATION_RESULT:
        OperationResult *ores;
    case SCO_TRANSACTION:
        Transaction *tx;
    case SCO_BIGNUM:
        SCBigNum bn;
};

struct SCEnvEntry {
    SCSymbol key;
    SCVal val;
};
typedef SCEnvEntry SCEnv<>;

}