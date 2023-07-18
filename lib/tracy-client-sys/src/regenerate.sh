#!/bin/sh
bindgen "../../tracy/public/tracy/TracyC.h"   -o 'generated.rs'   --allowlist-function='.*[Tt][Rr][Aa][Cc][Yy].*'   --allowlist-type='.*[Tt][Rr][Aa][Cc][Yy].*'  -- -DTRACY_ENABLE -DTRACY_MANUAL_LIFETIME -DTRACY_FIBERS
sed -i 's/pub type/type/g' 'generated.rs'
