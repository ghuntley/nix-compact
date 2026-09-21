{ nixpkgs, module, package }:
let
  system = "x86_64-linux";
  pkgs = nixpkgs.legacyPackages.${system};
  evaluate = extra: (nixpkgs.lib.nixosSystem {
    inherit system;
    modules = [ module extra ];
  }).config;
  enabled = evaluate { programs.nix-compact.enable = true; };
  disabled = evaluate {};
  override = evaluate { programs.nix-compact.enable = true; nix.package = pkgs.nix; };
  custom = evaluate { programs.nix-compact.enable = true; programs.nix-compact.package = pkgs.nix; };
  checks = {
    explicitOptIn = !disabled.programs.nix-compact.enable;
    nativePackageSelected = enabled.nix.package.outPath == package.outPath;
    cliActuallyPatched = package.nix-cli.outPath != pkgs.nix.nix-cli.outPath;
    daemonLibrariesUnchanged = package.libs.nix-store.outPath == pkgs.nix.libs.nix-store.outPath;
    disabledRetainsUpstream = disabled.nix.package.outPath == pkgs.nix.outPath;
    explicitPackageWins = override.nix.package.outPath == pkgs.nix.outPath;
    customPackageHonored = custom.nix.package.outPath == pkgs.nix.outPath;
  };
in assert builtins.all (result: result) (builtins.attrValues checks);
pkgs.runCommand "nix-compact-configuration-check" {
  result = builtins.toJSON checks;
} ''
  printf '%s\n' "$result" > $out
''
