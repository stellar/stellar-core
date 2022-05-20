#!/bin/sh
bindgen "../../tracy/TracyC.h"   -o 'generated.rs'   --whitelist-function='.*[Tt][Rr][Aa][Cc][Yy].*'   --whitelist-type='.*[Tt][Rr][Aa][Cc][Yy].*'   --size_t-is-usize -- -DTRACY_ENABLE -DTRACY_MANUAL_LIFETIME -DTRACY_FIBERS
sed -i 's/pub type/type/g' 'generated.rs'
