#include "transactions/contracts/HostVal.h"
#include "crypto/ShortHash.h"
#include "src/ledger/LedgerHashUtils.h"
#include "src/util/XDROperators.h"
#include "transactions/contracts/HostContext.h"
#include <Tracy.hpp>
#include <algorithm>
#include <boost/multiprecision/rational_adaptor.hpp>
#include <variant>
#include <vector>

namespace stellar
{

std::string
HostVal::asSymbol() const
{
    ZoneScoped;
    size_t MAXCHARS = SCSymbol::max_size();
    size_t MAXBITS = MAXCHARS * 6;
    static char dict[64] =
        "_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string out;
    out.reserve(MAXCHARS);
    auto tmp = getBody();
    for (size_t off = 6; off <= MAXBITS; off += 6)
    {
        auto idx = (tmp >> (MAXBITS - off)) & 63;
        if (idx != 0)
        {
            out += dict[idx - 1];
        }
    }
    return out;
}

HostVal
HostVal::fromSymbol(std::string const& s)
{
    ZoneScoped;
    size_t MAXSZ = SCSymbol::max_size();
    if (s.size() > MAXSZ)
    {
        throw std::runtime_error("bad symbol size");
    }
    uint64_t accum = 0;
    for (auto const c : s)
    {
        accum <<= 6;
        if (c == '_')
        {
            // 1 is underscore
            accum |= 1;
        }
        else if ('0' <= c && c <= '9')
        {
            // 2-11 inclusive are digits
            accum |= 2 + (c - '0');
        }
        else if ('A' <= c && c <= 'Z')
        {
            // 12-37 inclusive are uppercase
            accum |= 12 + (c - 'A');
        }
        else if ('a' <= c && c <= 'z')
        {
            // 38-63 inclusive are lowercase
            accum |= 38 + (c - 'a');
        }
        else
        {
            throw std::runtime_error("bad symbol char");
        };
    }
    return fromBodyAndTag(accum, TAG_SYMBOL);
}

std::ostream&
operator<<(std::ostream& out, HostVal const& v)
{
    ZoneScoped;
    if (v.isU63())
    {
        out << "u63(" << v.asU63() << ")";
    }
    else if (v.isVoid())
    {
        out << "void";
    }
    else if (v.isBool())
    {
        out << "bool(" << v.asBool() << ')';
    }
    else if (v.isStatus())
    {
        auto pair = v.asStatusTypeAndCode();
        out << "status(" << pair.first << "," << pair.second << ')';
    }
    else if (v.isU32())
    {
        out << "u32(" << v.asU32() << ')';
    }
    else if (v.isI32())
    {
        out << "i32(" << v.asI32() << ')';
    }
    else if (v.isSymbol())
    {
        out << "sym(" << v.asSymbol() << ')';
    }
    else if (v.isBitSet())
    {
        out << "bitset(";
        auto tmp = v.asBitSet();
        for (auto i = 0; i < 48; ++i)
        {
            out << (tmp & 1 ? '1' : '0');
            tmp >>= 1;
        }
        out << ')';
    }
    else if (v.isObject())
    {
        out << "obj(" << v.asObject() << ')';
    }
    else
    {
        auto payload = v.payload();
        out << "unknown(" << payload << ",0x" << std::hex << payload << ')';
    }
    return out;
}

struct HashHostObjectVisitor
{
    size_t
    operator()(HostBox const& box) const
    {
        return std::hash<HostValInContext>()(box.get());
    }

    size_t
    operator()(HostVec const& vec) const
    {
        size_t h{0};
        for (auto const& v : vec)
        {
            hashMix(h, std::hash<HostValInContext>()(v));
        }
        return h;
    }

    size_t
    operator()(HostMap const& map) const
    {
        size_t h{0};
        for (auto const& pair : map)
        {
            hashMix(h, std::hash<HostValInContext>()(pair.first));
            hashMix(h, std::hash<HostValInContext>()(pair.second));
        }
        return h;
    }

    size_t
    operator()(uint64_t const& u) const
    {
        return shortHash::xdrComputeHash(u);
    }

    size_t
    operator()(int64_t const& i) const
    {
        return shortHash::xdrComputeHash(i);
    }

    size_t
    operator()(xdr::xstring<> const& str) const
    {
        return shortHash::computeHash(ByteSlice(str));
    }

    size_t
    operator()(xdr::xvector<uint8_t> const& blob) const
    {
        return shortHash::computeHash(ByteSlice(blob));
    }

    size_t
    operator()(HostBigInt const& big) const
    {
        std::vector<uint8_t> tmp;
        auto bi = std::back_inserter(tmp);
        boost::multiprecision::export_bits(big, bi, 8);
        return shortHash::computeHash(ByteSlice(tmp));
    }

    size_t
    operator()(HostBigRat const& big) const
    {
        std::vector<uint8_t> tmp;
        auto bi = std::back_inserter(tmp);
        boost::multiprecision::export_bits(
            boost::multiprecision::numerator(big), bi, 8);
        boost::multiprecision::export_bits(
            boost::multiprecision::denominator(big), bi, 8);
        return shortHash::computeHash(ByteSlice(tmp));
    }

