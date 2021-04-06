#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "cereal/cereal.hpp"
#include <memory>

namespace stellar
{
template <typename T> using optional = std::shared_ptr<T>;

template <typename T, class... Args>
optional<T>
make_optional(Args&&... args)
{
    return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T>
optional<T>
nullopt()
{
    return std::shared_ptr<T>();
};
}

namespace cereal
{
template <class Archive, class T>
void
save(Archive& ar, stellar::optional<T> const& opt)
{
    ar(make_nvp("has", !!opt));
    if (opt)
    {
        ar(make_nvp("val", *opt));
    }
}

template <class Archive, class T>
void
load(Archive& ar, stellar::optional<T>& o)
{
    bool isSet;
    o.reset();
    ar(make_nvp("has", isSet));
    if (isSet)
    {
        T v;
        ar(make_nvp("val", v));
        o = stellar::make_optional<T>(v);
    }
}
} // namespace cereal
