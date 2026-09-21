# License and provenance

SPDX-License-Identifier: LGPL-2.1-or-later

The nix-compact implementation is licensed under the GNU Lesser General Public
License, version 2.1 or (at your option) any later version. See `LICENSE` for the
license text.

This project was extracted, with the author's authorization, from the native
compact logging implementation in `antithesishq/kittens`, commit
[`4bca9a0b1a3656dde55e188b2a6ee4d7693995f7`](https://github.com/antithesishq/kittens/commit/4bca9a0b1a3656dde55e188b2a6ee4d7693995f7).
The extraction preserves the C++ behavior and Hegel tests while generalizing
packaging and public module interfaces. Maintainer: Geoffrey Huntley (@ghuntley).

The package builds and patches [Nix](https://github.com/NixOS/nix), retaining its
upstream copyrights and LGPL licensing. Upstream sources are fetched at the
locked revision rather than vendored here. Nlohmann JSON and Hegel retain their
respective upstream licenses; Hegel is a test/development dependency, not part of
the patched CLI's runtime dependency closure.
