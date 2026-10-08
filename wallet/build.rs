fn main() {
    println!("cargo:rerun-if-changed=backend.c");
    println!("cargo:rerun-if-changed=../src");
    if std::env::var_os("CARGO_FEATURE_EMBEDDED_CORE").is_none() {
        return;
    }
    let mut build = cc::Build::new();
    build
        .include("../src")
        .define("CONSTELLA_NETWORK", "3")
        .define("CONSTELLA_WALLET_EMBEDDED", None)
        .define("_GNU_SOURCE", None)
        .define("_FORTIFY_SOURCE", "2")
        .flag("-std=c11")
        .flag("-pthread")
        .flag("-fstack-protector-strong")
        .flag("-ffunction-sections")
        .flag("-fdata-sections")
        .opt_level_str("s");
    for source in [
        "blake2b",
        "bn",
        "share",
        "science",
        "tx",
        "wallet",
        "sieve",
        "throttle",
        "miner",
        "chain",
        "ledger",
        "mempool",
        "resolve",
        "net",
        "util",
        "addr",
        "vendor/monocypher",
    ] {
        build.file(format!("../src/{source}.c"));
    }
    build.file("backend.c").compile("constella_wallet_core");
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=m");
}
