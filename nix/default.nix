{
  assimp,
  boost,
  c-blosc,
  clangStdenv,
  cmake,
  fmt,
  glfw,
  gtk3,
  hdf5,
  fetchFromGitHub,
  imath,
  lib,
  lz4,
  ninja,
  openimageio,
  opensubdiv,
  openvdb,
  pkg-config,
  pugixml,
  python3,
  spirv-cross,
  tbb,
  vulkan-loader,
  vulkan-tools,
  zlib,
}:
let
  stdenv = clangStdenv;

  openvdb_9 = openvdb.overrideAttrs (old: rec {
    name = "${old.pname}-${version}";
    version = "9.1.0";
    src = fetchFromGitHub {
      owner = "AcademySoftwareFoundation";
      repo = "openvdb";
      tag = "v${version}";
      hash = "sha256-OP1xCR1YW60125mhhrW5+8/4uk+EBGIeoWGEU9OiIGY=";
    };
    meta = old.meta // {
      license = lib.licenses.mpl20;
    };
  });
in
stdenv.mkDerivation {
  pname = "falcor";
  version = "8.0";

  src = ../.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    python3
    spirv-cross
  ];

  buildInputs = [
    assimp
    boost
    c-blosc
    fmt
    glfw
    gtk3
    hdf5
    imath
    lz4
    openimageio
    opensubdiv
    openvdb_9
    pugixml
    python3
    tbb
    vulkan-loader
    vulkan-tools
    zlib
  ];

  cmakeFlags = [
    "-DFALCOR_USE_SYSTEM_PYTHON=ON"
    "-DFALCOR_ENABLE_USD=OFF"
  ];

  meta = with lib; {
    description = "Falcor Realtime Rendering Framework";
    homepage = "https://github.com/NVIDIAGameWorks/Falcor";
    license = licenses.bsd3;
    platforms = [
      "x86_64-linux"
    ];
  };
}