    size_t
    operator()(xdr::pointer<LedgerKey> const& lk) const
    {
        return shortHash::xdrComputeHash(lk);
    }

    size_t
    operator()(xdr::pointer<Operation> const& op) const
    {
        return shortHash::xdrComputeHash(op);
    }

    size_t
    operator()(xdr::pointer<OperationResult> const& ores) const
    {
        return shortHash::xdrComputeHash(ores);
    }

    size_t
    operator()(xdr::pointer<Transaction> const& tx) const
    {
        return shortHash::xdrComputeHash(tx);
    }

    size_t
    operator()(Asset const& a) const
    {
        return shortHash::xdrComputeHash(a);
    }

    size_t
    operator()(Price const& p) const
    {
        return shortHash::xdrComputeHash(p);
    }

    size_t
    operator()(AccountID const& a) const
    {
        return shortHash::xdrComputeHash(a);
    }
};

size_t
HostValInContext::getHash() const
{
    ZoneScoped;
    if (mVal.isU63())
    {
        return std::hash<uint64_t>()(mVal.asU63());
    }
    if (mVal.isObject())
    {
        releaseAssert(mCtx);
        std::unique_ptr<HostObject const> const& ptr = mCtx->getObject(mVal);
        if (!ptr)
        {
            return 0;
        }
        else
        {
            HashHostObjectVisitor hhov;
            return std::visit(hhov, *ptr);
        }
    }
    return std::hash<uint64_t>()(mVal.payload());
}

struct EqHostObjectVisitor
{
    HostObject const& mOther;

    template <typename T>
    bool
    eqOther(T const& v) const
    {
        releaseAssert(std::holds_alternative<T>(mOther));
        return v == std::get<T>(mOther);
    }

    template <typename T>
    bool
    eqOtherPtr(xdr::pointer<T> const& p) const
    {
        releaseAssert(!std::holds_alternative<xdr::pointer<T>>(mOther));
        auto const& otherP = std::get<xdr::pointer<T>>(mOther);
        if (p.get() == otherP.get())
        {
            return true;
        }
        if (p && otherP)
        {
            return *p == *otherP;
        }
        return false;
    }

    bool
    operator()(HostBox const& box) const
    {
        return eqOther(box);
    }

    bool
    operator()(HostVec const& vec) const
    {
        return eqOther(vec);
    }

    bool
    operator()(HostMap const& map) const
    {
        return eqOther(map);
    }

    bool
    operator()(uint64_t const& u) const
    {
        return eqOther(u);
    }

    bool
    operator()(int64_t const& i) const
    {
        return eqOther(i);
    }

    bool
    operator()(xdr::xstring<> const& str) const
    {
        return eqOther(str);
    }

    bool
    operator()(xdr::xvector<uint8_t> const& blob) const
    {
        return eqOther(blob);
    }

    bool
    operator()(HostBigInt const& big) const
    {
        return eqOther(big);
    }

    bool
    operator()(HostBigRat const& big) const
    {
        return eqOther(big);
    }

    bool
    operator()(xdr::pointer<LedgerKey> const& lk) const
    {
        return eqOtherPtr(lk);
    }

    bool
    operator()(xdr::pointer<Operation> const& op) const
    {
        return eqOtherPtr(op);
    }

    bool
    operator()(xdr::pointer<OperationResult> const& ores) const
    {
        return eqOtherPtr(ores);
    }

    bool
    operator()(xdr::pointer<Transaction> const& tx) const
    {
        return eqOtherPtr(tx);
    }

    bool
    operator()(Asset const& a) const
    {
        return eqOther(a);
    }

    bool
    operator()(Price const& p) const
    {
        return eqOther(p);
    }

    bool
    operator()(AccountID const& a) const
    {
        return eqOther(a);
    }
};

bool
HostValInContext::operator==(HostValInContext const& other) const
{
    ZoneScoped;
    releaseAssert(mCtx);
    releaseAssert(mCtx == other.mCtx);
    if (mVal.payload() == other.mVal.payload())
    {
        // Fast path: bitwise identical.
        return true;
    }
    if (mVal.isObject() && other.mVal.isObject())
    {
        releaseAssert(mCtx);
        std::unique_ptr<HostObject const> const& ptr = mCtx->getObject(mVal);
        std::unique_ptr<HostObject const> const& otherPtr =
            mCtx->getObject(other.mVal);
        if (!ptr || !otherPtr)
        {
            // Sub-fast-path: one or the other nullptr.
            return !ptr && !otherPtr;
        }
        // Catch unequal-variants first.
        auto idx = ptr->index();
        auto otherIdx = otherPtr->index();
        if (idx != otherIdx)
        {
            return false;
        }
        // Deep compare same-variant objects.
        EqHostObjectVisitor ehov{*otherPtr};
        return std::visit(ehov, *ptr);
    }
    // Anything that wasn't shallow-identical and isn't an object
    // isn't equal.
    return false;
}

struct LtHostObjectVisitor
{
    HostObject const& mOther;

