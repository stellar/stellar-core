
/*
This is a delta that is sent by peers to catch other peers up to the network

*/

namespace stellar
{
    class CLFDelta
    {
        std::vector<LedgerEntry> mEntries;
    public:
        typedef std::shared_ptr<CLFDelta> pointer;
    };
}