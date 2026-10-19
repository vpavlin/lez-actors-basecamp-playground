{
  description = "LEZ Actors: a Basecamp app to inspect and call LEZ v0.4 actor programs. Root flake = the v0.4 wallet/tx library (LEZ core_ffi).";

  # LEZ v0.4 (the actor model) lives on `dev`; there is no tag yet. Pinned to the commit the
  # wallet/signing spike passed on (2026-10-19). dev's own flake still builds the removed
  # `wallet-ffi` crate, so we build its replacement, `core_ffi`, here with LEZ's own inputs and
  # environment (circuits, rapidsnark, the pre-fetched risc0 recursion zkr).
  inputs = {
    lez.url = "github:logos-blockchain/logos-execution-zone/f6afdd285d274b3406b93877b16d85fd849c2df3";
    nixpkgs.follows = "lez/nixpkgs";
    # LEZ's own locked rust-overlay predates Rust 1.98.1, which LEZ pins (rust-toolchain.toml).
    rust-overlay = { url = "github:oxalica/rust-overlay"; inputs.nixpkgs.follows = "nixpkgs"; };
  };

  outputs = { self, lez, nixpkgs, rust-overlay, ... }:
    let
      inherit (lez.inputs) logos-nix crane logos-blockchain-circuits rust-rapidsnark;
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAll = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAll (system:
        let
          pkgs = import nixpkgs { inherit system; overlays = logos-nix.lib.nativeOverlays ++ [ rust-overlay.overlays.default ]; };
          craneLib = (crane.mkLib pkgs).overrideToolchain (pkgs.rust-bin.fromRustupToolchainFile "${lez}/rust-toolchain.toml");
          # patches/0001: an in-process local dev chain with its JSON-RPC (the wizard's "start a
          # local chain"): sequencer_ffi_local_setup/_start_local behind the `local-chain` feature
          src = pkgs.applyPatches { name = "lez-src-patched"; src = lez; patches = [ ./patches/0001-core-ffi-local-chain.patch ]; };
          cargoLock = builtins.fromTOML (builtins.readFile "${src}/Cargo.lock");
          r0rec = builtins.head (builtins.filter (p: p.name == "risc0-circuit-recursion") cargoLock.package);
          r0recCrate = pkgs.fetchurl {
            url = "https://static.crates.io/crates/risc0-circuit-recursion/${r0rec.version}/download";
            sha256 = r0rec.checksum;
            name = "risc0-circuit-recursion-${r0rec.version}.crate";
          };
          zkrHash = builtins.replaceStrings [ "\n" " " ] [ "" "" ] (builtins.readFile (pkgs.runCommand "r0-zkr-hash" { nativeBuildInputs = [ pkgs.gnutar ]; } ''
            tmp=$(mktemp -d); tar xf ${r0recCrate} -C "$tmp"
            grep -o '"[0-9a-f]\{64\}"' "$tmp/risc0-circuit-recursion-${r0rec.version}/build.rs" | head -1 | tr -d '"' | tr -d '\n' > $out
          ''));
          recursionZkr = pkgs.fetchurl { url = "https://risc0-artifacts.s3.us-west-2.amazonaws.com/zkr/${zkrHash}.zip"; sha256 = zkrHash; };
          coreFfi = craneLib.buildPackage {
            inherit src;
            pname = "lez-core-ffi";
            version = "0.1.0";
            cargoExtraArgs = "-p core_ffi --features local-chain";
            doCheck = false;
            buildInputs = [ pkgs.openssl pkgs.pcsclite ];
            nativeBuildInputs = [ pkgs.pkg-config pkgs.clang pkgs.llvmPackages.libclang.lib pkgs.gnutar pkgs.python3 ];
            LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
            LBC_ROOT_DIR = logos-blockchain-circuits.packages.${system}.default;
            RAPIDSNARK_LIB_DIR = rust-rapidsnark.packages.${system}.rapidsnark;
            RECURSION_SRC_PATH = "${recursionZkr}";
            preBuild = "export HOME=$(mktemp -d)";
            postInstall = ''
              mkdir -p $out/include
              cp lez/core_ffi/core_ffi.h $out/include/
              rm -f $out/lib/libcore_ffi.a
            '' + pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
              install_name_tool -id "$out/lib/libcore_ffi.dylib" $out/lib/libcore_ffi.dylib
            '';
          };
        in { core_ffi = coreFfi; default = coreFfi; });
    };
}
