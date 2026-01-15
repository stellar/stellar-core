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

pub(crate) fn current_exe() -> Result<String, Box<dyn std::error::Error>> {
    std::env::current_exe()?
        .into_os_string()
        .into_string()
        .map_err(|e| format!("Failed to convert path to string: {:?}", e).into())
}

fn compare_xdr_files_sha256(
    crate1: &str,
    files1: &[(&str, &str)],
    crate2: &str,
    files2: &[(&str, &str)],
) -> Result<(), Box<dyn std::error::Error>> {
    if files1.len() != files2.len() {
        return Err(format!("XDR files count mismatch between {} and {}", crate1, crate2).into());
    }
    for (file1, sha1) in files1 {
        if let Some((_, sha2)) = files2.iter().find(|(f, _)| f == file1) {
            if sha1 != sha2 {
                return Err(
                    format!("XDR file {} SHA256 mismatch: {} != {}", file1, sha1, sha2).into(),
                );
            }
        } else {
            return Err(format!("XDR file {} not found in {}", file1, crate2).into());
        }
    }
    Ok(())
}

pub(crate) fn check_xdr_version_identities() -> Result<(), Box<dyn std::error::Error>> {
    compare_xdr_files_sha256(
        "stellar_quorum_analyzer",
        &stellar_quorum_analyzer::xdr::curr::XDR_FILES_SHA256,
        "soroban_env_curr",
        &crate::soroban_proto_all::soroban_curr::soroban_env_host::xdr::XDR_FILES_SHA256,
    )?;
    // Add more comparisons between XDR file lists as needed
    Ok(())
}
