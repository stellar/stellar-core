use crate::{BridgeError, CxxBuf, RustBuf};

impl From<Vec<u8>> for RustBuf {
    fn from(value: Vec<u8>) -> Self {
        Self { data: value }
    }
}

impl AsRef<[u8]> for CxxBuf {
    fn as_ref(&self) -> &[u8] {
        self.data.as_slice()
    }
}

impl CxxBuf {
    #[cfg(feature = "testutils")]
    pub(crate) fn replace_data_with(
        &mut self,
        slice: &[u8],
    ) -> Result<(), Box<dyn std::error::Error>> {
        if self.data.is_null() {
            return Err("CxxBuf::replace_data_with: data is null".into());
        }
        while self.data.len() > 0 {
            self.data.pin_mut().pop();
        }
        for byte in slice {
            self.data.pin_mut().push(*byte);
        }
        Ok(())
    }
}

impl std::fmt::Display for BridgeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl std::error::Error for BridgeError {}

pub(crate) fn get_rustc_version() -> String {
    rustc_simple_version::RUSTC_VERSION.to_string()
}
