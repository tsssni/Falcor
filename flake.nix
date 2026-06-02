{
  description = "falcor devenv";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    tsssni = {
      url = "github:tsssni/tsssni.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    nixgl = {
      url = "github:tsssni/nixGL";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      nixpkgs,
      tsssni,
      nixgl,
      ...
    }:
    let
      lib = nixpkgs.lib;

      systems = [
        "x86_64-linux"
      ];

      systemAttrs = f: system: { ${system} = f system; };

      mapSystems = f: systems |> lib.map (systemAttrs f) |> lib.mergeAttrsList;

      mapPkgs =
        system:
        import nixpkgs {
          inherit system;
          overlays = tsssni.pkgs;
          config.allowUnfree = true;
        };

      packages = mapSystems (
        system:
        let
          pkgs = mapPkgs system;
        in
        {
          default = pkgs.callPackage ./nix { };
        }
      );

      devShells = mapSystems (
        system:
        let
          pkgs = mapPkgs system;
          glpkgs =
            let
              isx86 = system == "x86_64-linux";
            in
            import nixgl {
              inherit pkgs;
              enable32bits = isx86;
              enableIntelX86Extensions = isx86;
            };
        in
        rec {
          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            inputsFrom = [ packages.${system}.default ];
            packages = with pkgs; [
              clang-tools
              cmake-language-server
              shader-slang
              ty
              vulkan-validation-layers
              (python3.withPackages (ps: with ps; [ tqdm ]))
            ];
            shellHook = ''
              for nvtt_so in external/nvtt/libnvtt.so.*; do
                [ -e "$nvtt_so" ] || continue
                ${pkgs.patchelf}/bin/patchelf --set-rpath ${
                  lib.makeLibraryPath [ pkgs.stdenv.cc.cc.lib ]
                } "$nvtt_so" 2>/dev/null || true
                ln -sf "$(basename "$nvtt_so")" external/nvtt/libnvtt.so 2>/dev/null || true
              done
              export CMAKE_INSTALL_PREFIX=$HOME/metatron/out
              export VK_LAYER_PATH=${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d
              export LD_LIBRARY_PATH=${pkgs.vulkan-loader}/lib:$LD_LIBRARY_PATH
              export XDG_DATA_DIRS=${pkgs.gtk3}/share/gsettings-schemas/${pkgs.gtk3.name}:${pkgs.glib}/share/gsettings-schemas/${pkgs.glib.name}:$XDG_DATA_DIRS
              export SHELL=nu
            '';
          };

          impure = default.overrideAttrs (oldAttrs: {
            nativeBuildInputs =
              oldAttrs.nativeBuildInputs
              ++ (with glpkgs; [
                nixVulkanIntel
                auto.nixVulkanNvidia
              ]);
          });
        }
      );
    in
    {
      inherit packages devShells;
    };
}
