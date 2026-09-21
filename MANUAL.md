# Compact Nix build logs

The development shell and optional NixOS module use a source-patched Nix 2.34.8.
Ordinary `nix build` saves received logs locally and shows a compact transcript.
This is implemented inside Nix's CLI, not by a shell alias or subprocess wrapper.

```text
[nix] Output suppressed; progress every 15s. Full log: /home/user/.local/state/nix/build-logs/1789989528257-1234-0.jsonl
[nix] L# = log line; D# = diagnostic. Read: nix build-log '/home/user/.local/state/nix/build-logs/1789989528257-1234-0.jsonl' --diagnostics
[nix] 15s: 3 built, 1 running, 8 copied; phase: checkPhase
[nix] D1 — example-app — level-json/error
L842 [example-app] [rendered] Connection refused
[nix] SUCCESS in 19s | exit 0 | 1 diagnostic candidates; 0 omitted; 0 repeats
[nix] Log: /home/user/.local/state/nix/build-logs/1789989528257-1234-0.jsonl
```

An application logging `error`, `fatal`, `failed` or a structured error event does
**not** fail a build. Only Nix's real process result controls `SUCCESS`/`FAILED`
and its exit status. A diagnostic candidate is evidence to investigate, not an
assertion that execution failed. No classification state crosses invocations.

## Read only the evidence you need

Use the absolute path advertised by that invocation. There is no ambiguous
`latest` alias when several builds run concurrently.

```sh
nix build-log /absolute/path/session.jsonl --diagnostics --limit 3
nix build-log /absolute/path/session.jsonl --diagnostic D1 --context 40
nix build-log /absolute/path/session.jsonl --lines 800:900
nix build-log /absolute/path/session.jsonl --build example-app --tail 100
```

`L842` identifies received payload line 842 in the session, while `D1` identifies
a diagnostic. These are independent of source locations such as `src/main.rs:42`.
The JSONL archive also contains session, diagnostic and completion metadata, so
an `L` identifier is **not** a physical JSONL-file row number. Use the reader for
line selection. Build filters preserve global identifiers; gaps belong to other
build activity. Native multiline errors share one event identifier. Rendered
structured diagnostics point back to their original archived payload line.

The reader defaults to 100 payload lines and 8 KiB, whichever is reached first.
It prints an exact continuation command when shortened, including a byte offset
for oversized lines. A diagnostic-index row consumes one row of its limit.
`--after D3` pages the diagnostic index. `--all` explicitly removes output budgets.
`--format original` removes display labels and reconstructs received payload
bytes and their recorded terminators; event boundaries without a received newline
do not acquire an invented newline. The normal labeled view is best for reading
builder line events. `--activity` preserves exact identity in generated
continuations, including when several derivations share a display name.

Reader notices go to stderr; selected data goes to stdout. Indexes are optional:
a missing or corrupt `.jsonl.index` file falls back to the authoritative archive.
The reader rejects malformed records and records larger than 64 MiB rather than
allocating without limit; such records remain in the archive for external tools.

## Output policy

| Output | Default bound |
| --- | --- |
| Progress | One line per 15 seconds, at most 160 bytes |
| Error context | Up to 20 preceding and 20 following lines from the same build |
| Following context window | Two seconds, closed by a new diagnostic or activity end |
| Failure with no earlier detected error | Up to 40 preceding lines from its recently completed builder |
| One diagnostic excerpt | 4 KiB, prioritizing the primary diagnostic |
| Automatic diagnostic details | 12 KiB per invocation; does not refill |
| Low-confidence word matches | At most 4 KiB within that total, reserving space for higher-confidence errors |
| Completion/final failure summary | Separate 2 KiB allowance |
| Warnings | First warning detail, then counts and archived evidence |

Exact repeated short diagnostics are counted rather than reprinted. Overlapping
context is not printed twice. Classification checks native errors, Elixir
LoggerJSON `severity`, Rust tracing `level`, Cargo compiler-message
`message.level`, recognizable textual diagnostics, then error-like whole words.
Recognized structured severity takes precedence over words in payloads. Nix's
mandatory build-plan output uses error *verbosity* upstream but is not treated
as an error merely for that reason.

All received diagnostic and builder payloads are archived before filtering.
This does not recover logging disabled by the producer, pre-command startup
messages, or historical logs of a build that was substituted rather than run.
Upstream Nix may normalize builder bytes before delivering line events; the
archive preserves the payload actually delivered to this logger. Progress
counters drive the heartbeat rather than becoming repeated archived messages.

Bounded live state retains context for up to 256 activities and the last 16
completed builders, plus up to 1,024 short deduplication keys. Beyond those
limits, capture continues but automatic context/deduplication may be reduced.
The complete received history remains available through the reader.

## Full output and compatibility

