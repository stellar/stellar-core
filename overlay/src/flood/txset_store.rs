//! Durable store of TX sets.
//!
//! A validator must hold the TX set of a value on stable storage before it
//! votes for that value, so that it still has the set after a crash. The
//! overlay already holds every set in the form peers exchange (a length
//! prefix and one zstd frame, see `txset_encoding`), so that is what is
//! stored: one file per set, named by the set's content hash, written and
//! synced before the set is handed to Core (a set received from a peer) or
//! before Core's next message is processed (a set Core built).
//!
//! Only sets that Core holds are stored (built by it, or delivered to it),
//! and a stored set is deleted once its slot is a number of ledgers behind,
//! independently of the in-memory cache. Sets found on disk at startup are
//! served on demand (Core asks for the sets its restored SCP state names) and
//! deleted once the node has moved past them.

use super::txset_encoding;
use super::{Hash256, TxSetData};
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

const FILE_EXTENSION: &str = "txset";
const TMP_EXTENSION: &str = "tmp";

pub struct TxSetStore {
    dir: PathBuf,
    /// Hashes that have a complete file in `dir`.
    stored: Mutex<HashSet<Hash256>>,
}

fn file_name(hash: &Hash256) -> String {
    let mut name = String::with_capacity(64 + 1 + FILE_EXTENSION.len());
    for b in hash {
        name.push_str(&format!("{:02x}", b));
    }
    name.push('.');
    name.push_str(FILE_EXTENSION);
    name
}

