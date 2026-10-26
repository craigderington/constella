# Static runtime redistribution notices

These notices accompany the native binary bundle; they do not replace project
or crate/module licenses. Retain the full `licenses/` directory when distributing
artifacts. Node and Explorer images include the applicable subset.

- `musl/COPYRIGHT`: musl distribution copyright/permission notice, retained from
  the build workstation's musl package. Upstream: https://musl.libc.org/.
- `gcc/RUNTIME.LIBRARY.EXCEPTION`: GCC Runtime Library Exception 3.1, retained
  from the build workstation's GCC package; covers eligible compiled runtime use.
- `go/LICENSE`: Go 1.27.3 runtime license from that installed compiler release.
- `rust-1.98.1/COPYRIGHT-library.html`: complete Rust 1.98.1 standard-library
  copyright and third-party license report from the qualified official toolchain
  distribution's `share/doc/rust/`. This includes notices beyond Cargo.lock.

For other compiler/runtime versions, qualify and refresh the relevant notice
inventory before distributing their output. Container base images have separate
package/license inventories; the scratch node/Explorer carry no base filesystem.
