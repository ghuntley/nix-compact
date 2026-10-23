{
  description = "Native compact Nix build logs for humans and coding agents";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/da6dc0e7b26060f2f71ce3ef7088fb56ee44fa36";
    nixpkgs-dev.url = "github:NixOS/nixpkgs/02c087f3fe673c9e317f206a627cb6b910336dbe";
  };

  outputs = { self, nixpkgs, nixpkgs-dev }:
    let
      systems = import ./systems.nix;
      forAllSystems = nixpkgs.lib.genAttrs systems;
      packageForSystem = target:
        if builtins.elem target systems then self.packages.${target}.default
        else throw "nix-compact supports ${builtins.concatStringsSep ", " systems}; requested ${target}";
    in {
      lib = {
        supportedNixVersion = "2.34.9";
        supportedSystems = systems;
        mkPackage = import ./package;
        mkCoreCheck = import ./package/core-check.nix;
        mkNativeCheck = import ./package/native-check.nix;
      };

      packages = forAllSystems (system: {
        nix-compact = self.lib.mkPackage { pkgs = nixpkgs.legacyPackages.${system}; };
        default = self.packages.${system}.nix-compact;
      });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/nix";
          meta.description = "Run Nix with compact build output and full local log capture";
        };
      });

      nixosModules.default = import ./modules/nixos.nix { inherit packageForSystem; };
      devenvModules.default = { pkgs, lib, ... }: {
        packages = [ (lib.hiPrio (packageForSystem pkgs.stdenv.hostPlatform.system)) ];
      };

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          development = import ./dev-environment.nix { inherit pkgs; };
        in {
          default = pkgs.mkShell ({ packages = development.packages; } // development.env);
        });

      checks = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in {
          core = self.lib.mkCoreCheck { inherit pkgs; };
          native = self.lib.mkNativeCheck { inherit pkgs; package = self.packages.${system}.default; };
          native-development = self.lib.mkNativeCheck { pkgs = nixpkgs-dev.legacyPackages.${system}; };
        } // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          # NixOS module evaluation is platform-independent; one Linux system suffices.
          configuration = import ./checks/configuration.nix {
            inherit nixpkgs;
            module = self.nixosModules.default;
            package = self.packages.${system}.default;
          };
        });
    };
}
