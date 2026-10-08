mod backend;
mod model;
mod store;
#[cfg(test)]
mod tests;
mod ui;
use anyhow::{Context, Result, ensure};
use backend::Backend;
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
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--wallet-core")) {
        embedded_main();
        return;
    }
    if let Err(e) = run() {
        eprintln!("constella-wallet: {e:#}");
        std::process::exit(1);
    }
}
#[cfg(feature = "embedded-core")]
fn embedded_main() {
    use std::{ffi::CString, os::unix::ffi::OsStrExt};
    unsafe extern "C" {
        fn constella_wallet_main(argc: libc::c_int, argv: *mut *mut libc::c_char) -> libc::c_int;
    }
    let args: Vec<CString> = std::env::args_os()
        .skip(1)
        .map(|a| CString::new(a.as_bytes()).unwrap())
        .collect();
    let mut argv: Vec<*mut libc::c_char> = args.iter().map(|a| a.as_ptr().cast_mut()).collect();
    argv.push(std::ptr::null_mut());
    // Run only in the short-lived helper process. C owns key handling and never returns keys to Rust.
    let code = unsafe { constella_wallet_main(args.len() as libc::c_int, argv.as_mut_ptr()) };
    unsafe {
        libc::fflush(std::ptr::null_mut());
    }
    std::process::exit(code);
}
