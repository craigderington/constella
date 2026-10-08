use crate::model::{CHAIN, address, number};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::{
        fd::AsRawFd,
        unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
    },
    path::{Path, PathBuf},
};

pub fn private_dir(path: &Path) -> Result<()> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)?;
    let m = fs::symlink_metadata(path)?;
    ensure!(
        m.is_dir() && m.uid() == unsafe { libc::getuid() } && m.mode() & 0o077 == 0,
        "{} must be a private directory owned by you (0700)",
        path.display()
    );
    Ok(())
}
fn check(file: &File) -> Result<()> {
    let m = file.metadata()?;
    ensure!(
        m.is_file() && m.uid() == unsafe { libc::getuid() } && m.mode() & 0o077 == 0,
        "File must be regular, owned by you, and private (0600)"
    );
    Ok(())
}
pub fn read_private(path: &Path) -> Result<Vec<u8>> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)?;
    check(&file)?;
    let mut bytes = Vec::new();
    file.take(2_000_001).read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= 2_000_000, "Wallet state is too large");
    Ok(bytes)
}
pub struct Store {
    pub data: Value,
    pub directory: PathBuf,
    _lock: Option<File>,
}
impl Store {
    pub fn open(directory: PathBuf) -> Result<Self> {
        private_dir(&directory)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(directory.join("lock"))?;
        check(&lock)?;
        ensure!(
            unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0,
            "Another wallet window is using this state directory"
        );
        let data = match read_private(&directory.join("state.json")) {
            Ok(bytes) => {
                serde_json::from_slice(&bytes).context("Invalid state; existing file preserved")?
            }
            Err(e)
                if e.downcast_ref::<std::io::Error>()
                    .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound) =>
            {
                Self::empty()
            }
            Err(e) => return Err(e),
        };
        Self::validate(&data)?;
        Ok(Self {
            data,
            directory,
            _lock: Some(lock),
        })
    }
    fn empty() -> Value {
        json!({"version":1,"chain_id":CHAIN,"receipts":[],"contacts":{}})
    }
    pub fn memory() -> Self {
        Self {
            data: Self::empty(),
            directory: PathBuf::new(),
            _lock: None,
        }
    }
    fn validate(v: &Value) -> Result<()> {
        ensure!(
            v["version"] == 1 && v["chain_id"] == CHAIN,
            "Wallet state belongs to a different network or version"
        );
        ensure!(
            v["contacts"].is_object() && v["receipts"].is_array(),
            "Invalid wallet state"
        );
        for a in v["contacts"].as_object().unwrap().values() {
            address(a.as_str().unwrap_or(""))?;
        }
        for r in v["receipts"].as_array().unwrap() {
            for k in ["id", "from", "to"] {
                address(r[k].as_str().unwrap_or(""))?;
            }
            for k in ["amount", "fee", "nonce"] {
                number(&r[k])?;
            }
            ensure!(
                valid_raw(r["raw"].as_str().unwrap_or("")) && r["status"].is_string(),
                "Invalid saved transaction; state preserved"
            );
        }
        Ok(())
    }
    pub fn save(&self) -> Result<()> {
        ensure!(self._lock.is_some(), "Demo cannot save state");
        let bytes = serde_json::to_vec_pretty(&self.data)?;
        ensure!(
            bytes.len() <= 2_000_000,
            "Wallet state exceeds its size limit; nothing submitted"
        );
        let mut tmp = tempfile::Builder::new()
            .prefix(".wallet-")
            .tempfile_in(&self.directory)?;
        tmp.write_all(&bytes)?;
        tmp.write_all(b"\n")?;
        tmp.as_file().sync_all()?;
        tmp.persist(self.directory.join("state.json"))?;
        File::open(&self.directory)?.sync_all()?;
        Ok(())
    }
}
pub fn valid_raw(s: &str) -> bool {
    s.len() == 304
        && s.bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
