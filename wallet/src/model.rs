use crate::{
    backend::Core,
    store::{Store, private_dir, read_private, valid_raw},
};
use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::OpenOptionsExt,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use zeroize::Zeroizing;
pub const CHAIN: &str = "2094b0868a27b032";
pub const COIN: u64 = 100_000_000;
pub fn amount(s: &str) -> Result<u64> {
    let (whole, fraction) = s.split_once('.').unwrap_or((s, ""));
    ensure!(
        !whole.is_empty()
            && whole.bytes().all(|b| b.is_ascii_digit())
            && fraction.bytes().all(|b| b.is_ascii_digit())
            && fraction.len() <= 8
            && (!s.contains('.') || !fraction.is_empty()),
        "Use a decimal amount with at most 8 places"
    );
    let mut n = whole
        .parse::<u64>()?
        .checked_mul(COIN)
        .context("Amount exceeds supported range")?;
    if !fraction.is_empty() {
        n = n
            .checked_add(fraction.parse::<u64>()? * 10u64.pow(8 - fraction.len() as u32))
            .context("Amount exceeds supported range")?;
    }
    Ok(n)
}
pub fn number(v: &Value) -> Result<u64> {
    v.as_u64()
        .or_else(|| {
            v.as_str().and_then(|s| {
                (!s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
                    .then(|| s.parse().ok())
                    .flatten()
            })
        })
        .context("Invalid unsigned account/transaction value")
}
pub fn coins(n: u64) -> String {
    format!("{}.{:08}", n / COIN, n % COIN)
}
pub fn address(s: &str) -> Result<String> {
    ensure!(
        s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()),
        "An address must contain exactly 64 hexadecimal characters"
    );
    Ok(s.to_ascii_lowercase())
}
pub fn safe(s: &str) -> String {
    s.chars()
        .map(|c| {
            if c.is_ascii() && !c.is_ascii_control() {
                c
            } else {
                ' '
            }
        })
        .collect()
}
pub fn text<'a>(v: &'a Value, k: &str) -> &'a str {
    v[k].as_str().unwrap_or("")
}
pub fn explorer_json(base: &str, route: &str) -> Result<Value> {
    let u = url::Url::parse(base)?;
    ensure!(
        matches!(u.scheme(), "http" | "https")
            && u.host_str().is_some()
            && u.username().is_empty()
            && u.password().is_none()
            && u.query().is_none()
            && u.fragment().is_none(),
        "Explorer must be an HTTPS URL without credentials, query or fragment"
    );
    ensure!(
        u.scheme() == "https" || matches!(u.host_str(), Some("localhost" | "127.0.0.1" | "[::1]")),
        "Use HTTPS for a remote Explorer"
    );
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(5))
        .redirect(reqwest::redirect::Policy::none())
        .build()?;
    let response = client
        .get(format!("{}{route}", base.trim_end_matches('/')))
        .header("Accept", "application/json")
        .header("User-Agent", "Constella-Wallet/0.2")
        .send()?;
    ensure!(
        response.status().is_success(),
        "Explorer returned {}",
        response.status()
    );
    let mut bytes = Vec::new();
    response.take(1_000_001).read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= 1_000_000, "Explorer response is too large");
    Ok(serde_json::from_slice(&bytes)?)
}
#[derive(Clone, Debug)]
pub struct Snapshot {
    pub address: String,
    pub key: PathBuf,
    pub peer: String,
    pub explorer: String,
    pub account: Option<Value>,
    pub account_at: Option<Instant>,
    pub history: Vec<Value>,
    pub receipts: Vec<Value>,
    pub contacts: Value,
    pub chain_height: u64,
    pub node_error: String,
    pub history_error: String,
    pub watch: bool,
    pub demo: bool,
}
impl Snapshot {
    pub fn rows(&self) -> Vec<Value> {
        let mut rows = self.receipts.clone();
        rows.extend(
            self.history
                .iter()
                .filter(|h| !self.receipts.iter().any(|r| r["id"] == h["id"]))
                .cloned(),
        );
        rows.sort_by_key(|r| std::cmp::Reverse(number(&r["height"]).unwrap_or(u64::MAX)));
        rows
    }
}
pub struct Wallet {
    pub core: Box<dyn Core>,
    pub store: Store,
    pub view: Snapshot,
}
impl Wallet {
    pub fn new(
        core: Box<dyn Core>,
        store: Store,
        key: PathBuf,
        peer: String,
        explorer: String,
        watch: bool,
    ) -> Self {
        Self {
            core,
            store,
            view: Snapshot {
                address: String::new(),
                key,
                peer,
                explorer,
                account: None,
                account_at: None,
                history: vec![],
                receipts: vec![],
                contacts: json!({}),
                chain_height: 0,
                node_error: String::new(),
                history_error: String::new(),
                watch,
                demo: false,
            },
        }
    }
    pub fn snapshot(&mut self) -> Snapshot {
        self.view.receipts = self.store.data["receipts"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|r| r["from"] == self.view.address)
            .cloned()
            .collect();
        self.view.contacts = self.store.data["contacts"].clone();
        self.view.clone()
    }
    fn spendable(&self) -> Result<()> {
        ensure!(
            !self.view.watch && !self.view.demo && !self.view.address.is_empty(),
            "Open a spendable wallet first"
        );
        Ok(())
    }
    fn unresolved(&self) -> bool {
        self.store.data["receipts"]
            .as_array()
            .unwrap()
            .iter()
            .any(|r| r["from"] == self.view.address && r["status"] != "applied")
    }
    pub fn open(
        &mut self,
        key: PathBuf,
        peer: String,
        explorer: String,
        create: bool,
    ) -> Result<()> {
        ensure!(
            !self.view.watch && !self.view.demo,
            "Watch-only/demo mode cannot open keys"
        );
        if create {
            private_dir(key.parent().context("Wallet path needs a parent")?)?;
        }
        let result = self.core.call(&[
            if create { "new" } else { "address" },
            key.to_str().context("Invalid wallet path")?,
        ])?;
        let addr = address(text(&result, "address"))?;
        self.store.data["config"] = json!({"wallet":key,"peer":peer,"explorer":explorer});
        self.store.save()?;
        self.view.address = addr;
        self.view.key = key;
        self.view.peer = peer;
        self.view.explorer = explorer;
        self.view.account = None;
        self.view.account_at = None;
        self.view.history.clear();
        self.view.receipts.clear();
        self.view.node_error.clear();
        self.view.history_error.clear();
        Ok(())
    }
    fn balance(&mut self) -> Result<Value> {
        let a = self
            .core
            .call(&["balance", &self.view.peer, &self.view.address])?;
        for k in ["balance", "nonce", "next", "height"] {
            number(&a[k])?;
        }
        ensure!(
            number(&a["next"])? >= number(&a["nonce"])?,
            "Invalid account nonce"
        );
        // A fresh peer rollback invalidates prior inclusion even if Explorer is offline.
        let nonce = number(&a["nonce"])?;
        let mut changed = false;
        for receipt in self.store.data["receipts"].as_array_mut().unwrap() {
            if receipt["from"] == self.view.address
                && receipt["status"] == "applied"
                && nonce <= number(&receipt["nonce"])?
            {
                receipt["status"] = json!("unverified");
                changed = true;
            }
        }
        if changed {
            self.store.save()?;
        }
        self.view.account = Some(a.clone());
        self.view.account_at = Some(Instant::now());
        self.view.node_error.clear();
        Ok(a)
    }
    pub fn refresh(&mut self) {
        if self.view.address.is_empty() || self.view.demo {
            return;
        }
        if let Err(e) = self.balance() {
            self.view.node_error = e.to_string();
        }
        if let Err(e) = self.history() {
            self.view.history_error = format!("History unavailable: {e}");
        }
    }
    fn history(&mut self) -> Result<()> {
        ensure!(
            !self.view.explorer.is_empty(),
            "No Explorer configured; inclusion is unverified"
        );
        let stats = explorer_json(&self.view.explorer, "/api/stats")?;
        let data = explorer_json(
            &self.view.explorer,
            &format!("/api/address/{}", self.view.address),
        )?;
        self.reconcile(&stats, &data)
    }
    pub fn reconcile(&mut self, stats: &Value, data: &Value) -> Result<()> {
        ensure!(
            stats["meta"]["chain_id"] == CHAIN,
            "Explorer is on a different chain"
        );
        let height = number(&stats["meta"]["height"])?;
        ensure!(
            data["address"] == self.view.address,
            "Explorer returned a different account"
        );
        let rows = data["txs"]
            .as_array()
            .context("Invalid transaction history")?;
        for row in rows {
            for k in ["id", "from", "to"] {
                address(text(row, k))?;
            }
            ensure!(
                row["from"] == self.view.address || row["to"] == self.view.address,
                "Unrelated transaction in Explorer"
            );
            for k in ["amount", "fee", "nonce", "height"] {
                ensure!(row[k].is_u64(), "Invalid transaction value");
            }
            ensure!(row["status"].is_string(), "Invalid transaction status");
        }
        let fresh_nonce = if self.view.node_error.is_empty() {
            self.view
                .account
                .as_ref()
                .map(|a| number(&a["nonce"]))
                .transpose()?
        } else {
            None
        };
        let mut changed = false;
        for r in self.store.data["receipts"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .filter(|r| r["from"] == self.view.address)
        {
            let row = rows.iter().find(|h| h["id"] == r["id"]);
            if fresh_nonce.is_some_and(|n| n <= number(&r["nonce"]).unwrap()) {
                if r["status"] == "applied" || row.is_some_and(|h| h["status"] == "applied") {
                    r["status"] = json!("unverified");
                    changed = true;
                }
            } else if let Some(h) = row {
                if h["status"] == "applied"
                    && ["from", "to"].iter().all(|k| h[k] == r[k])
                    && ["amount", "fee", "nonce"]
                        .iter()
                        .all(|k| number(&h[k]).ok() == number(&r[k]).ok())
                {
                    r["status"] = json!("applied");
                    r["height"] = h["height"].clone();
                    changed = true;
                } else {
                    r["status"] = json!("unverified");
                    changed = true;
                }
            }
        }
        if changed {
            self.store.save()?;
        }
        self.view.history = rows.clone();
        self.view.chain_height = height;
        self.view.history_error.clear();
        Ok(())
    }
    pub fn review(&mut self, recipient: &str, value: &str, fee: &str) -> Result<Value> {
        self.spendable()?;
        let recipient = self.store.data["contacts"]
            .get(recipient)
            .and_then(Value::as_str)
            .unwrap_or(recipient);
        let to = address(recipient.trim())?;
        let n = amount(value.trim())?;
        let fee = amount(fee.trim())?;
        ensure!(n > 0, "Amount must be greater than zero");
        let total = n
            .checked_add(fee)
            .context("Total exceeds supported range")?;
        ensure!(fee <= n.min(COIN), "Fee exceeds the wallet safety limit");
        ensure!(
            !self.unresolved(),
            "A saved transfer is still unverified. Check History; R retries that transaction."
        );
        let a = self.balance()?;
        ensure!(
            !self.unresolved(),
            "Peer state changed; verify the saved payment in History before sending"
        );
        let nonce = number(&a["nonce"])?;
        ensure!(
            nonce == number(&a["next"])? && nonce < u64::MAX,
            "Another outgoing transaction is pending or nonce is exhausted"
        );
        ensure!(
            total <= number(&a["balance"])?,
            "Insufficient balance including the fee"
        );
        Ok(
            json!({"from":self.view.address,"to":to,"amount":n,"fee":fee,"nonce":nonce,"peer":self.view.peer,"wallet":self.view.key,"chain_id":CHAIN}),
        )
    }
    pub fn send(&mut self, review: &Value) -> Result<Value> {
        self.spendable()?;
        ensure!(
            review["from"] == self.view.address
                && review["peer"] == self.view.peer
                && review["wallet"] == json!(self.view.key)
                && review["chain_id"] == CHAIN,
            "Wallet settings changed; review the payment again"
        );
        ensure!(
            !self.unresolved(),
            "Resolve the existing saved payment first"
        );
        let mut tx = self.core.call(&[
            "prepare",
            self.view.key.to_str().context("Invalid key path")?,
            &self.view.peer,
            text(review, "to"),
            &coins(number(&review["amount"])?),
            &coins(number(&review["fee"])?),
            &number(&review["nonce"])?.to_string(),
        ])?;
        for k in ["from", "to"] {
            ensure!(
                tx[k] == review[k],
                "Signed transaction does not match review; nothing broadcast"
            );
        }
        for k in ["amount", "fee", "nonce"] {
            ensure!(
                number(&tx[k])? == number(&review[k])?,
                "Signed transaction does not match review; nothing broadcast"
            );
        }
        address(text(&tx, "id"))?;
        ensure!(
            valid_raw(text(&tx, "raw")),
            "Invalid signed transaction; nothing broadcast"
        );
        tx["status"] = json!("saved");
        tx["created_at"] = json!(chrono::Utc::now().to_rfc3339());
        self.store.data["receipts"]
            .as_array_mut()
            .unwrap()
            .push(tx.clone());
        self.store.save()?;
        self.retry(text(&tx, "id"))
    }
    pub fn retry(&mut self, id: &str) -> Result<Value> {
        self.spendable()?;
        let receipts = self.store.data["receipts"].as_array().unwrap();
        let i = receipts
            .iter()
            .position(|r| {
                r["id"] == id && r["from"] == self.view.address && r["status"] != "applied"
            })
            .context("Select a saved unverified payment from this wallet")?;
        // Also persist before manual retry: a prior disk failure may have left a receipt only in memory.
        self.store.save()?;
        let result = self
            .core
            .call(&["broadcast", &self.view.peer, text(&receipts[i], "raw")]);
        let tx = &mut self.store.data["receipts"][i];
        match result {
            Ok(r)
                if matches!(
                    text(&r, "status"),
                    "accepted" | "duplicate" | "bad-signature" | "rejected" | "mempool-full"
                ) =>
            {
                tx["status"] = r["status"].clone();
                tx.as_object_mut().unwrap().remove("error");
            }
            result => {
                tx["status"] = json!("unknown");
                tx["error"] = json!(match result {
                    Err(e) => e.to_string(),
                    _ => "Invalid broadcast response".into(),
                });
            }
        }
        self.store.save()?;
        Ok(self.store.data["receipts"][i].clone())
    }
    pub fn backup(&self, target: &Path) -> Result<()> {
        self.spendable()?;
        self.core.call(&[
            "address",
            self.view.key.to_str().context("Invalid key path")?,
        ])?;
        let data = Zeroizing::new(read_private(&self.view.key)?);
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(target)?;
        file.write_all(&data)?;
        file.sync_all()?;
        let result = self
            .core
            .call(&["address", target.to_str().context("Invalid backup path")?])?;
        ensure!(
            result["address"] == self.view.address,
            "Backup address mismatch; retain the original"
        );
        File::open(target.parent().context("Backup needs a parent directory")?)?.sync_all()?;
        Ok(())
    }
    pub fn contact(&mut self, name: &str, addr: &str) -> Result<()> {
        ensure!(
            !self.view.demo
                && !name.is_empty()
                && name.len() <= 32
                && name.bytes().all(|c| (32..127).contains(&c)),
            "Use a contact name of 1-32 printable characters"
        );
        self.store.data["contacts"][name] = json!(address(addr)?);
        self.store.save()
    }
}
