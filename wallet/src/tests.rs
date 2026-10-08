use super::*;
use anyhow::{Result, bail, ensure};
use backend::Core;
use model::{CHAIN, COIN, amount, number};
use serde_json::{Value, json};
use std::{
    fs,
    os::unix::fs::PermissionsExt,
    sync::{Arc, Mutex},
};
#[derive(Default)]
struct FakeState {
    calls: Vec<Vec<String>>,
    nonce: u64,
    next: u64,
    lost: bool,
    mismatch: bool,
    state: PathBuf,
}
struct Fake(Arc<Mutex<FakeState>>);
impl Core for Fake {
    fn call(&self, a: &[&str]) -> Result<Value> {
        let mut s = self.0.lock().unwrap();
        s.calls.push(a.iter().map(|s| s.to_string()).collect());
        match a[0] {
            "balance" => Ok(
                json!({"ok":true,"balance":"1000000000","nonce":s.nonce.to_string(),"next":s.next.to_string(),"height":42}),
            ),
            "prepare" => Ok(
                json!({"id":"cc".repeat(32),"raw":"ab".repeat(152),"from":"aa".repeat(32),"to":if s.mismatch {"dd".repeat(32)} else {a[3].into()},"amount":amount(a[4])?.to_string(),"fee":amount(a[5])?.to_string(),"nonce":a[6]}),
            ),
            "broadcast" => {
                let disk: Value = serde_json::from_slice(&fs::read(&s.state)?)?;
                ensure!(
                    disk["receipts"][0]["raw"] == a[2],
                    "Transaction not durable before broadcast"
                );
                if s.lost {
                    bail!("Lost acknowledgement");
                }
                Ok(json!({"status":"accepted"}))
            }
            "address" | "new" => Ok(json!({"address":"aa".repeat(32)})),
            _ => bail!("unexpected command"),
        }
    }
}
struct Fixture {
    _tmp: tempfile::TempDir,
    state: Arc<Mutex<FakeState>>,
    wallet: Wallet,
}
impl Fixture {
    fn new() -> Self {
        let tmp = tempfile::tempdir().unwrap();
        let directory = tmp.path().join("state");
        let state = Arc::new(Mutex::new(FakeState {
            state: directory.join("state.json"),
            ..Default::default()
        }));
        let mut wallet = Wallet::new(
            Box::new(Fake(state.clone())),
            Store::open(directory).unwrap(),
            tmp.path().join("wallet.key"),
            "127.0.0.1:18473".into(),
            String::new(),
            false,
        );
        wallet.view.address = "aa".repeat(32);
        Self {
            _tmp: tmp,
            state,
            wallet,
        }
    }
    fn review(&mut self) -> Value {
        self.wallet.review(&"bb".repeat(32), "1", "0.001").unwrap()
    }
    fn sent(&mut self) -> Value {
        let review = self.review();
        self.wallet.send(&review).unwrap()
    }
    fn history(&self, tx: &Value, status: &str) -> (Value, Value) {
        let mut row = tx.clone();
        for k in ["amount", "fee", "nonce"] {
            row[k] = json!(number(&row[k]).unwrap());
        }
        row["height"] = json!(43);
        row["status"] = json!(status);
        (
            json!({"meta":{"chain_id":CHAIN,"height":"45"}}),
            json!({"address":self.wallet.view.address,"txs":[row]}),
        )
    }
}
#[test]
fn decimal_boundaries() {
    assert_eq!(amount("184467440737.09551615").unwrap(), u64::MAX);
    assert_eq!(amount("0.00000001").unwrap(), 1);
    for s in [
        "",
        ".1",
        "1.",
        "1e2",
        "-1",
        "NaN",
        "1.000000001",
        "184467440737.09551616",
        "184467440738",
        "1.2.3",
    ] {
        assert!(amount(s).is_err(), "{s}");
    }
    assert_eq!(amount("001.100").unwrap(), 110000000);
    assert_eq!(model::safe("hi\x1b\n\u{202e}"), "hi   ");
}
#[test]
fn review_only_queries_and_checks_balance_nonce_fee() {
    let mut f = Fixture::new();
    f.review();
    assert_eq!(f.state.lock().unwrap().calls[0][0], "balance");
    assert_eq!(f.state.lock().unwrap().calls.len(), 1);
    for (n, fee) in [
        ("11", "0.001"),
        ("1", "2"),
        ("0", "0"),
        ("1e1", "0"),
        ("184467440737.09551615", "1"),
    ] {
        assert!(f.wallet.review(&"bb".repeat(32), n, fee).is_err());
    }
    f.state.lock().unwrap().next = 1;
    assert!(f.wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
}
#[test]
fn save_before_broadcast_and_block_second_send() {
    let mut f = Fixture::new();
    assert_eq!(f.sent()["status"], "accepted");
    assert!(f.wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
    let m = fs::metadata(f.wallet.store.directory.join("state.json")).unwrap();
    assert_eq!(m.permissions().mode() & 0o777, 0o600);
}
#[test]
fn disk_failure_blocks_send_and_manual_retry() {
    let mut f = Fixture::new();
    let review = f.review();
    let dir = f.wallet.store.directory.clone();
    f.wallet.store.directory = dir.join("missing");
    assert!(f.wallet.send(&review).is_err());
    assert!(f.wallet.retry(&"cc".repeat(32)).is_err());
    assert!(
        !f.state
            .lock()
            .unwrap()
            .calls
            .iter()
            .any(|a| a[0] == "broadcast")
    );
    f.wallet.store.directory = dir;
    assert_eq!(
        f.wallet.retry(&"cc".repeat(32)).unwrap()["status"],
        "accepted"
    );
}
#[test]
fn lost_ack_restart_retries_identical_bytes() {
    let mut f = Fixture::new();
    f.state.lock().unwrap().lost = true;
    let tx = f.sent();
    assert_eq!(tx["status"], "unknown");
    let directory = f.wallet.store.directory.clone();
    drop(std::mem::replace(&mut f.wallet.store, Store::memory()));
    f.wallet.store = Store::open(directory).unwrap();
    assert!(f.wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
    f.state.lock().unwrap().lost = false;
    f.wallet.retry(model::text(&tx, "id")).unwrap();
    let state = f.state.lock().unwrap();
    let broadcasts: Vec<_> = state.calls.iter().filter(|a| a[0] == "broadcast").collect();
    assert_eq!(broadcasts.len(), 2);
    assert_eq!(broadcasts[0][2], broadcasts[1][2]);
    assert_eq!(state.calls.iter().filter(|a| a[0] == "prepare").count(), 1);
}
#[test]
fn changed_settings_and_mismatched_signature_refuse() {
    let mut f = Fixture::new();
    let review = f.review();
    f.wallet.view.peer = "other:1".into();
    assert!(f.wallet.send(&review).is_err());
    assert!(
        !f.state
            .lock()
            .unwrap()
            .calls
            .iter()
            .any(|a| a[0] == "prepare")
    );
    let mut f = Fixture::new();
    let review = f.review();
    f.state.lock().unwrap().mismatch = true;
    assert!(f.wallet.send(&review).is_err());
    assert!(
        !f.state
            .lock()
            .unwrap()
            .calls
            .iter()
            .any(|a| a[0] == "broadcast")
    );
}
#[test]
fn inclusion_reorg_and_stale_explorer() {
    let mut f = Fixture::new();
    let tx = f.sent();
    let (stats, data) = f.history(&tx, "applied");
    f.wallet.view.account = Some(json!({"nonce":"1"}));
    f.wallet.reconcile(&stats, &data).unwrap();
    assert_eq!(f.wallet.snapshot().receipts[0]["status"], "applied");
    let (_, orphan) = f.history(&tx, "orphaned");
    f.wallet.reconcile(&stats, &orphan).unwrap();
    assert_eq!(f.wallet.snapshot().receipts[0]["status"], "unverified");
    f.wallet.reconcile(&stats, &data).unwrap();
    f.wallet.view.account = Some(json!({"nonce":"0"}));
    f.wallet.reconcile(&stats, &data).unwrap();
    assert_eq!(f.wallet.snapshot().receipts[0]["status"], "unverified");
    let mut wrong = stats;
    wrong["meta"]["chain_id"] = json!("wrong");
    assert!(f.wallet.reconcile(&wrong, &data).is_err());
}
#[test]
fn state_lock_symlinks_and_unknown_fields_preserved() {
    let mut f = Fixture::new();
    let directory = f.wallet.store.directory.clone();
    assert!(Store::open(directory.clone()).is_err());
    let link = f._tmp.path().join("alias");
    std::os::unix::fs::symlink(&directory, &link).unwrap();
    assert!(Store::open(link).is_err());
    f.wallet.store.data["future_field"] = json!({"keep":true});
    f.wallet.store.save().unwrap();
    drop(std::mem::replace(&mut f.wallet.store, Store::memory()));
    let store = Store::open(directory.clone()).unwrap();
    assert_eq!(store.data["future_field"]["keep"], true);
    drop(store);
    fs::write(directory.join("state.json"), b"not json").unwrap();
    assert!(Store::open(directory.clone()).is_err());
    assert_eq!(fs::read(directory.join("state.json")).unwrap(), b"not json");
}
#[test]
fn backup_private_no_overwrite_and_watch_cannot_access_keys() {
    let mut f = Fixture::new();
    fs::write(&f.wallet.view.key, b"test secret").unwrap();
    fs::set_permissions(&f.wallet.view.key, fs::Permissions::from_mode(0o600)).unwrap();
    let target = f._tmp.path().join("backup.key");
    f.wallet.backup(&target).unwrap();
    assert_eq!(
        fs::metadata(&target).unwrap().permissions().mode() & 0o777,
        0o600
    );
    assert!(f.wallet.backup(&target).is_err());
    f.wallet.view.watch = true;
    f.state.lock().unwrap().calls.clear();
    assert!(f.wallet.backup(&target).is_err());
    assert!(
        f.wallet
            .open(target.clone(), "x".into(), String::new(), false)
            .is_err()
    );
    assert!(f.wallet.review(&"bb".repeat(32), "1", "0").is_err());
    assert!(f.state.lock().unwrap().calls.is_empty());
}
#[test]
fn explorer_remote_plaintext_credentials_and_invalid_amounts_refused() {
    for base in [
        "http://example.test",
        "https://user:secret@example.test",
        "https://example.test?q=1",
        "ftp://example.test",
    ] {
        assert!(model::explorer_json(base, "/api/stats").is_err());
    }
    let mut f = Fixture::new();
    let tx = f.sent();
    let (stats, mut data) = f.history(&tx, "applied");
    data["txs"][0]["amount"] = json!(1.5);
    assert!(f.wallet.reconcile(&stats, &data).is_err());
}
#[test]
fn contact_and_legacy_receipt_roundtrip() {
    let mut f = Fixture::new();
    f.wallet.contact("Mini", &"bb".repeat(32)).unwrap();
    let r = f.wallet.review("Mini", "1", "0").unwrap();
    assert_eq!(r["to"], "bb".repeat(32));
    f.wallet.send(&r).unwrap();
    let directory = f.wallet.store.directory.clone();
    drop(std::mem::replace(&mut f.wallet.store, Store::memory()));
    f.wallet.store = Store::open(directory).unwrap();
    assert_eq!(f.wallet.snapshot().receipts.len(), 1);
    assert_eq!(f.wallet.store.data["version"], 1);
    assert_eq!(
        f.wallet.store.data["receipts"][0]["amount"],
        COIN.to_string()
    );
}
#[test]
fn terminal_layouts_and_confirmation_gate() {
    use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
    use ratatui::{Terminal, backend::TestBackend, layout::Rect};
    let mut w = ui::demo_wallet();
    let mut u = ui::Ui::new(w.snapshot());
    for (width, height) in [(78, 24), (80, 24), (100, 30), (140, 40), (40, 12)] {
        let mut terminal = Terminal::new(TestBackend::new(width, height)).unwrap();
        for tab in 0..4 {
            u.tab = tab;
            terminal.draw(|f| u.draw(f)).unwrap();
        }
    }
    let enter = KeyEvent::new(KeyCode::Enter, KeyModifiers::NONE);
    let esc = KeyEvent::new(KeyCode::Esc, KeyModifiers::NONE);
    u.tab = 1;
    assert!(u.key(enter, Rect::new(0, 0, 80, 24)).unwrap().is_none());
    assert!(u.form.is_none());
    // Use a spendable snapshot to exercise the actual confirmation form.
    let mut f = Fixture::new();
    let review = f.review();
    u.view = f.wallet.snapshot();
    u.review = Some(review.clone());
    u.test_confirm("");
    assert!(u.key(enter, Rect::new(0, 0, 80, 24)).unwrap().is_none());
    u.test_confirm("SEND");
    assert!(u.key(enter, Rect::new(0, 0, 40, 12)).unwrap().is_none());
    let mut terminal = Terminal::new(TestBackend::new(78, 24)).unwrap();
    terminal.draw(|f| u.draw(f)).unwrap();
    let b = terminal.backend().buffer();
    let screen = (0..24)
        .map(|y| (0..78).map(|x| b[(x, y)].symbol()).collect::<String>())
        .collect::<Vec<_>>()
        .join("\n");
    for s in [
        "Total:",
        "Sign and send",
        &"aa".repeat(32),
        &"bb".repeat(32),
        CHAIN,
    ] {
        assert!(screen.contains(s), "missing {s}: {screen}");
    }
    assert!(matches!(
        u.key(enter, Rect::new(0, 0, 80, 24)).unwrap(),
        Some(ui::Action::Send(_))
    ));
    u.test_confirm("SEND");
    assert!(u.key(esc, Rect::new(0, 0, 80, 24)).unwrap().is_none());
    assert!(u.form.is_none());
    assert!(u.review.is_none());
}

// Run explicitly with `--ignored`: disposable authenticated loopback peer, no live funds.
#[test]
#[ignore = "requires loopback networking and compiled C protocol fixtures"]
fn real_core_lost_ack_recovery_through_rust_model() {
    use std::{
        net::TcpListener,
        process::{Command, Stdio},
        thread,
        time::Duration,
    };
    struct Child(std::process::Child);
    impl Drop for Child {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .to_owned();
    let core = Backend::new(Some(
        std::env::var_os("CONSTELLA_WALLET_TEST_CORE")
            .map(PathBuf::from)
            .unwrap_or_else(|| root.join("constella-wallet-core")),
    ))
    .unwrap();
    let tmp = tempfile::tempdir().unwrap();
    let key = tmp.path().join("wallet.key");
    let addr = core.call(&["new", key.to_str().unwrap()]).unwrap()["address"]
        .as_str()
        .unwrap()
        .to_owned();
    let sock = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = sock.local_addr().unwrap().port();
    drop(sock);
    let mut peer = Child(
        Command::new(root.join("wallet/tests/peer-test"))
            .arg(port.to_string())
            .arg("drop")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    );
    let node = format!("127.0.0.1:{port}");
    let mut ready = false;
    for _ in 0..100 {
        if core.call(&["balance", &node, &addr]).is_ok() {
            ready = true;
            break;
        }
        assert!(peer.0.try_wait().unwrap().is_none());
        thread::sleep(Duration::from_millis(20));
    }
    assert!(ready);
    let directory = tmp.path().join("state");
    let mut wallet = Wallet::new(
        Box::new(core),
        Store::open(directory.clone()).unwrap(),
        key.clone(),
        node.clone(),
        String::new(),
        false,
    );
    wallet.open(key, node, String::new(), false).unwrap();
    let review = wallet.review(&"bb".repeat(32), "1", "0.001").unwrap();
    let tx = wallet.send(&review).unwrap();
    assert_eq!(tx["status"], "unknown");
    drop(std::mem::replace(&mut wallet.store, Store::memory()));
    wallet.store = Store::open(directory).unwrap();
    assert!(wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
    let retry = wallet.retry(model::text(&tx, "id")).unwrap();
    assert_eq!(retry["status"], "duplicate");
    assert_eq!(retry["raw"], tx["raw"]);
    assert_eq!(wallet.snapshot().receipts.len(), 1);
}

#[test]
fn fresh_peer_rollback_blocks_review_without_explorer() {
    let mut f = Fixture::new();
    f.sent();
    f.wallet.store.data["receipts"][0]["status"] = json!("applied");
    f.wallet.store.save().unwrap();
    assert!(f.wallet.review(&"bb".repeat(32), "1", "0.001").is_err());
    assert_eq!(f.wallet.snapshot().receipts[0]["status"], "unverified");
    let disk: Value =
        serde_json::from_slice(&fs::read(f.wallet.store.directory.join("state.json")).unwrap())
            .unwrap();
    assert_eq!(disk["receipts"][0]["status"], "unverified");
}
