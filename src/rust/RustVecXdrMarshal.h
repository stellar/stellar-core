#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// You should include this header if you want to un-marshal XDR types from the
// C++ type rust::Vec<T> declared by cxx.rs.
//
// This is a good thing to do especially if you're returning serialized XDR over
// the rust bridge, since most Rust code is doing to produce a rust::Vec, not a
// std::vector / CxxVector. Avoids more redundant byte-copies.

#include "rust/RustBridge.h"

namespace xdr
{
namespace detail
{
// This just tells xdrpp that it's safe to use rust::Vec<uint8_t> as a byte
// buffer type. It has to be declared _before_ we include xdrpp/marshal.h
void bytes_to_void(const rust::Vec<uint8_t>&);
}
}

#include <xdrpp/xdrpp/marshal.h>
