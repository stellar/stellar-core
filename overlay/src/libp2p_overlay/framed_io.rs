//! Length-prefixed writes without an intermediate bulk-payload allocation.
use futures::{AsyncWrite, AsyncWriteExt};
use std::io;

/// Either one segmented frame or several complete length-prefixed frames.
/// A batch preserves the wire boundaries of its individual messages.
#[derive(Clone, Copy)]
pub(super) enum Frames<'a> {
    One(&'a [&'a [u8]]),
    Batch(&'a [u8]),
}

impl Frames<'_> {
    pub(super) async fn write<W: AsyncWrite + Unpin>(self, writer: &mut W) -> io::Result<()> {
        match self {
            Self::One(parts) => write_frame_parts(writer, parts).await,
            Self::Batch(bytes) => {
                writer.write_all(bytes).await?;
                writer.flush().await
            }
        }
    }
}

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
    use futures::AsyncRead;
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
        let discriminant = (stellar_xdr::MessageType::GeneralizedTxSet as i32).to_be_bytes();
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

    /// Delivers at most `max` bytes per read and yields between reads, like
    /// a QUIC stream handing over a batch as packets arrive.
    struct ShortReader {
        bytes: Vec<u8>,
        pos: usize,
        max: usize,
        yield_next: bool,
    }

    impl AsyncRead for ShortReader {
        fn poll_read(
            mut self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<io::Result<usize>> {
            if self.yield_next {
                self.yield_next = false;
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            self.yield_next = true;
            let pos = self.pos;
            let n = buf.len().min(self.max).min(self.bytes.len() - pos);
            buf[..n].copy_from_slice(&self.bytes[pos..pos + n]);
            self.pos += n;
            Poll::Ready(Ok(n))
        }
    }

    #[tokio::test]
    async fn batched_frames_read_back_through_a_buffered_reader() {
        // Frames from 1 byte to more than twice the read buffer: the large
        // ones exercise the reader's bypass once its buffer is drained.
        let frames: Vec<Vec<u8>> = (0..40u8).map(|i| vec![i; 1 + 61 * i as usize]).collect();
        let mut batch = Vec::new();
        for frame in &frames[..30] {
            batch.extend_from_slice(&(frame.len() as u32).to_be_bytes());
            batch.extend_from_slice(frame);
        }
        let mut writer = ShortWriter::default();
        Frames::Batch(&batch).write(&mut writer).await.unwrap();
        for frame in &frames[30..] {
            Frames::One(&[frame]).write(&mut writer).await.unwrap();
        }

        let reader = ShortReader {
            bytes: writer.bytes,
            pos: 0,
            max: 700,
            yield_next: false,
        };
        let mut reader = futures::io::BufReader::with_capacity(1024, reader);
        for frame in &frames {
            assert_eq!(
                &super::super::read_framed(&mut reader).await.unwrap(),
                frame
            );
        }
        let end = super::super::read_framed(&mut reader).await.unwrap_err();
        assert_eq!(end.kind(), io::ErrorKind::UnexpectedEof);
    }
}
