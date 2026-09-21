# nix-compact development

Read `CONTRACTS`, `package/CONTRACTS`, `README.md` and `DESIGN.md` before edits.
Keep source changes compatible with the pinned Nix release. The implementation
belongs in the native CLI; do not replace it with a shell/subprocess wrapper.

Use typed C++23 interfaces and validate external JSON before narrowing values.
Tests use the pinned Hegel C ABI against the same headers as production.
Use `devenv shell` or `nix develop` for the development toolchain.

Required checks:

```sh
nix flake check --no-build
nix build --no-link .#checks.x86_64-linux.core \
  .#checks.x86_64-linux.native .#checks.x86_64-linux.native-development \
  .#checks.x86_64-linux.configuration .#default
git diff --check
```

Review logging contracts semantically as well as running tests. Diagnostic
classification must never change execution results. Preserve all received
payloads even when console budgets are exhausted; capture failures restore visible
logging. Never add arguments/environment snapshots or automatic remote export.
Keep generated-case counts and runtime/transport validation claims precise.

Update public interfaces, supported versions, licensing/provenance and design
documentation together with implementation changes. Limit supported platforms
to those actually verified. Do not commit or push without user authorization.
