{
  description = "LEZ Actors desktop view: pure-QML over lez_actors_module (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    lez_actors_module.url = "path:../core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
