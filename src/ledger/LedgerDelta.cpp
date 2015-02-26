#include "ledger/LedgerDelta.h"
#include "main/Application.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"

namespace stellar
{
    void LedgerDelta::addEntry(EntryFrame const& entry)
    {
        addEntry(entry.copy());
    }

    void LedgerDelta::deleteEntry(EntryFrame const& entry)
    {
        deleteEntry(entry.copy());
    }

    void LedgerDelta::modEntry(EntryFrame const& entry)
    {
        modEntry(entry.copy());
    }

    void LedgerDelta::addEntry(EntryFrame::pointer entry)
    {
        auto k = entry->getKey();
        auto del_it = mDelete.find(k);
        if (del_it != mDelete.end())
        {
            // delete + new is an update
            mDelete.erase(del_it);
            mMod[k] = entry;
        }
        else
        {
            assert(mNew.find(k) == mNew.end()); // double new
            assert(mMod.find(k) == mMod.end()); // mod + new is invalid
            mNew[k] = entry;
        }
    }

    void LedgerDelta::deleteEntry(EntryFrame::pointer entry)
    {
        auto k = entry->getKey();
        deleteEntry(k);
    }

    void LedgerDelta::deleteEntry(LedgerKey const& k)
    {
        auto new_it = mNew.find(k);
        if (new_it != mNew.end())
        {
            // new + delete -> don't add it in the first place
            mNew.erase(new_it);
        }
        else
        {
            assert(mDelete.find(k) == mDelete.end()); // double delete is invalid
            // only keep the delete
            mMod.erase(k);
            mDelete.insert(k);
        }
    }

    void LedgerDelta::modEntry(EntryFrame::pointer entry)
    {
        auto k = entry->getKey();
        auto mod_it = mMod.find(k);
        if ( mod_it != mMod.end())
        {
            // collapse mod
            mod_it->second = entry;
        }
        else
        {
            auto new_it = mNew.find(k);
            if (new_it != mNew.end())
            {
                // new + mod = new (with latest value)
                new_it->second = entry;
            }
            else
            {
                assert(mDelete.find(k) == mDelete.end()); // delete + mod is illegal
                mMod[k] = entry;
            }
        }
    }

    void LedgerDelta::merge(LedgerDelta &other)
    {
        for (auto &d : other.mDelete)
        {
            deleteEntry(d);
        }
        for (auto &n : other.mNew)
        {
            addEntry(n.second);
        }
        for (auto &m : other.mMod)
        {
            modEntry(m.second);
        }
    }

    xdr::msg_ptr LedgerDelta::getTransactionMeta() const
    {
        TransactionMeta tm;

        for (auto const &k : mNew)
        {
            tm.entries.emplace_back(LIVEENTRY);
            tm.entries.back().liveEntry() = k.second->mEntry;
        }
        for (auto const &k : mMod)
        {
            tm.entries.emplace_back(LIVEENTRY);
            tm.entries.back().liveEntry() = k.second->mEntry;
        }

        for (auto const &k : mDelete)
        {
            tm.entries.emplace_back(DEADENTRY);
            tm.entries.back().deadEntry() = k;
        }

        return xdr::xdr_to_msg(tm);
    }

    std::vector<LedgerEntry> LedgerDelta::getLiveEntries() const
    {
        std::vector<LedgerEntry> live;
        
        live.reserve(mNew.size() + mMod.size());

        for (auto const &k : mNew)
        {
            live.push_back(k.second->mEntry);
        }
        for (auto const &k : mMod)
        {
            live.push_back(k.second->mEntry);
        }

        return live;
    }

    std::vector<LedgerKey> LedgerDelta::getDeadEntries() const
    {
        std::vector<LedgerKey> dead;
        
        dead.reserve(mDelete.size());

        for (auto const &k : mDelete)
        {
            dead.push_back(k);
        }
        return dead;
    }


    void
    LedgerDelta::markMeters(Application& app) const
    {
        for (auto const& ke : mNew)
        {
            switch (ke.first.type())
            {
            case ACCOUNT:
                app.getMetrics().NewMeter({"ledger", "account", "add"}, "entry").Mark();
                break;
            case TRUSTLINE:
                app.getMetrics().NewMeter({"ledger", "trust", "add"}, "entry").Mark();
                break;
            case OFFER:
                app.getMetrics().NewMeter({"ledger", "offer", "add"}, "entry").Mark();
                break;
            }
        }

        for (auto const& ke : mMod)
        {
            switch (ke.first.type())
            {
            case ACCOUNT:
                app.getMetrics().NewMeter({"ledger", "account", "modify"}, "entry").Mark();
                break;
            case TRUSTLINE:
                app.getMetrics().NewMeter({"ledger", "trust", "modify"}, "entry").Mark();
                break;
            case OFFER:
                app.getMetrics().NewMeter({"ledger", "offer", "modify"}, "entry").Mark();
                break;
            }
        }

        for (auto const& ke : mDelete)
        {
            switch (ke.type())
            {
            case ACCOUNT:
                app.getMetrics().NewMeter({"ledger", "account", "delete"}, "entry").Mark();
                break;
            case TRUSTLINE:
                app.getMetrics().NewMeter({"ledger", "trust", "delete"}, "entry").Mark();
                break;
            case OFFER:
                app.getMetrics().NewMeter({"ledger", "offer", "delete"}, "entry").Mark();
                break;
            }
        }

    }

}
