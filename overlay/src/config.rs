//! Configuration for the overlay process.

use serde::Deserialize;
use std::path::PathBuf;

/// Overlay configuration.
#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct Config {
    /// Path to Core IPC socket
    pub core_socket: PathBuf,

    /// IP address for libp2p QUIC to bind to (e.g., "127.0.0.1" for local tests).
    /// Using a specific IP avoids Identify protocol advertising all local IPs,
    /// which can cause connection churn in test networks.
    pub libp2p_listen_ip: String,

    /// Peer port (used when generating listen address dynamically)
    pub peer_port: u16,

    /// Log level
    pub log_level: String,

    /// Target payload bytes per TX set shard. The overlay may increase this to
    /// keep the original + recovery shard count below 256.
    pub txset_target_shard_size: usize,

    /// Recovery shards as a percentage of original shards.
    pub txset_shard_recovery_factor_percent: usize,

    /// Initial shard rebroadcast TTL.
    pub txset_shard_ttl: u8,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            core_socket: PathBuf::from("/tmp/stellar-overlay.sock"),
            libp2p_listen_ip: "0.0.0.0".to_string(), // Bind to all interfaces for internet operation
            peer_port: 11625,
            log_level: "info".to_string(),
            txset_target_shard_size: 1024,
            txset_shard_recovery_factor_percent: 50,
            txset_shard_ttl: 1,
        }
    }
}

impl Config {
    /// Load config from TOML file
    pub fn from_file(path: &std::path::Path) -> Result<Self, ConfigError> {
        let content = std::fs::read_to_string(path).map_err(|e| ConfigError::Io(e))?;
        Self::from_str(&content)
    }

    /// Parse config from TOML string
    pub fn from_str(content: &str) -> Result<Self, ConfigError> {
        toml::from_str(content).map_err(|e| ConfigError::Parse(e.to_string()))
    }
}

#[derive(Debug)]
pub enum ConfigError {
    Io(std::io::Error),
    Parse(String),
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConfigError::Io(e) => write!(f, "config I/O error: {}", e),
            ConfigError::Parse(e) => write!(f, "config parse error: {}", e),
        }
    }
}

impl std::error::Error for ConfigError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = Config::default();
        assert_eq!(
            config.core_socket,
            PathBuf::from("/tmp/stellar-overlay.sock")
        );
        assert_eq!(config.peer_port, 11625);
        assert_eq!(config.log_level, "info");
        assert_eq!(config.txset_target_shard_size, 1024);
        assert_eq!(config.txset_shard_recovery_factor_percent, 50);
        assert_eq!(config.txset_shard_ttl, 1);
    }

    #[test]
    fn test_parse_config() {
        let toml = r#"
            core_socket = "/var/run/stellar.sock"
            libp2p_listen_ip = "127.0.0.1"
            peer_port = 12625
            log_level = "debug"
            txset_target_shard_size = 2048
            txset_shard_recovery_factor_percent = 75
            txset_shard_ttl = 2
        "#;

        let config = Config::from_str(toml).unwrap();
        assert_eq!(config.core_socket, PathBuf::from("/var/run/stellar.sock"));
        assert_eq!(config.libp2p_listen_ip, "127.0.0.1");
        assert_eq!(config.peer_port, 12625);
        assert_eq!(config.log_level, "debug");
        assert_eq!(config.txset_target_shard_size, 2048);
        assert_eq!(config.txset_shard_recovery_factor_percent, 75);
        assert_eq!(config.txset_shard_ttl, 2);
    }

    // ═══ Parse Error Cases ═══

    #[test]
    fn test_parse_invalid_toml() {
        let toml = "not valid { toml";
        let result = Config::from_str(toml);
        assert!(result.is_err());
        assert!(matches!(result.unwrap_err(), ConfigError::Parse(_)));
    }

    #[test]
    fn test_parse_wrong_type() {
        let toml = r#"peer_port = "eight""#;
        let result = Config::from_str(toml);
        assert!(result.is_err());
    }

    // ═══ Partial Config (uses defaults) ═══

    #[test]
    fn test_partial_config_uses_defaults() {
        let toml = r#"peer_port = 12625"#;
        let config = Config::from_str(toml).unwrap();

        // Specified value
        assert_eq!(config.peer_port, 12625);
        // Default values
        assert_eq!(
            config.core_socket,
            PathBuf::from("/tmp/stellar-overlay.sock")
        );
        assert_eq!(config.log_level, "info");
    }

    #[test]
    fn test_empty_config_uses_defaults() {
        let toml = "";
        let config = Config::from_str(toml).unwrap();

        assert_eq!(config.peer_port, 11625);
        assert_eq!(config.log_level, "info");
    }
}
