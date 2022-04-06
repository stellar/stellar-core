#include "transactions/contracts/HostVal.h"

namespace stellar
{

std::string
HostVal::asSymbol() const
{
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
        auto pair = v.asStatus();
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

}