{ pkgs }:
let
  package = import ./package { inherit pkgs; };
  hegel = import ./package/hegel.nix { inherit pkgs; };
in {
  packages = [
    (pkgs.lib.hiPrio package)
    pkgs.git
    pkgs.stdenv.cc
    pkgs.nlohmann_json
    pkgs.pkg-config
    pkgs.cargo
    pkgs.rustc
    hegel
  ];
  env = {
    NIX_COMPACT_TEST_BINARY = "${package.nix-cli}/bin/nix";
    NIX_COMPACT_TEST_BASH = "${pkgs.bash}/bin/bash";
    NIX_COMPACT_TEST_SYSTEM = pkgs.stdenv.hostPlatform.system;
    NIX_COMPACT_TEST_SLEEP = "${pkgs.coreutils}/bin/sleep";
    NIX_COMPACT_TEST_BUSYBOX = "${pkgs.pkgsStatic.busybox}/bin/busybox";
  };
}
