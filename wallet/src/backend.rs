use crate::model::CHAIN;
use anyhow::{Context, Result, bail, ensure};
use serde_json::Value;
use std::{
    io::Read,
    path::PathBuf,
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

pub trait Core: Send {
    fn call(&self, args: &[&str]) -> Result<Value>;
}

pub struct Backend {
    binary: PathBuf,
    embedded: bool,
}
impl Backend {
    pub fn new(external: Option<PathBuf>) -> Result<Self> {
        let (binary, embedded) = match external {
            Some(p) => (p, false),
            None if cfg!(feature = "embedded-core") => (std::env::current_exe()?, true),
            None => (
                PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../constella-wallet-core"),
                false,
            ),
        };
        Self::checked(binary, embedded)
    }
    pub fn bundled(binary: PathBuf) -> Result<Self> {
        Self::checked(binary, true)
    }
    fn checked(binary: PathBuf, embedded: bool) -> Result<Self> {
        let core = Self { binary, embedded };
        let p = core.call(&["profile"])?;
        ensure!(
            p["api"] == 1 && p["network"] == "testnet-v5" && p["chain_id"] == CHAIN,
            "This wallet requires its matching testnet-v5 backend."
        );
        Ok(core)
    }
}
impl Core for Backend {
    fn call(&self, args: &[&str]) -> Result<Value> {
        let mut cmd = Command::new(&self.binary);
        if self.embedded {
            cmd.arg("--wallet-core");
        }
        cmd.args(args);
        let bytes = command_output(cmd, Duration::from_secs(15))?;
        let data: Value = serde_json::from_slice(&bytes).context("Invalid wallet response")?;
        if data["ok"] != true {
            bail!(
                "{}",
                data["error"].as_str().unwrap_or("Wallet operation failed")
            );
        }
        Ok(data)
    }
}

pub fn command_output(mut cmd: Command, timeout: Duration) -> Result<Vec<u8>> {
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .context("Cannot start wallet operation; check the executable and local Docker access")?;
    let stdout = child.stdout.take().unwrap();
    // Drain concurrently so a faulty backend cannot deadlock on a full pipe.
    // Bound retained output; kill/reap on timeout or excessive output.
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.take(65537).read_to_end(&mut bytes).map(|_| bytes)
    });
    let deadline = Instant::now() + timeout;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Ok(None) if Instant::now() < deadline => thread::sleep(Duration::from_millis(20)),
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                break Err(anyhow::anyhow!(
                    "Operation timed out. Check the saved payment before retrying."
                ));
            }
            Err(e) => {
                let _ = child.kill();
                let _ = child.wait();
                break Err(e.into());
            }
        }
    };
    let bytes = reader
        .join()
        .map_err(|_| anyhow::anyhow!("Wallet response reader failed"))??;
    let status = status?;
    ensure!(
        status.success(),
        "{}",
        serde_json::from_slice::<Value>(&bytes)
            .ok()
            .and_then(|v| v["error"].as_str().map(str::to_owned))
            .unwrap_or_else(|| "Wallet operation failed; check local node/Docker access".into())
    );
    ensure!(bytes.len() <= 65536, "Wallet response is too large");
    Ok(bytes)
}
