{ pkgs }:
assert pkgs.lib.assertMsg (pkgs.stdenv.hostPlatform.system == "x86_64-linux")
  "nix-compact currently supports x86_64-linux";
assert pkgs.lib.assertMsg (pkgs.nix.version == "2.34.8")
  "nix-compact supports Nix 2.34.8; use its pinned flake package or a compatible package set";
let
  # Devenv's evaluation cache tracks readFile dependencies. Force a read as well
  # as copying the path so editing a header invalidates a cached shell selection.
  header = path: assert builtins.isPath path; builtins.seq (builtins.readFile path) path;
  # Override only the native CLI component; do not fork daemon libraries or
  # substitute a PATH wrapper. Source edits deliberately fail on upstream drift.
  patched = pkgs.nix.overrideScope (_final: previous: {
    # Upstream tests validate raw CLI compatibility; our checks exercise compact
    # defaults. The completion assertion must include the newly added command.
    nix-functional-tests = previous.nix-functional-tests.overrideAttrs (old: {
      NIX_COMPACT_LOG = "0";
      postPatch = (old.postPatch or "") + ''
        substituteInPlace completions.sh \
          --replace-fail ${pkgs.lib.escapeShellArg "normal\\nbuild\\t"} ${pkgs.lib.escapeShellArg "normal\\nbuild\\t\\nbuild-log\\t"}
      '';
    });
    nix-cli = previous.nix-cli.overrideAttrs (old: {
      postPatch = (old.postPatch or "") + ''
        mkdir compact
        cp ${header ./archive.hh} compact/archive.hh
        cp ${header ./classify.hh} compact/classify.hh
        cp ${header ./session.hh} compact/session.hh
        cp ${header ./reader.hh} compact/reader.hh
        cp ${header ./native.hh} compact/native.hh
        substituteInPlace main.cc \
          --replace-fail '#include "self-exe.hh"' '#include "self-exe.hh"
        #include "compact/native.hh"' \
          --replace-fail '    NixArgs args;' '    NixArgs args;
            bool explicitLogFormat = false;
            args.addFlag({.longName = "log-format", .description = "Select the native log format and disable compact capture.",
                .labels = {"format"}, .handler = {[&](std::string format) {
                    explicitLogFormat = true; setLogFormat(format);
                }}});' \
          --replace-fail '    applyJSONLogger();' '    if (args.command && args.command->first == "build" && !args.helpRequested && !args.completions
                && !explicitLogFormat && !logger->isVerbose() && !settings.verboseBuild && getEnv("NIX_COMPACT_LOG").value_or("1") != "0")
                compact::start();
            applyJSONLogger();' \
          --replace-fail '    return nix::handleExceptions(argv[0], [&]() { nix::mainWrapped(argc, argv); });' '    int result = nix::handleExceptions(argv[0], [&]() { nix::mainWrapped(argc, argv); });
            nix::compact::finish(result);
            return result;'
      '';
    });
  });
in patched
