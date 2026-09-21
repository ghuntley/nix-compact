{ pkgs, package ? import ./. { inherit pkgs; } }:
let
  hegel = import ./hegel.nix { inherit pkgs; };
in pkgs.runCommand "nix-compact-native-check" {
  nativeBuildInputs = [ pkgs.stdenv.cc ];
  buildInputs = [ pkgs.nlohmann_json hegel ];
  NIX_COMPACT_TEST_BINARY = "${package.nix-cli}/bin/nix";
  NIX_COMPACT_TEST_BASH = "${pkgs.bash}/bin/bash";
  NIX_COMPACT_TEST_SYSTEM = pkgs.stdenv.hostPlatform.system;
  NIX_COMPACT_TEST_SLEEP = "${pkgs.coreutils}/bin/sleep";
  NIX_COMPACT_TEST_BUSYBOX = "${pkgs.pkgsStatic.busybox}/bin/busybox";
} ''
  c++ -std=c++23 -Wall -Wextra -Werror -pthread ${./property-test.cc} -I${./.} -lhegel_c -o properties
  ./properties "$TMPDIR/native-properties"
  touch $out
''
