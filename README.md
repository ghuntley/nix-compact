# nix-compact

**Full build logs on disk. Small, useful output for humans and coding agents.**

nix-compact source-patches Nix's native CLI to suppress routine `nix build` output,
report progress every 15 seconds, and surface bounded diagnostic excerpts. It
prints the full-log location and supplies a reader for targeted investigation.
It is not a shell alias or a subprocess wrapper.

```text
[nix] Output suppressed; progress every 15s. Full log: /home/user/.local/state/nix/build-logs/session.jsonl
[nix] L# = log line; D# = diagnostic. Read: nix build-log '/home/user/.local/state/nix/build-logs/session.jsonl' --diagnostics
[nix] 15s: 3 built, 1 running, 8 copied; phase: checkPhase
[nix] D1 — example-app — level-json/error
L842 [example-app] [rendered] Connection refused
[nix] SUCCESS in 19s | exit 0 | 1 diagnostic candidates; 0 omitted; 0 repeats
[nix] Log: /home/user/.local/state/nix/build-logs/session.jsonl
```

An application saying `error` does **not** fail the build. Only Nix's actual
execution result determines its exit status. Detection affects presentation only.

## Try it

```sh
nix run github:ghuntley/nix-compact -- build .#example-app
```

Or add the executable to a temporary shell:

```sh
nix shell github:ghuntley/nix-compact
nix build .#example-app
```

Inspect the archive advertised by that invocation:

```sh
nix build-log /absolute/path/session.jsonl --diagnostics --limit 3
nix build-log /absolute/path/session.jsonl --diagnostic D1 --context 40
nix build-log /absolute/path/session.jsonl --lines 800:900
```

`L#` identifies a received payload line; `D#` identifies a diagnostic. Read results
default to 100 lines / 8 KiB and include continuation commands when shortened.

## Consume as a flake

```nix
inputs.nix-compact.url = "github:ghuntley/nix-compact";
```

Commit your `flake.lock` to pin the source revision. The standalone flake pins its
own package inputs; following a consumer's unrelated Nixpkgs is unnecessary.

### NixOS

```nix
imports = [ inputs.nix-compact.nixosModules.default ];
programs.nix-compact.enable = true;
```

The module defaults `nix.package` to the project's tested package. Its package
input is independent of the consuming host's Nixpkgs version. Enable it explicitly
and rebuild the system normally. An explicit `nix.package` override takes priority.

### devenv

In `devenv.yaml`:

```yaml
inputs:
  nix-compact:
    url: github:ghuntley/nix-compact
```

In `devenv.nix`:

```nix
{ inputs, ... }: {
  imports = [ inputs.nix-compact.devenvModules.default ];
}
```

Commit `devenv.lock` to pin the dependency. Alternatively add
`inputs.nix-compact.packages.x86_64-linux.default` to your shell's packages.

### Public outputs

| Output | Purpose |
| --- | --- |
| `packages.x86_64-linux.default` / `.nix-compact` | Complete patched Nix package |
| `apps.x86_64-linux.default` | Patched `nix` executable |
| `devShells.x86_64-linux.default` | Contributor toolchain and native test environment |
| `nixosModules.default` | Opt-in NixOS module |
| `devenvModules.default` | Add the pinned package to devenv |
| `lib.mkPackage { pkgs; }` | Build with an explicitly supplied compatible package set |
| `lib.mkCoreCheck { pkgs; }` | Core C++/Hegel check factory |
| `lib.mkNativeCheck { pkgs; package ? …; }` | Real CLI/runtime check factory |
| `checks.x86_64-linux.*` | `core`, `native`, `native-development`, `configuration` |

**Supported:** Nix 2.34.8, `x86_64-linux`. Unsupported versions/platforms fail
explicitly. The native suite is exercised against both stable NixOS and
development package inputs. Other platforms are not advertised as supported.

## Bounds and compatibility

- At most one 160-byte progress line every 15 seconds.
- Up to 20 preceding and 20 following lines per error; 4 KiB per excerpt.
- At most 12 KiB automatic diagnostic details per invocation. Low-confidence
  word matches get at most 4 KiB of that total, reserving capacity for clearer errors.
- A separate 2 KiB final-result allowance.
- Structured Elixir LoggerJSON, Rust tracing and Cargo diagnostics are recognized
  before heuristic text matching.
- All received diagnostic/builder payloads are saved before console filtering.
- Private archives, exact line references, repeated-diagnostic counting, bounded
  context, configurable completed-log retention and visible capture-failure fallback.
- `--json` and `--print-out-paths` preserve machine-readable stdout.

Use `nix build -L …`, an explicit `--log-format`, or `NIX_COMPACT_LOG=0` to select
upstream output and bypass compact capture. The patch does not alter daemon/store
protocols or build scheduling. Capture starts after command parsing and cannot
recover messages disabled by their producer or historical substituted-build logs.

See [MANUAL.md](MANUAL.md) for storage, retention, archive format, limits and replay.

## Develop and test

This repository was initialized with devenv. Both entry points share the pinned
toolchain definition:

```sh
devenv shell
# or
nix develop
```

```sh
nix flake check --no-build --option allow-import-from-derivation false
nix build --no-link .#default .#checks.x86_64-linux.core \
  .#checks.x86_64-linux.native .#checks.x86_64-linux.native-development \
  .#checks.x86_64-linux.configuration
```

Hegel's native C ABI tests the actual C++: 1,000 generated core cases and 50
generated real-build cases per native variant, plus deterministic/runtime
fixtures. Coverage includes arbitrary bytes, structured severity, interleaving,
output budgets, pagination, retention, Unix daemon and remote protocol capture,
TTY, quiet builds, interruptions, stdout compatibility and unchanged exit status.
The complete package also retains upstream Nix test gates.

A synthetic 10,000-line build in the original implementation retained every
builder line while reducing stderr from 359,114 to 609 bytes (over 99.8%). This is
a console-byte measurement for that fixture, not a universal tokenizer guarantee.

## License

LGPL-2.1-or-later. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md) for licensing,
upstream attribution and extraction provenance. Architecture and maintenance
boundaries are documented in [DESIGN.md](DESIGN.md).
