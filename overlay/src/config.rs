//! Configuration for the overlay process.

use serde::Deserialize;
use std::net::SocketAddr;
use std::path::PathBuf;

/// Overlay configuration.
#[derive(Debug, Clone, Deserialize)]
#[serde(default)]
pub struct Config {
    /// Path to Core IPC socket
    pub core_socket: PathBuf,

    /// Address to listen for peer connections
    pub listen_addr: SocketAddr,

    /// IP address for libp2p QUIC to bind to (e.g., "127.0.0.1" for local tests).
    /// Using a specific IP avoids Identify protocol advertising all local IPs,
    /// which can cause connection churn in test networks.
    pub libp2p_listen_ip: String,

    /// Peer port (used when generating listen address dynamically)
    pub peer_port: u16,

    /// Target number of outbound peer connections
    pub target_outbound_peers: usize,

    /// Maximum number of inbound peer connections
    pub max_inbound_peers: usize,

    /// Preferred peer addresses (always connect)
    pub preferred_peers: Vec<SocketAddr>,

    /// Known peer addresses (initial bootstrap)
    pub known_peers: Vec<SocketAddr>,

    /// Number of peers to push transactions to (push-k strategy)
    pub tx_push_peer_count: usize,

    /// Maximum mempool size (number of transactions)
    pub max_mempool_size: usize,

    /// HTTP server address for TX submission (None = disabled)
    pub http_addr: Option<SocketAddr>,

    /// Log level
    pub log_level: String,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            core_socket: PathBuf::from("/tmp/stellar-overlay.sock"),
            listen_addr: "0.0.0.0:11625".parse().unwrap(),
            libp2p_listen_ip: "0.0.0.0".to_string(), // Bind to all interfaces for internet operation
            peer_port: 11625,
            target_outbound_peers: 8,
            max_inbound_peers: 64,
            preferred_peers: Vec::new(),
            known_peers: Vec::new(),
            tx_push_peer_count: 8, // √64 ≈ 8
            max_mempool_size: 100_000,
            http_addr: Some("127.0.0.1:11626".parse().unwrap()),
            log_level: "info".to_string(),
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

    /// Validate configuration
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.target_outbound_peers == 0 {
            return Err(ConfigError::Invalid(
                "target_outbound_peers must be > 0".into(),
            ));
        }
        if self.tx_push_peer_count == 0 {
            return Err(ConfigError::Invalid(
                "tx_push_peer_count must be > 0".into(),
            ));
        }
        if self.max_mempool_size == 0 {
            return Err(ConfigError::Invalid("max_mempool_size must be > 0".into()));
        }
        Ok(())
    }
}

#[derive(Debug)]
pub enum ConfigError {
    Io(std::io::Error),
    Parse(String),
    Invalid(String),
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConfigError::Io(e) => write!(f, "config I/O error: {}", e),
            ConfigError::Parse(e) => write!(f, "config parse error: {}", e),
            ConfigError::Invalid(e) => write!(f, "config invalid: {}", e),
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
        assert!(config.validate().is_ok());
    }

    #[test]
    fn test_parse_config() {
        let toml = r#"
            core_socket = "/var/run/stellar.sock"
            listen_addr = "0.0.0.0:11625"
            target_outbound_peers = 10
            max_inbound_peers = 100
            tx_push_peer_count = 8
            max_mempool_size = 50000
            log_level = "debug"
            
            preferred_peers = ["1.2.3.4:11625", "5.6.7.8:11625"]
            known_peers = ["10.0.0.1:11625"]
        "#;

        let config = Config::from_str(toml).unwrap();
        assert_eq!(config.target_outbound_peers, 10);
        assert_eq!(config.preferred_peers.len(), 2);
    }

    // ═══ Validation Edge Cases ═══

    #[test]
    fn test_validate_zero_outbound_peers() {
        let mut config = Config::default();
        config.target_outbound_peers = 0;

        let result = config.validate();
        assert!(result.is_err());
        assert!(matches!(result.unwrap_err(), ConfigError::Invalid(_)));
    }

    #[test]
    fn test_validate_zero_tx_push_peer_count() {
        let mut config = Config::default();
        config.tx_push_peer_count = 0;

        let result = config.validate();
        assert!(result.is_err());
        assert!(matches!(result.unwrap_err(), ConfigError::Invalid(_)));
    }

    #[test]
    fn test_validate_zero_mempool_size() {
        let mut config = Config::default();
        config.max_mempool_size = 0;

        let result = config.validate();
        assert!(result.is_err());
        assert!(matches!(result.unwrap_err(), ConfigError::Invalid(_)));
    }

    #[test]
    fn test_validate_zero_inbound_peers_allowed() {
        // Zero inbound is allowed (node could be outbound-only)
        let mut config = Config::default();
        config.max_inbound_peers = 0;

        assert!(config.validate().is_ok());
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
    fn test_parse_invalid_address() {
        let toml = r#"listen_addr = "not-an-address""#;
        let result = Config::from_str(toml);
        assert!(result.is_err());
    }

    #[test]
    fn test_parse_wrong_type() {
        let toml = r#"target_outbound_peers = "eight""#;
        let result = Config::from_str(toml);
        assert!(result.is_err());
    }

    // ═══ Partial Config (uses defaults) ═══

    #[test]
    fn test_partial_config_uses_defaults() {
        let toml = r#"target_outbound_peers = 20"#;
        let config = Config::from_str(toml).unwrap();

        // Specified value
        assert_eq!(config.target_outbound_peers, 20);
        // Default values
        assert_eq!(config.max_inbound_peers, 64);
        assert_eq!(config.tx_push_peer_count, 8);
    }

    #[test]
    fn test_empty_config_uses_defaults() {
        let toml = "";
        let config = Config::from_str(toml).unwrap();

        assert_eq!(config.target_outbound_peers, 8);
        assert_eq!(config.max_mempool_size, 100_000);
        assert!(config.validate().is_ok());
    }

    // ═══ Edge Values ═══

    #[test]
    fn test_minimum_valid_values() {
        let mut config = Config::default();
        config.target_outbound_peers = 1;
        config.tx_push_peer_count = 1;
        config.max_mempool_size = 1;

        assert!(config.validate().is_ok());
    }

    #[test]
    fn test_large_values() {
        let mut config = Config::default();
        config.target_outbound_peers = 1000;
        config.max_inbound_peers = 10000;
        config.max_mempool_size = 10_000_000;

        assert!(config.validate().is_ok());
    }
}
