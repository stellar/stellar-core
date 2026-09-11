//! Mandatory OS receive buffering for every QUIC socket.

use socket2::Socket;
use std::io;

pub(crate) const RECEIVE_BUFFER_BYTES: usize = 4 * 1024 * 1024;

// Linux reports twice the requested SO_RCVBUF size for bookkeeping overhead.
const REQUIRED_REPORTED_BYTES: usize = if cfg!(target_os = "linux") {
    2 * RECEIVE_BUFFER_BYTES
} else {
    RECEIVE_BUFFER_BYTES
};

pub(crate) fn configure_receive_buffer(socket: &Socket) -> io::Result<usize> {
    require_receive_buffer(socket)
}

// Keep the OS operations separate so tests can exercise clamping and permission
// failures deterministically without changing shared kernel settings.
trait ReceiveBuffer {
    fn request(&self) -> io::Result<()>;
    fn reported_size(&self) -> io::Result<usize>;
    fn force(&self) -> io::Result<()>;
}

fn require_receive_buffer(socket: &impl ReceiveBuffer) -> io::Result<usize> {
    socket.request().map_err(|error| {
        io::Error::new(
            error.kind(),
            format!("QUIC requires a {RECEIVE_BUFFER_BYTES}-byte UDP receive buffer: {error}"),
        )
    })?;
    let mut effective = socket.reported_size()?;
    if effective < REQUIRED_REPORTED_BYTES {
        socket.force().map_err(|error| {
            io::Error::new(
                error.kind(),
                format!(
                    "QUIC requires a {RECEIVE_BUFFER_BYTES}-byte UDP receive-buffer request \
                     ({REQUIRED_REPORTED_BYTES} reported bytes), but the OS provided {effective}. \
                     Increase the OS receive-buffer allowance (Linux: net.core.rmem_max), \
                     or grant CAP_NET_ADMIN for the Linux per-socket increase: {error}"
                ),
            )
        })?;
        effective = socket.reported_size()?;
    }
    if effective < REQUIRED_REPORTED_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!(
                "QUIC requires {REQUIRED_REPORTED_BYTES} reported UDP receive-buffer bytes; \
                 OS still reports {effective} after the per-socket increase"
            ),
        ));
    }
    Ok(effective)
}

impl ReceiveBuffer for Socket {
    fn request(&self) -> io::Result<()> {
        self.set_recv_buffer_size(RECEIVE_BUFFER_BYTES)
    }

    fn reported_size(&self) -> io::Result<usize> {
        self.recv_buffer_size()
    }

    fn force(&self) -> io::Result<()> {
        #[cfg(target_os = "linux")]
        {
            use std::os::fd::AsRawFd;
            let bytes = RECEIVE_BUFFER_BYTES as libc::c_int;
            // SAFETY: self owns a live fd; bytes is an initialized c_int and
            // the pointer and size describe it for the duration of the syscall.
            let result = unsafe {
                libc::setsockopt(
                    self.as_raw_fd(),
                    libc::SOL_SOCKET,
                    libc::SO_RCVBUFFORCE,
                    (&bytes as *const libc::c_int).cast(),
                    std::mem::size_of_val(&bytes) as libc::socklen_t,
                )
            };
            if result != 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }
        #[cfg(not(target_os = "linux"))]
        {
            Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "this platform has no privileged per-socket receive-buffer increase",
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    struct TestSocket {
        ordinary: usize,
        forced: io::Result<usize>,
        effective: Cell<usize>,
        operations: RefCell<Vec<&'static str>>,
    }

    impl TestSocket {
        fn new(ordinary: usize, forced: io::Result<usize>) -> Self {
            Self {
                ordinary,
                forced,
                effective: Cell::new(0),
                operations: RefCell::new(Vec::new()),
            }
        }
    }

    impl ReceiveBuffer for TestSocket {
        fn request(&self) -> io::Result<()> {
            self.operations.borrow_mut().push("request");
            self.effective.set(self.ordinary);
            Ok(())
        }

        fn reported_size(&self) -> io::Result<usize> {
            self.operations.borrow_mut().push("read");
            Ok(self.effective.get())
        }

        fn force(&self) -> io::Result<()> {
            self.operations.borrow_mut().push("force");
            match &self.forced {
                Ok(size) => {
                    self.effective.set(*size);
                    Ok(())
                }
                Err(error) => Err(io::Error::new(error.kind(), error.to_string())),
            }
        }
    }

    #[test]
    fn adequate_ordinary_buffer_does_not_require_privilege() {
        for size in [REQUIRED_REPORTED_BYTES, REQUIRED_REPORTED_BYTES * 2] {
            let socket = TestSocket::new(size, Err(io::ErrorKind::PermissionDenied.into()));
            assert_eq!(require_receive_buffer(&socket).unwrap(), size);
            assert_eq!(*socket.operations.borrow(), ["request", "read"]);
        }
    }

    #[test]
    fn clamped_buffer_is_increased_and_verified() {
        let socket = TestSocket::new(212_992, Ok(REQUIRED_REPORTED_BYTES));
        assert_eq!(
            require_receive_buffer(&socket).unwrap(),
            REQUIRED_REPORTED_BYTES
        );
        assert_eq!(
            *socket.operations.borrow(),
            ["request", "read", "force", "read"]
        );
    }

    #[test]
    fn denied_or_unsupported_increase_never_accepts_an_undersized_socket() {
        for kind in [io::ErrorKind::PermissionDenied, io::ErrorKind::Unsupported] {
            let socket = TestSocket::new(212_992, Err(kind.into()));
            let error = require_receive_buffer(&socket).unwrap_err();
            assert_eq!(error.kind(), kind);
            assert!(error.to_string().contains("4194304"));
            assert!(error.to_string().contains("212992"));
            assert_eq!(*socket.operations.borrow(), ["request", "read", "force"]);
        }
    }

    #[test]
    fn successful_syscall_with_insufficient_buffer_is_an_error() {
        let socket = TestSocket::new(212_992, Ok(REQUIRED_REPORTED_BYTES - 1));
        let error = require_receive_buffer(&socket).unwrap_err();
        assert!(error.to_string().contains("OS still reports"));
        assert_eq!(
            *socket.operations.borrow(),
            ["request", "read", "force", "read"]
        );
    }
}