fn parse_file_name(path: &Path) -> Option<Hash256> {
    if path.extension()?.to_str()? != FILE_EXTENSION {
        return None;
    }
    let stem = path.file_stem()?.to_str()?;
    if stem.len() != 64 {
        return None;
    }
    let mut hash = [0u8; 32];
    for (i, byte) in hash.iter_mut().enumerate() {
        *byte = u8::from_str_radix(stem.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(hash)
}

impl TxSetStore {
    /// Opens (creating it if needed) the store in `dir`. Sets left by an
    /// earlier run are kept; leftovers of interrupted writes are removed.
    pub fn open(dir: PathBuf) -> io::Result<Self> {
        fs::create_dir_all(&dir)?;
        let mut stored = HashSet::new();
        for entry in fs::read_dir(&dir)? {
            let path = entry?.path();
            if let Some(hash) = parse_file_name(&path) {
                stored.insert(hash);
            } else if path.extension().and_then(|e| e.to_str()) == Some(TMP_EXTENSION) {
                let _ = fs::remove_file(&path);
            }
        }
        Ok(Self {
            dir,
            stored: Mutex::new(stored),
        })
    }

    fn path(&self, hash: &Hash256) -> PathBuf {
        self.dir.join(file_name(hash))
    }

    /// Whether the set is on stable storage.
    pub fn contains(&self, hash: &Hash256) -> bool {
        self.stored.lock().unwrap().contains(hash)
    }

    /// Hashes of all stored sets.
    pub fn hashes(&self) -> Vec<Hash256> {
        self.stored.lock().unwrap().iter().copied().collect()
    }

    /// Writes the encoded form of a set and syncs it to disk. Blocking: call
    /// from a blocking worker. A set that is already stored is left alone.
    pub fn persist(&self, hash: &Hash256, data: &TxSetData) -> io::Result<()> {
        if self.contains(hash) {
            return Ok(());
        }
        let path = self.path(hash);
        let tmp = path.with_extension(TMP_EXTENSION);
        {
            let mut file = File::create(&tmp)?;
            file.write_all(data.encoded())?;
            file.sync_data()?;
        }
        fs::rename(&tmp, &path)?;
        // Make the new name durable as well.
        File::open(&self.dir)?.sync_all()?;
        self.stored.lock().unwrap().insert(*hash);
        Ok(())
    }

    /// Reads a stored set back. Returns `None` if the set is not stored, or
    /// if its file does not decode to a set with that hash (the file is
    /// removed then). Blocking.
    pub fn load(&self, hash: &Hash256) -> io::Result<Option<TxSetData>> {
        if !self.contains(hash) {
            return Ok(None);
        }
        let frame = match fs::read(self.path(hash)) {
            Ok(frame) => frame,
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                self.stored.lock().unwrap().remove(hash);
                return Ok(None);
            }
            Err(e) => return Err(e),
        };
        match txset_encoding::decode(frame) {
            Ok(data) if crate::xdr::tx_set_hash_matches(hash, data.as_slice()) => Ok(Some(data)),
            _ => {
                self.remove(hash);
                Ok(None)
            }
        }
    }

    /// Deletes a stored set, if present.
    pub fn remove(&self, hash: &Hash256) {
        if self.stored.lock().unwrap().remove(hash) {
            let _ = fs::remove_file(self.path(hash));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_set(seed: u8) -> (Hash256, TxSetData) {
        let xdr = vec![seed; 1000 + seed as usize];
        let hash = crate::xdr::sha256_hash(&xdr);
        (hash, TxSetData::from_local(xdr).unwrap())
    }

    #[test]
    fn persists_the_encoded_frame_and_loads_it_back() {
        let dir = tempfile::tempdir().unwrap();
        let store = TxSetStore::open(dir.path().join("txsets")).unwrap();
        let (hash, data) = test_set(7);

        assert!(!store.contains(&hash));
        assert!(store.load(&hash).unwrap().is_none());
        store.persist(&hash, &data).unwrap();
        assert!(store.contains(&hash));

        // The file is exactly the frame peers exchange, not the raw XDR.
        let on_disk = fs::read(dir.path().join("txsets").join(file_name(&hash))).unwrap();
        assert_eq!(on_disk, data.encoded());
        assert!(on_disk.len() < data.len());

        let loaded = store.load(&hash).unwrap().unwrap();
        assert_eq!(loaded.as_slice(), data.as_slice());
        assert_eq!(loaded.encoded(), data.encoded());

        // persisting again is a no-op
        store.persist(&hash, &data).unwrap();
        assert_eq!(store.hashes(), vec![hash]);
    }

    #[test]
    fn reopening_finds_stored_sets_and_drops_interrupted_writes() {
        let dir = tempfile::tempdir().unwrap();
        let (hash_a, data_a) = test_set(1);
        let (hash_b, data_b) = test_set(2);
        {
            let store = TxSetStore::open(dir.path().to_path_buf()).unwrap();
            store.persist(&hash_a, &data_a).unwrap();
            store.persist(&hash_b, &data_b).unwrap();
        }
        // An interrupted write and an unrelated file.
        let tmp = dir
            .path()
            .join(file_name(&[9; 32]))
            .with_extension(TMP_EXTENSION);
        fs::write(&tmp, b"partial").unwrap();
        fs::write(dir.path().join("README"), b"not a set").unwrap();

        let store = TxSetStore::open(dir.path().to_path_buf()).unwrap();
        let mut hashes = store.hashes();
        hashes.sort();
        let mut expected = vec![hash_a, hash_b];
        expected.sort();
        assert_eq!(hashes, expected);
        assert!(!tmp.exists());
        assert!(dir.path().join("README").exists());
        assert_eq!(
            store.load(&hash_b).unwrap().unwrap().as_slice(),
            data_b.as_slice()
        );
    }

    #[test]
    fn remove_deletes_the_file() {
        let dir = tempfile::tempdir().unwrap();
        let store = TxSetStore::open(dir.path().to_path_buf()).unwrap();
        let (hash, data) = test_set(3);
        store.persist(&hash, &data).unwrap();
        let path = dir.path().join(file_name(&hash));
        assert!(path.exists());
        store.remove(&hash);
        assert!(!path.exists());
        assert!(!store.contains(&hash));
        assert!(store.load(&hash).unwrap().is_none());
        // removing an unknown set is fine
        store.remove(&hash);
    }

    #[test]
    fn a_corrupt_or_mismatching_file_is_not_served() {
        let dir = tempfile::tempdir().unwrap();
        let (hash, data) = test_set(4);
        let (other_hash, other_data) = test_set(5);

        // garbage under a valid name
        fs::write(dir.path().join(file_name(&hash)), b"garbage").unwrap();
        // a valid frame under the wrong name
        fs::write(dir.path().join(file_name(&other_hash)), data.encoded()).unwrap();

        let store = TxSetStore::open(dir.path().to_path_buf()).unwrap();
        assert!(store.contains(&hash));
        assert!(store.load(&hash).unwrap().is_none());
        assert!(!store.contains(&hash));
        assert!(!dir.path().join(file_name(&hash)).exists());

        assert!(store.load(&other_hash).unwrap().is_none());
        assert!(!dir.path().join(file_name(&other_hash)).exists());

        // and the store still works for the real thing
        store.persist(&other_hash, &other_data).unwrap();
        assert_eq!(
            store.load(&other_hash).unwrap().unwrap().as_slice(),
            other_data.as_slice()
        );
    }
}
