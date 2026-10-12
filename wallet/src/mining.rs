//! Local Docker adapter. Signing keys stay inside a read-only mount of the
//! existing miner volume. The UI persists signed bytes before broadcasting.
use crate::{
    backend::{Backend, Core, command_output},
    model::{address, text},
    store::read_private,
};
use anyhow::{Context, Result, bail, ensure};
use serde_json::Value;
use std::{path::PathBuf, process::Command, time::Duration};

const KEY: &str = "/data/wallet.key";
const DOCKER_HOST: &str = "unix:///var/run/docker.sock";

#[derive(Clone, Debug, PartialEq)]
pub struct Target {
    pub id: String,
    image: String,
    volume: String,
    port: u16,
    pub peer: String,
}
impl Target {
    pub fn parse(data: &Value) -> Result<Self> {
        ensure!(
            data["State"]["Running"] == true && data["State"]["Paused"] == false,
            "The mining node must be running"
        );
        let env = data["Config"]["Env"]
            .as_array()
            .context("Missing miner configuration")?;
        let setting = |key: &str| {
            env.iter()
                .filter_map(Value::as_str)
                .find_map(|s| s.strip_prefix(&format!("{key}=")))
        };
        ensure!(
            setting("CONSTELLA_DATA") == Some("/data")
                && setting("CONSTELLA_KEY").is_none_or(|p| p == KEY),
            "Unsupported mining key location"
        );
        ensure!(
            setting("CONSTELLA_ADDR").is_none_or(str::is_empty),
            "Miner has a payout-address override; configure the wallet for that address instead"
        );
        let port = setting("CONSTELLA_PORT")
            .context("Missing miner port")?
            .parse::<u16>()?;
        ensure!(port > 0, "Invalid miner port");
        let mounts = data["Mounts"].as_array().context("Missing miner storage")?;
        let mount = mounts
            .iter()
            .find(|m| m["Destination"] == "/data")
            .context("Miner has no data volume")?;
        ensure!(
            mount["Type"] == "volume",
            "Mining transfers require a named data volume"
        );
        let volume = text(mount, "Name").to_owned();
        ensure!(
            !volume.is_empty()
                && volume
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c)),
            "Invalid data volume name"
        );
        let bindings = data["NetworkSettings"]["Ports"][format!("{port}/tcp")]
            .as_array()
            .context("Miner needs a published P2P port")?;
        let binding = bindings
            .first()
            .context("Miner P2P port is not published")?;
        let host = text(binding, "HostIp");
        let mapped = text(binding, "HostPort").parse::<u16>()?;
        ensure!(mapped > 0, "Invalid published miner port");
        let host = if host == "0.0.0.0" || host.is_empty() {
            "127.0.0.1"
        } else if host == "::" {
            "::1"
        } else {
            host
        };
        let ip = host.parse::<std::net::IpAddr>()?;
        let peer = std::net::SocketAddr::new(ip, mapped).to_string();
        let id = address(text(data, "Id"))?;
        let image = text(data, "Image").to_owned();
        address(
            image
                .strip_prefix("sha256:")
                .context("Miner image must be pinned by digest")?,
        )?;
        Ok(Self {
            id,
            image,
            volume,
            port,
            peer,
        })
    }
    pub fn signer_args(
        &self,
        binary: &std::path::Path,
        name: &str,
        operation: &[&str],
    ) -> Result<Vec<String>> {
        let path = binary
            .to_str()
            .context("Unsupported wallet executable path")?;
        ensure!(
            binary.is_absolute() && !path.contains(','),
            "Wallet executable needs an absolute path without commas"
        );
        let mut args: Vec<String> = [
            "run",
            "--rm",
            "--pull=never",
            "--name",
            name,
            "--read-only",
            "--cap-drop=ALL",
            "--security-opt=no-new-privileges:true",
            "--user=0:0",
            "--memory=64m",
            "--memory-swap=64m",
            "--cpus=0.25",
            "--pids-limit=16",
            "--no-healthcheck",
            "--log-driver=none",
            "--label=constella.wallet.signer=true",
            "--entrypoint=/wallet-signer",
        ]
        .into_iter()
        .map(str::to_owned)
        .collect();
        args.extend([
            "--network".into(),
            format!("container:{}", self.id),
            "--mount".into(),
            format!(
                "type=volume,src={},dst=/data,readonly,volume-nocopy",
                self.volume
            ),
            "--mount".into(),
            format!("type=bind,src={path},dst=/wallet-signer,readonly"),
            self.image.clone(),
            "--wallet-core".into(),
        ]);
        args.extend(operation.iter().map(|s| s.to_string()));
        Ok(args)
    }
}
pub struct MiningBackend {
    local: Backend,
    target: Target,
    expected: String,
    binary: PathBuf,
}
impl MiningBackend {
    fn docker() -> Command {
        let mut c = Command::new("docker");
        c.args(["--host", DOCKER_HOST]);
        c.env_remove("DOCKER_CONTEXT")
            .env_remove("DOCKER_HOST")
            .env_remove("DOCKER_TLS_VERIFY")
            .env_remove("DOCKER_CERT_PATH");
        c
    }
    fn inspect(container: &str) -> Result<Target> {
        ensure!(
            !container.is_empty()
                && !container.starts_with('-')
                && container
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b)),
            "Invalid local miner container name"
        );
        let mut cmd = Self::docker();
        // Read metadata only; private key files are never returned by this adapter.
        cmd.args(["inspect", "--format", r#"{"Id":{{json .Id}},"Image":{{json .Image}},"State":{{json .State}},"Config":{"Env":{{json .Config.Env}}},"Mounts":{{json .Mounts}},"NetworkSettings":{"Ports":{{json .NetworkSettings.Ports}}}}"#, container]);
        let data: Value = serde_json::from_slice(&command_output(cmd, Duration::from_secs(10))?)?;
        Target::parse(&data)
    }
    pub fn new(container: &str, expected: &str) -> Result<Self> {
        Self::with_binary(container, expected, std::env::current_exe()?)
    }
    fn with_binary(container: &str, expected: &str, binary: PathBuf) -> Result<Self> {
        ensure!(
            cfg!(feature = "embedded-core"),
            "Mining transfers require the bundled wallet build"
        );
        let expected = address(expected)?;
        let target = Self::inspect(container)?;
        Ok(Self {
            local: Backend::bundled(binary.clone())?,
            target,
            expected,
            binary,
        })
    }
    pub fn peer(&self) -> &str {
        &self.target.peer
    }
    fn signer(&self, operation: &[&str]) -> Result<Value> {
        ensure!(
            Self::inspect(&self.target.id)? == self.target,
            "Mining node changed; reopen the wallet and review again"
        );
        // A random name lets cleanup target only this helper if Docker times out.
        let token = tempfile::Builder::new()
            .prefix("constella-wallet-sign-")
            .tempdir()?;
        let name = token.path().file_name().unwrap().to_str().unwrap();
        let mut cmd = Self::docker();
        cmd.args(self.target.signer_args(&self.binary, name, operation)?);
        let output = command_output(cmd, Duration::from_secs(25));
        if output.is_err() {
            let mut cleanup = Self::docker();
            cleanup.args(["rm", "--force", name]);
            let _ = command_output(cleanup, Duration::from_secs(5));
        }
        let data: Value = serde_json::from_slice(&output?)?;
        ensure!(data["ok"] == true, "{}", text(&data, "error"));
        Ok(data)
    }
}
impl Core for MiningBackend {
    fn call(&self, args: &[&str]) -> Result<Value> {
        match args {
            ["profile"] => self.local.call(args),
            ["address", KEY] => {
                let a = self.signer(&["address", KEY])?;
                ensure!(
                    a["address"] == self.expected,
                    "Mining key does not match the configured mining address"
                );
                Ok(a)
            }
            ["prepare", KEY, peer, to, amount, fee, nonce] => {
                ensure!(
                    *peer == self.target.peer,
                    "Mining node settings changed; review again"
                );
                self.call(&["address", KEY])?;
                let tx = self.signer(&[
                    "prepare",
                    KEY,
                    &format!("127.0.0.1:{}", self.target.port),
                    to,
                    amount,
                    fee,
                    nonce,
                ])?;
                ensure!(
                    tx["from"] == self.expected,
                    "Mining signer returned a different address; nothing broadcast"
                );
                Ok(tx)
            }
            ["balance", peer, addr] if *peer == self.target.peer && *addr == self.expected => {
                self.local.call(args)
            }
            ["broadcast", peer, _] if *peer == self.target.peer => self.local.call(args),
            _ => bail!(
                "This mining wallet supports reward transfers only; key creation and export are disabled"
            ),
        }
    }
}

