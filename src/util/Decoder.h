// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <lib/util/basen.h>

namespace stellar
{

namespace decoder
{

template <class T>
inline std::string
encode_b32(T const& v)
{
    return bn::encode_b32(v);
}

template <class T>
inline std::string
encode_b64(T const& v)
{
    return bn::encode_b64(v);
}

inline size_t
encoded_size32(size_t rawsize)
{
    return bn::encoded_size32(rawsize);
}

inline size_t
encoded_size64(size_t rawsize)
{
    return bn::encoded_size64(rawsize);
}

template <class V, class T>
inline void
decode_b32(V const& v, T& out)
{
    bn::decode_b32(v, out);
}

template <class V, class T>
inline void
decode_b64(V const& v, T& out)
{
    bn::decode_b64(v, out);
}

template <class Iter1, class Iter2>
inline void
decode_b64(Iter1 start, Iter1 end, Iter2 out)
{
    bn::decode_b64(start, end, out);
}
}
}
