mod backend;
mod mining;
mod model;
mod store;
#[cfg(test)]
mod tests;
mod ui;
use anyhow::{Context, Result, ensure};
use backend::{Backend, Core};
use clap::Parser;
use model::{Wallet, address};
use std::{io::IsTerminal, path::PathBuf};
use store::Store;

#[derive(Parser)]
#[command(
    version,
    about = "Constella testnet-v5 wallet — no Docker or local miner required"
)]
struct Args {
    #[arg(long)]
    wallet: Option<PathBuf>,
    /// Authenticated P2P host:port
    #[arg(long)]
    node: Option<String>,
    /// HTTPS Explorer; empty disables history
    #[arg(long)]
    explorer: Option<String>,
    /// Public address; disables all key access and signing
    #[arg(long)]
    watch: Option<String>,
    #[arg(long)]
    state_dir: Option<PathBuf>,
    /// Optional external v5 core (migration/diagnostics)
    #[arg(long)]
    backend: Option<PathBuf>,
    /// Offline interactive preview; no files, keys or network
    #[arg(long)]
    demo: bool,
    /// Enable confirmed transfers from a local Docker miner; never exports its key
    #[arg(long, requires="mining_address", conflicts_with_all=["wallet","watch","backend","demo","node"])]
    mining_container: Option<String>,
    /// Expected mining payout address; checked against the key before use
    #[arg(long, requires = "mining_container")]
    mining_address: Option<String>,
    /// Personal wallet used to prefill the transfer recipient
    #[arg(long, requires = "mining_container")]
    personal_wallet: Option<PathBuf>,
}
pub fn expand(path: PathBuf) -> Result<PathBuf> {
    let path = if path.starts_with("~") {
        PathBuf::from(std::env::var_os("HOME").context("HOME is not set")?)
            .join(path.strip_prefix("~")?)
    } else {
        path
    };
    Ok(if path.is_absolute() {
        path
    } else {
        std::env::current_dir()?.join(path)
    })
}
fn run() -> Result<()> {
    let args = Args::parse();
    ensure!(
        std::io::stdin().is_terminal() && std::io::stdout().is_terminal(),
        "Open the wallet in an interactive terminal"
    );
    unsafe {
        libc::umask(0o077);
    }
    if args.demo {
        return ui::demo();
    }
    let home = PathBuf::from(std::env::var_os("HOME").context("HOME is not set")?);
    if let Some(container) = args.mining_container {
        let expected = address(args.mining_address.as_deref().unwrap())?;
        let personal =
            mining::personal_state(&home.join(".local/share/constella-wallet-v5/state.json"))?;
        let personal_key = expand(args.personal_wallet.unwrap_or_else(|| {
            personal["config"]["wallet"]
                .as_str()
                .map(PathBuf::from)
                .unwrap_or_else(|| home.join(".constella-testnet-v5/wallet.key"))
        }))?;
        let personal_address = if personal_key.exists() {
            // Only the public address crosses the C key boundary.
            Backend::new(None)?
                .call(&[
                    "address",
                    personal_key
                        .to_str()
                        .context("Invalid personal wallet path")?,
                ])
                .ok()
                .and_then(|v| v["address"].as_str().and_then(|a| address(a).ok()))
                .unwrap_or_default()
        } else {
            String::new()
        };
        let mut store = Store::open(expand(args.state_dir.unwrap_or_else(|| {
            home.join(".local/share/constella-wallet-mining-v5")
                .join(&expected)
        }))?)?;
        store.import_mining(&personal, &expected)?;
        let core = mining::MiningBackend::new(&container, &expected)?;
        let peer = core.peer().to_owned();
        let mut wallet = Wallet::new(
            Box::new(core),
            store,
            PathBuf::from("/data/wallet.key"),
            peer,
            args.explorer
                .unwrap_or_else(|| "https://explorer.catasterism.xyz".into()),
            false,
        );
        wallet.connect_mining(&expected, personal_address)?;
        return ui::run(wallet);
    }
    let store = Store::open(expand(
        args.state_dir
            .unwrap_or_else(|| home.join(".local/share/constella-wallet-v5")),
    )?)?;
    let config = &store.data["config"];
    let key = expand(args.wallet.unwrap_or_else(|| {
        config["wallet"]
            .as_str()
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".constella-testnet-v5/wallet.key"))
    }))?;
    let peer = args.node.unwrap_or_else(|| {
        config["peer"]
            .as_str()
            .unwrap_or("explorer.catasterism.xyz:7043")
            .into()
    });
    let explorer = args.explorer.unwrap_or_else(|| {
        config["explorer"]
            .as_str()
            .unwrap_or("https://explorer.catasterism.xyz")
            .into()
    });
    let core = Backend::new(args.backend)?;
    let mut wallet = Wallet::new(
        Box::new(core),
        store,
        key.clone(),
        peer.clone(),
        explorer.clone(),
        args.watch.is_some(),
    );
    if let Some(addr) = args.watch {
        wallet.view.address = address(&addr)?;
    } else if std::fs::symlink_metadata(&key).is_ok() {
        wallet.open(key, peer, explorer, false)?;
    }
    ui::run(wallet)
}
fn main() {
    #[cfg(feature = "embedded-core")]
    match std::env::args_os().nth(1).as_deref() {
        Some(flag) if flag == "--wallet-core" => {
            embedded_main(false);
            return;
        }
        // The C resolver re-execs /proc/self/exe with an empty environment.
        // Dispatch before Clap, terminal setup, or any wallet/key access.
        Some(flag) if flag == "--resolve-seed" => {
            embedded_main(true);
            return;
        }
        _ => {}
    }
    if let Err(e) = run() {
        eprintln!("constella-wallet: {e:#}");
        std::process::exit(1);
    }
}
#[cfg(feature = "embedded-core")]
fn embedded_main(resolver: bool) {
    use std::{ffi::CString, os::unix::ffi::OsStrExt};
    unsafe extern "C" {
        fn constella_wallet_main(argc: libc::c_int, argv: *mut *mut libc::c_char) -> libc::c_int;
        fn resolve_main(argc: libc::c_int, argv: *mut *mut libc::c_char) -> libc::c_int;
    }
    let args: Vec<CString> = std::env::args_os()
        .skip(if resolver { 0 } else { 1 })
        .map(|a| CString::new(a.as_bytes()).unwrap())
        .collect();
    let mut argv: Vec<*mut libc::c_char> = args.iter().map(|a| a.as_ptr().cast_mut()).collect();
    argv.push(std::ptr::null_mut());
    // Run only in the short-lived helper process. C owns key handling and never returns keys to Rust.
    let code = unsafe {
        libc::alarm(20);
        if resolver {
            resolve_main(args.len() as libc::c_int, argv.as_mut_ptr())
        } else {
            constella_wallet_main(args.len() as libc::c_int, argv.as_mut_ptr())
        }
    };
    unsafe {
        libc::fflush(std::ptr::null_mut());
    }
    std::process::exit(code);
}
