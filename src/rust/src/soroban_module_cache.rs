// The SorobanModuleCache needs to hold a different protocol-specific cache for
// each supported protocol version it's going to be used with. It has to hold
// all these caches _simultaneously_ because it might perform an upgrade from
// protocol N to protocol N+1 in a single transaction, and needs to be ready for
// that before it happens.
//
// Most of these caches can be empty at any given time, because we're not
// expecting core to need to replay old protocols, and/or if it does it's during
// replay and there's no problem stalling while filling a cache with new entries
// on a per-ledger basis as they are replayed.
//
// But for the current protocol version we need to have a cache ready to execute
// anything thrown at it once it's in sync, so we should prime the
// current-protocol cache as soon as we start, as well as the next-protocol
// cache (if it exists) so that we can upgrade without stalling.

use crate::{
    rust_bridge::CxxBuf,
    soroban_proto_all::{get_host_module_for_protocol, p23, protocol_agnostic},
};

pub(crate) struct SorobanModuleCache {
    pub(crate) p23_cache: p23::soroban_proto_any::ProtocolSpecificModuleCache,
}

impl SorobanModuleCache {
    fn new() -> Result<Self, Box<dyn std::error::Error>> {
        Ok(Self {
            p23_cache: p23::soroban_proto_any::ProtocolSpecificModuleCache::new()?,
        })
    }
    pub fn compile(
        &mut self,
        ledger_protocol: u32,
        _wasm: &[u8],
    ) -> Result<(), Box<dyn std::error::Error>> {
        match ledger_protocol {
            23 => self.p23_cache.compile(_wasm),
            // Currently p24 uses the same env as p23 and thus can keep using
            // p23 module cache.
            #[cfg(feature = "next")]
            24 => self.p23_cache.compile(_wasm),
            // Add other protocols here as needed.
            _ => Err(protocol_agnostic::make_error("unsupported protocol")),
        }
    }
    pub fn shallow_clone(&self) -> Result<Box<Self>, Box<dyn std::error::Error>> {
        Ok(Box::new(Self {
            p23_cache: self.p23_cache.shallow_clone()?,
        }))
    }

    pub fn evict_contract_code(&mut self, key: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        let _hash: [u8; 32] = key
            .as_ref()
            .try_into()
            .map_err(|_| "Invalid contract-code key length")?;
        self.p23_cache.evict(&_hash)?;
        Ok(())
    }
    pub fn clear(&mut self) -> Result<(), Box<dyn std::error::Error>> {
        self.p23_cache.clear()?;
        Ok(())
    }

    pub fn contains_module(
        &self,
        protocol: u32,
        key: &[u8],
    ) -> Result<bool, Box<dyn std::error::Error>> {
        let _hash: [u8; 32] = key
            .as_ref()
            .try_into()
            .map_err(|_| "Invalid contract-code key length")?;
        match protocol {
            23 => self.p23_cache.contains_module(&_hash),
            #[cfg(feature = "next")]
            24 => self.p23_cache.contains_module(&_hash),
            _ => Err(protocol_agnostic::make_error("unsupported protocol")),
        }
    }
    pub fn get_mem_bytes_consumed(&self) -> Result<u64, Box<dyn std::error::Error>> {
        #[allow(unused_mut)]
        let mut bytes = 0;
        bytes = bytes.max(self.p23_cache.get_mem_bytes_consumed()?);
        Ok(bytes)
    }
}

pub(crate) fn new_module_cache() -> Result<Box<SorobanModuleCache>, Box<dyn std::error::Error>> {
    Ok(Box::new(SorobanModuleCache::new()?))
}

pub(crate) fn contract_code_memory_size_for_rent(
    config_max_protocol: u32,
    protocol_version: u32,
    contract_code_entry: &CxxBuf,
    cpu_cost_params: &CxxBuf,
    mem_cost_params: &CxxBuf,
) -> Result<u32, Box<dyn std::error::Error>> {
    let hm = get_host_module_for_protocol(config_max_protocol, protocol_version)?;
    (hm.contract_code_memory_size_for_rent)(contract_code_entry, cpu_cost_params, mem_cost_params)
}
