# Third-party notices

The project MIT license applies to original Constella source. It does not replace
third-party terms.

- Vendored Monocypher 4.0.2: its BSD-2-Clause / CC0 notice is preserved at
  [src/vendor/MONOCYPHER-LICENCE.md](src/vendor/MONOCYPHER-LICENCE.md) and in source.
- Go dependencies: versions in `explorer/go.mod` and `go.sum`; redistributable
  sources/notices in `explorer/vendor/`, including edwards25519, lib/pq, x/crypto
  and x/sys. Retain that directory's license and patent notices.
- Rust wallet dependencies: versions/checksums in `wallet/Cargo.lock`; their
  individual package licenses remain applicable. Generate the complete cargo
  dependency/license inventory when packaging a wallet binary and include it
  with the release. The source checkout alone does not constitute that binary's
  complete license bundle.
- Container base images and compiler/runtime packages retain their own licenses.
  Image references are pinned in `release/build-inputs.json`; distributed images
  require their applicable notices and source obligations.
- Static runtime notices for musl, Go, Rust 1.98.1 and the GCC Runtime Library Exception are
  retained in `third_party/runtime/`. The scratch node and Explorer images carry
  their project and applicable runtime/dependency notices under `/licenses`.

Release packaging must retain LICENSE, this file and all vendored notices.
