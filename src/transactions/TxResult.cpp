#include "transactions/TxResult.h"

/*

txResult['id']=
txResult['code']=
txResult['effects']['delete']=
txResult['effects']['new']=
txResult['effects']['mod']=

result
    TxID
    result code
    effects
        delete
            EntryID
            ...
        new
            EntryXDR
            ...
        mod
            EntryID
                fieldName : new value
                ...
            ...




*/
