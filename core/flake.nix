{
  description = "LEZ Actors core module: inspect and call LEZ v0.4 actor programs (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # The repo-root flake builds core_ffi (LEZ dev's v0.4 wallet library) from a pinned LEZ commit.
    lez_actors.url = "path:..";
  };

  outputs = inputs@{ logos-module-builder, lez_actors, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        core_ffi = { input = lez_actors; packages.default = "core_ffi"; };
      };
    };
}
