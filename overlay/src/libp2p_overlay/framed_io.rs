//! Length-prefixed writes without an intermediate bulk-payload allocation.
use futures::{AsyncWrite, AsyncWriteExt};
use std::io;

/// Write a single length-prefixed frame without assembling a bulk copy.
/// Callers sharing a stream must hold its lock over this entire future.
pub(super) async fn write_frame_parts<W: AsyncWrite + Unpin>(
    writer: &mut W,
    parts: &[&[u8]],
) -> io::Result<()> {
    let length = parts
        .iter()
        .try_fold(0u32, |total, part| {
            u32::try_from(part.len())
                .ok()
                .and_then(|n| total.checked_add(n))
        })
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "frame length exceeds u32"))?;
    writer.write_all(&length.to_be_bytes()).await?;
    for part in parts {
        writer.write_all(part).await?;
    }
    writer.flush().await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::pin::Pin;
    use std::task::{Context, Poll};

    #[derive(Default)]
    struct ShortWriter {
        bytes: Vec<u8>,
        yield_next: bool,
    }

    impl AsyncWrite for ShortWriter {
        fn poll_write(
            mut self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            bytes: &[u8],
        ) -> Poll<io::Result<usize>> {
            if self.yield_next {
                self.yield_next = false;
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            self.yield_next = true;
            let n = bytes.len().min(3);
            self.bytes.extend_from_slice(&bytes[..n]);
            Poll::Ready(Ok(n))
        }

        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }

        fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Ready(Ok(()))
        }
    }

    #[tokio::test]
    async fn segmented_txset_frame_matches_xdr_with_partial_writes() {
        let (_, data) = super::super::test_txset_xdr(7);
        let expected = crate::xdr::frame_tx_set(&data);
        let discriminant = (stellar_xdr::curr::MessageType::GeneralizedTxSet as i32).to_be_bytes();
        let mut writer = ShortWriter::default();
        write_frame_parts(&mut writer, &[&discriminant, &[], &data])
            .await
            .unwrap();
        write_frame_parts(&mut writer, &[b"next frame"])
            .await
            .unwrap();

        let mut expected_frames = (expected.len() as u32).to_be_bytes().to_vec();
        expected_frames.extend_from_slice(&expected);
        expected_frames.extend_from_slice(&10u32.to_be_bytes());
        expected_frames.extend_from_slice(b"next frame");
        assert_eq!(writer.bytes, expected_frames);
    }
}
