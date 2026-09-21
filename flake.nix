{
  description = "Native compact Nix build logs for humans and coding agents";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/c3eea5b2156db11c7eeeada3dc737711255b253e";
    nixpkgs-dev.url = "github:cachix/devenv-nixpkgs/256551e45f6303e142ab4a98be1bf243feb77dc0";
    nixpkgs-dev.inputs.nixpkgs-src = {
      url = "github:NixOS/nixpkgs/c8f90650c15282fa8656a041bfbbd2403997a9a7";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, nixpkgs-dev }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      developmentPkgs = nixpkgs-dev.legacyPackages.${system};
      packageForSystem = target:
        if target == system then self.packages.${system}.default
        else throw "nix-compact currently supports x86_64-linux; requested ${target}";
      development = import ./dev-environment.nix { inherit pkgs; };
    in {
      lib = {
        supportedNixVersion = "2.34.8";
        supportedSystems = [ system ];
        mkPackage = import ./package;
        mkCoreCheck = import ./package/core-check.nix;
        mkNativeCheck = import ./package/native-check.nix;
      };

      packages.${system} = {
        nix-compact = self.lib.mkPackage { inherit pkgs; };
        default = self.packages.${system}.nix-compact;
      };

      apps.${system}.default = {
        type = "app";
        program = "${self.packages.${system}.default}/bin/nix";
        meta.description = "Run Nix with compact build output and full local log capture";
      };

      nixosModules.default = import ./modules/nixos.nix { inherit packageForSystem; };
      devenvModules.default = { pkgs, lib, ... }: {
        packages = [ (lib.hiPrio (packageForSystem pkgs.stdenv.hostPlatform.system)) ];
      };

      devShells.${system}.default = pkgs.mkShell ({
        packages = development.packages;
      } // development.env);

      checks.${system} = {
        core = self.lib.mkCoreCheck { inherit pkgs; };
        native = self.lib.mkNativeCheck { inherit pkgs; package = self.packages.${system}.default; };
        native-development = self.lib.mkNativeCheck { pkgs = developmentPkgs; };
        configuration = import ./checks/configuration.nix {
          inherit nixpkgs;
          module = self.nixosModules.default;
          package = self.packages.${system}.default;
        };
      };
    };
}
