#pragma once
#include "main/Application.h"
#include <string>

namespace stellar
{
using namespace std;

class PersistentState
{
public:
    PersistentState(Application &app) : mApp(app) {}

    enum Entry {
        kLastClosedLedger = 0,
        kNewNetworkOnNextLunch,
        kLastEntry
    };

    static void dropAll(Database &db);

    string getStoreStateName(Entry n);

    string getState(Entry stateName);
  
    void setState(Entry stateName, const string &value);

private:
    static const char *kSQLCreateStatement;

    Application &mApp;
};



}
