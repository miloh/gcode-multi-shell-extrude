{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs;
    [
      tio
      openscad-unstable
      prusa-slicer
    ];
}
