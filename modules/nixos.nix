{ packageForSystem }:
{ config, lib, pkgs, ... }:
let
  cfg = config.programs.nix-compact;
in {
  options.programs.nix-compact = {
    enable = lib.mkEnableOption "native compact Nix build output and private numbered logs";
    package = lib.mkOption {
      type = lib.types.package;
      default = packageForSystem pkgs.stdenv.hostPlatform.system;
      description = "Patched Nix package, independent of the consuming system's Nixpkgs version.";
    };
  };

  # @cc [owner:ghuntley,label:compatibility] explicit-nixos-selection
  # Enabling the module MUST default nix.package to the selected patched package;
  # an explicit nix.package override MUST retain ordinary NixOS precedence.
  config = lib.mkIf cfg.enable {
    nix.package = lib.mkDefault cfg.package;
  };
}
