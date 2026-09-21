{ pkgs }:
let
  source = pkgs.fetchFromGitHub {
    owner = "hegeldev";
    repo = "hegel-rust";
    rev = "9a130bfde99005504b0004428e047803ac18d3a0";
    hash = "sha256-Q69N6gRUAgTA/dKTb4XFM9Vd3cxuTvlemEj2zuGOWGk=";
  };
in
# Build the native C ABI directly: the Rust frontend's nested Cargo build is
# neither necessary for C++ properties nor appropriate inside the Nix sandbox.
pkgs.rustPlatform.buildRustPackage {
  pname = "nix-compact-hegel";
  version = "0.43.1-9a130bf";
  src = source;
  cargoLock.lockFile = "${source}/Cargo.lock";
  cargoBuildFlags = [ "-p" "hegeltest-c" ];
  cargoTestFlags = [ "-p" "hegeltest-c" ];
  postInstall = ''
    install -Dm644 hegel-c/include/hegel.h $out/include/hegel.h
  '';
}
