{ pkgs, ... }:
let
  development = import ./dev-environment.nix { inherit pkgs; };
in {
  inherit (development) packages env;
  enterTest = ''
    nix build --no-link .#checks.x86_64-linux.core .#checks.x86_64-linux.configuration
  '';
}