    template <typename T>
    bool
    ltOther(T const& v) const
    {
        releaseAssert(std::holds_alternative<T>(mOther));
        return v < std::get<T>(mOther);
    }

    template <typename T>
    bool
    ltOtherPtr(xdr::pointer<T> const& p) const
    {
        releaseAssert(std::holds_alternative<xdr::pointer<T>>(mOther));
        auto const& otherP = std::get<xdr::pointer<T>>(mOther);
        if (p.get() == otherP.get())
        {
            return false;
        }
        if (p && !otherP)
        {
            return false;
        }
        else if (!p && otherP)
        {
            return true;
        }
        else if (!p && !otherP)
        {
            return false;
        }
        else
        {
            return *p < *otherP;
        }
    }

    bool
    operator()(HostBox const& box) const
    {
        return ltOther(box);
    }

    bool
    operator()(HostVec const& vec) const
    {
        releaseAssert(std::holds_alternative<HostVec>(mOther));
        auto const& otherVec = std::get<HostVec>(mOther);
        return std::lexicographical_compare(vec.begin(), vec.end(),
                                            otherVec.begin(), otherVec.end());
    }

    bool
    operator()(HostMap const& map) const
    {
        releaseAssert(std::holds_alternative<HostMap>(mOther));
        auto const& otherMap = std::get<HostMap>(mOther);
        return std::lexicographical_compare(map.begin(), map.end(),
                                            otherMap.begin(), otherMap.end());
    }

    bool
    operator()(uint64_t const& u) const
    {
        return ltOther(u);
    }

    bool
    operator()(int64_t const& i) const
    {
        return ltOther(i);
    }

    bool
    operator()(xdr::xstring<> const& str) const
    {
        return ltOther(str);
    }

    bool
    operator()(xdr::xvector<uint8_t> const& blob) const
    {
        return ltOther(blob);
    }

    bool
    operator()(HostBigInt const& big) const
    {
        return ltOther(big);
    }

    bool
    operator()(HostBigRat const& big) const
    {
        return ltOther(big);
    }

    bool
    operator()(xdr::pointer<LedgerKey> const& lk) const
    {
        return ltOtherPtr(lk);
    }

    bool
    operator()(xdr::pointer<Operation> const& op) const
    {
        return ltOtherPtr(op);
    }

    bool
    operator()(xdr::pointer<OperationResult> const& ores) const
    {
        return ltOtherPtr(ores);
    }

    bool
    operator()(xdr::pointer<Transaction> const& tx) const
    {
        return ltOtherPtr(tx);
    }

    bool
    operator()(Asset const& a) const
    {
        return ltOther(a);
    }

    bool
    operator()(Price const& p) const
    {
        return ltOther(p);
    }

    bool
    operator()(AccountID const& a) const
    {
        return ltOther(a);
    }
};

bool
HostValInContext::operator<(HostValInContext const& other) const
{
    ZoneScoped;
    if (mVal.isU63())
    {
        if (other.mVal.isU63())
        {
            return mVal.asU63() < other.mVal.asU63();
        }
        else
        {
            return true;
        }
    }
    else if (other.mVal.isU63())
    {
        return false;
    }
    else if (mVal.getTag() != other.mVal.getTag())
    {
        return mVal.getTag() < other.mVal.getTag();
    }

    // We have the same tag, examine sub-cases.
    if (mVal.isU32())
    {
        return mVal.asU32() < other.mVal.asU32();
    }
    else if (mVal.isI32())
    {
        return mVal.asI32() < other.mVal.asI32();
    }
    else if (mVal.isVoid())
    {
        return false;
    }
    else if (mVal.isBool())
    {
        return mVal.asBool() < other.mVal.asBool();
    }
    else if (mVal.isObject())
    {
        // Objects require deep comparison.
        releaseAssert(mCtx);
        auto const& ptr = mCtx->getObject(mVal);
        auto const& otherPtr = mCtx->getObject(mVal);
        if (!ptr && otherPtr)
        {
            return true;
        }
        else if (ptr && !otherPtr)
        {
            return false;
        }
        else if (!ptr && !otherPtr)
        {
            return false;
        }
        else
        {
            // Order by object-variant first.
            auto idx = ptr->index();
            auto otherIdx = ptr->index();
            if (idx != otherIdx)
            {
                return idx < otherIdx;
            }
            // Deep-compare if they're the same variant.
            LtHostObjectVisitor lhov{*otherPtr};
            return std::visit(lhov, *ptr);
        }
    }
    else if (mVal.isSymbol())
    {
        return mVal.asSymbol() < other.mVal.asSymbol();
    }
    else if (mVal.isBitSet())
    {
        return mVal.asBitSet() < other.mVal.asBitSet();
    }
    else if (mVal.isStatus())
    {
        return mVal.asStatusTypeAndCode() < other.mVal.asStatusTypeAndCode();
    }
    else
    {
        throw std::runtime_error("unknown SCVal type in operator<");
    }
}

}