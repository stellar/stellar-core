//! IPC module for Core ↔ Overlay communication.

mod messages;
mod transport;

pub use messages::{Message, MessageType};
pub use transport::CoreIpc;
