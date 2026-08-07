//! IPC module for Core ↔ Overlay communication.

mod messages;
mod transport;

pub use messages::{Message, MessageCodec, MessageType};
pub use transport::CoreIpc;
