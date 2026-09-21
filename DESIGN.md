# Design and verification boundaries

## Pipeline

The native Nix CLI selects compact mode for ordinary `nix build` after parsing
arguments, before evaluation/build execution. Explicit logging formats and full
streaming retain upstream behavior. Other commands retain their implementations;
`nix build-log` is the added archive reader.

1. **Capture** received diagnostic and builder payloads into private JSONL files.
   Global payload-line IDs, activity IDs and event IDs preserve references under
   interleaving and multiline formatting. Non-UTF-8 bytes are reversibly encoded.
2. **Classify** structured severity first, recognized textual diagnostics second,
   heuristic whole-word matches last. Classification never changes execution.
3. **Present** bounded excerpts and periodic coalesced progress. Context is scoped
   to build activity. Diagnostic bytes, including lower-confidence details, have
   explicit per-invocation budgets. Completion receives the real process result.
4. **Retrieve** bounded ranges or diagnostic excerpts through a reader whose
   continuations preserve filters and intra-line offsets. The sparse index is
   optional and checked against the authoritative archive.

The first suppression notice advertises the full log and explains `L#` and `D#`.
`L#` is a payload-line identifier, not a physical JSONL row: the archive also
contains metadata records. Native/structured primary messages may be rendered for
readability while their identifiers still point to the original payload.

## Public integration

The flake's default package uses its own immutable Nixpkgs pin. Consumers do not
need the original monorepo, its modules, Elixir applications, credentials or tools.
The exported NixOS module is opt-in and uses `programs.nix-compact`; its package
does not follow the host's potentially incompatible Nix version. `lib.mkPackage`
is available when a consumer deliberately wants its own compatible package set.
The version/platform assertions and exact source substitutions fail on drift.

Only the CLI component is patched. Store and daemon libraries remain upstream.
The package keeps upstream test gates, with raw logging selected for upstream
output compatibility and the added command reflected in completion expectations.
Hegel's C ABI is built directly from locked sources; no nested Cargo downloads
are needed in sandboxed checks. It is excluded from production runtime inputs.

`dev-environment.nix` supplies both the initialized devenv environment and the
flake development shell. Header reads are explicit evaluation dependencies so
devenv does not retain a stale CLI after header-only changes.

## Verification

- Core tests: strict C++23 compilation, deterministic classifier/archive cases,
  200 Hegel cases each for capture, structured precedence, interleaved budgets,
  pagination and retention, plus operational edge fixtures.
- Native tests: the same core properties plus 50 generated real-build cases per
  package variant and runtime checks for JSON/paths, TTY, explicit formats, archive
  failure, native errors, Unix daemon, remote protocols, heartbeat and SIGINT.
- Configuration checks: opt-in, selected native CLI, unchanged store library,
  disabled behavior and explicit package overrides.
- Consumer checks: flake app/package/module evaluation and actual development
  shell selection. Public GitHub consumption is checked after publication.

Tests use independently decoded archives and measured console output as oracles.
Real-build properties compare compact and upstream-output execution status and
exercise a subsequent successful invocation. Hegel prints shrunk reproduction
blobs; `NIX_COMPACT_PROPERTY` and `NIX_COMPACT_REPLAY` select replay in the driver.

## Operational boundaries

Capture begins after command parsing and preserves what upstream Nix delivers;
producer-disabled messages, pre-selection startup output and historical cache-hit
logs cannot be reconstructed. Log I/O can backpressure on a slow disk. Retention
is a soft target and never prunes active or incomplete sessions. Bounded live
context may be reduced beyond documented activity limits. Reader limits do not
truncate the saved archive.

Remote fixtures exercise real Nix protocols using localhost transport bypass and
separate stores, not physical-network/fleet availability. Existing systems require
normal rebuild/activation after enabling the module. No deployment or automatic
remote export is performed merely by consuming the flake.
