{ pkgs }:
let
  hegel = import ./hegel.nix { inherit pkgs; };
in
pkgs.runCommand "nix-compact-core-check" {
  nativeBuildInputs = [ pkgs.stdenv.cc ];
  buildInputs = [ pkgs.nlohmann_json hegel ];
} ''
  c++ -std=c++23 -Wall -Wextra -Werror -pthread ${./classify-test.cc} -I${./.} -o classify-test
  ./classify-test
  c++ -std=c++23 -Wall -Wextra -Werror -pthread ${./archive-test.cc} -I${./.} -o archive-test
  ./archive-test "$TMPDIR/archive-fixture"
  c++ -std=c++23 -Wall -Wextra -Werror -pthread ${./property-test.cc} -I${./.} -lhegel_c -o property-test
  ./property-test "$TMPDIR/property-fixture"
  touch $out
''