```sh
nix build -L .#example-app
NIX_COMPACT_LOG=0 nix build .#example-app
nix build --log-format internal-json .#example-app
```

`-L`/`--print-build-logs`, `NIX_COMPACT_LOG=0`, or any explicit `--log-format`
select upstream output and bypass compact capture. `--json` and
`--print-out-paths` continue to produce clean stdout in compact mode. Other Nix
subcommands and the daemon protocol retain upstream behavior. Both TTY and
noninteractive builds receive the same bounded compact presentation.

## Storage and retention

Default location: `${XDG_STATE_HOME:-$HOME/.local/state}/nix/build-logs/`.
An absolute `XDG_STATE_HOME` is required; otherwise the home-directory default
is used. Override the directory with `NIX_BUILD_LOG_DIR`.

- Directories are private (`0700`); session/index files are `0600`.
- Each invocation creates a unique JSONL archive and an optional sparse index.
- Payloads that are not valid UTF-8 use reversible hex encoding in the archive.
- Writes are visible while builds run; completion syncs the archive.
- No completion footer means incomplete or still running, not success.
- Capture creation/write/sync errors announce incomplete capture and restore
  visible output without changing the build result.
- Logs are local build evidence and are not automatically exported to remote
  application logging. No argument/environment snapshot is added.

At session creation, completed logs are pruned using these environment settings:

| Variable | Default | Meaning |
| --- | --- | --- |
| `NIX_BUILD_LOG_KEEP_DAYS` | `7` | Remove older completed sessions; `0` disables age pruning |
| `NIX_BUILD_LOG_MAX_BYTES` | `1073741824` | Soft aggregate archive-byte target; `0` disables size pruning |

Active locked sessions, incomplete sessions and symlinks are never pruned.
Cleanup examines at most 10,000 archives per invocation. Active/incomplete
evidence can exceed the soft target; incomplete sessions require explicit manual
cleanup after investigation. Indexes are removed with their completed archive.

## Package selection

- `devenv shell` selects the custom Nix through `devenv.nix`.
- `packages.x86_64-linux.default` and `.nix-compact` expose the pinned package.
- `apps.x86_64-linux.default` runs its `nix` executable.
- `nixosModules.default` selects the package through `nix.package` when
  `programs.nix-compact.enable = true`. An explicit `nix.package` definition
  takes priority. `programs.nix-compact.package` permits a custom package.
- `devenvModules.default` adds the pinned package to a consuming devenv shell.
- `lib.mkPackage { pkgs = compatiblePkgs; }` builds against a supplied package set
  containing Nix 2.34.8. The other public factories are `lib.mkCoreCheck` and
  `lib.mkNativeCheck`, each accepting `pkgs`; the native check optionally accepts
  an explicit `package` too.

These are declarative configuration changes: existing hosts receive the package
on their normal system rebuild/activation. Refresh an older cached development
shell with `devenv --refresh-eval-cache shell`. Header reads are explicitly tracked
so subsequent source edits invalidate the development evaluation cache.

## Verification and maintenance

```sh
nix build .#checks.x86_64-linux.core
nix build .#checks.x86_64-linux.native
nix build .#checks.x86_64-linux.native-development
nix build .#checks.x86_64-linux.configuration
```

The C++ properties call the pinned Hegel native C ABI directly. Building that
ABI from its locked Rust workspace avoids the Rust frontend's nested Cargo
download/build. Hegel stays a test dependency, outside the runtime package.
Each core run uses 200 cases per property for arbitrary-byte archive round trips,
structured precedence, interleaved output budgets, lossless reader pagination,
and retention safety. Native checks add 50 generated real-build cases, plus
runtime compatibility, Unix daemon, remote protocol, quiet-build and interruption
fixtures. The remote protocol fixtures use Nix's localhost transport bypass and
separate stores; they are not physical-network or fleet-rollout evidence.

Hegel failures print the property name and a shrunk reproduction blob. In the
check's development environment, compile the same driver and replay it:

```sh
nix develop
c++ -std=c++23 -Wall -Wextra -Werror -pthread \
  package/property-test.cc -lhegel_c -o /tmp/nix-compact-properties
NIX_COMPACT_PROPERTY=archive-round-trip NIX_COMPACT_REPLAY='BLOB_FROM_FAILURE' \
  /tmp/nix-compact-properties /tmp/nix-compact-replay
```

Replay is pinned to the Hegel engine and generator version. The runner rejects
unknown property names and creates a uniquely owned fixture directory. Upstream
Nix functional tests remain enabled with raw logging; their completion assertion
includes the new `build-log` command. Nix version changes deliberately fail the
package assertion until source adaptation and both consumer checks are reviewed.

See [the design](DESIGN.md) and [flake usage](README.md).