pub fn personal_state(path: &std::path::Path) -> Result<Value> {
    match read_private(path) {
        Ok(bytes) => {
            let data: Value = serde_json::from_slice(&bytes)?;
            crate::store::Store::validate(&data)?;
            Ok(data)
        }
        Err(e)
            if e.downcast_ref::<std::io::Error>()
                .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound) =>
        {
            Ok(serde_json::json!({}))
        }
        Err(e) => Err(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn sample() -> Value {
        json!({"Id":"aa".repeat(32),"Image":format!("sha256:{}","bb".repeat(32)),"State":{"Running":true,"Paused":false},"Config":{"Env":["CONSTELLA_DATA=/data","CONSTELLA_PORT=18473"]},"Mounts":[{"Destination":"/data","Type":"volume","Name":"test-mining-data"}],"NetworkSettings":{"Ports":{"18473/tcp":[{"HostIp":"127.0.0.1","HostPort":"19473"}]}}})
    }
    #[test]
    fn validates_miner_and_mounts_read_only_without_extra_access() {
        let target = Target::parse(&sample()).unwrap();
        assert_eq!(target.peer, "127.0.0.1:19473");
        let args = target
            .signer_args(
                std::path::Path::new("/app/wallet"),
                "test-helper",
                &["address", KEY],
            )
            .unwrap();
        for flag in [
            "--read-only",
            "--cap-drop=ALL",
            "--pull=never",
            "--user=0:0",
            "--log-driver=none",
            "--entrypoint=/wallet-signer",
        ] {
            assert!(args.iter().any(|a| a == flag));
        }
        assert!(
            args.iter()
                .any(|a| a == "type=volume,src=test-mining-data,dst=/data,readonly,volume-nocopy")
        );
        assert!(
            args.iter()
                .any(|a| a == "type=bind,src=/app/wallet,dst=/wallet-signer,readonly")
        );
        assert!(
            !args
                .iter()
                .any(|a| a.contains("docker.sock") || a == "--privileged" || a == "--volumes-from")
        );
        assert!(
            target
                .signer_args(std::path::Path::new("/app,other/wallet"), "x", &[])
                .is_err()
        );
        for mutation in 0..5 {
            let mut data = sample();
            match mutation {
                0 => data["State"]["Running"] = json!(false),
                1 => data["Mounts"][0]["Type"] = json!("bind"),
                2 => data["Config"]["Env"]
                    .as_array_mut()
                    .unwrap()
                    .push(json!("CONSTELLA_KEY=/other/key")),
                3 => data["Config"]["Env"]
                    .as_array_mut()
                    .unwrap()
                    .push(json!("CONSTELLA_ADDR=override")),
                _ => data["NetworkSettings"]["Ports"] = json!({}),
            };
            assert!(Target::parse(&data).is_err());
        }
    }
    #[test]
    #[ignore = "requires explicitly provisioned disposable Docker mining fixture"]
    fn docker_reward_transfer_lost_ack_and_retry() {
        use crate::{model::Wallet, store::Store};
        let container = std::env::var("CONSTELLA_TEST_MINER").expect("Run make wallet-mining-test");
        let expected = std::env::var("CONSTELLA_TEST_MINER_ADDRESS").unwrap();
        let binary = PathBuf::from(std::env::var_os("CONSTELLA_WALLET_TEST_BINARY").unwrap());
        let mut wrong =
            MiningBackend::with_binary(&container, &"ff".repeat(32), binary.clone()).unwrap();
        assert!(wrong.call(&["address", KEY]).is_err());
        assert!(wrong.call(&["new", KEY]).is_err());
        // Replace the adapter after refusal; node and key remain untouched.
        wrong = MiningBackend::with_binary(&container, &expected, binary).unwrap();
        let peer = wrong.peer().to_owned();
        let tmp = tempfile::tempdir().unwrap();
        let directory = tmp.path().join("receipts");
        let mut wallet = Wallet::new(
            Box::new(wrong),
            Store::open(directory.clone()).unwrap(),
            PathBuf::from(KEY),
            peer,
            String::new(),
            false,
        );
        wallet.connect_mining(&expected, "bb".repeat(32)).unwrap();
        let review = wallet.review(&"bb".repeat(32), "1", "0.001").unwrap();
        let tx = wallet.send(&review).unwrap();
        assert_eq!(tx["status"], "unknown");
        drop(std::mem::replace(&mut wallet.store, Store::memory()));
        wallet.store = Store::open(directory).unwrap();
        assert!(wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
        let retry = wallet.retry(text(&tx, "id")).unwrap();
        assert_eq!(retry["status"], "duplicate");
        assert_eq!(retry["raw"], tx["raw"]);
        assert_eq!(wallet.snapshot().receipts.len(), 1);
    }
}
