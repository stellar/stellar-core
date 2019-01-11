// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <lib/util/basen.h>
#include <string>

namespace stellar
{

namespace decoder
{

inline size_t
encoded_size32(size_t rawsize)
{
    return ((rawsize + 4) / 5 * 8);
}

inline size_t
encoded_size64(size_t rawsize)
{
    return ((rawsize + 2) / 3 * 4);
}

template <class T>
inline std::string
encode_b32(T const& v)
{
    std::string res;
    res.reserve(encoded_size32(v.size() * sizeof(typename T::value_type)) + 1);
    bn::encode_b32(v.begin(), v.end(), std::back_inserter(res));
    return res;
}

template <class T>
inline std::string
encode_b64(T const& v)
{
    std::string res;
    res.reserve(encoded_size64(v.size() * sizeof(typename T::value_type)) + 1);
    bn::encode_b64(v.begin(), v.end(), std::back_inserter(res));
    return res;
}

template <class V, class T>
inline void
decode_b32(V const& v, T& out)
{
    out.clear();
    out.reserve(v.size() * sizeof(typename T::value_type));
    bn::decode_b32(v.begin(), v.end(), std::back_inserter(out));
}

template <class V, class T>
inline void
decode_b64(V const& v, T& out)
{
    out.clear();
    out.reserve(v.size() * sizeof(typename T::value_type));
    bn::decode_b64(v.begin(), v.end(), std::back_inserter(out));
}

template <class Iter1, class Iter2>
inline void
decode_b64(Iter1 start, Iter1 end, Iter2 out)
{
    bn::decode_b64(start, end, out);
}
}
}
