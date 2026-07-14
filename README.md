# LevelGen

Modern C++ build of Stefan Knorr's Level Generator. It turns a colour-coded PNG or GIF map into linked levels and a `.gmap`.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
cmake --install build --config Release --prefix out
```

The install bundle contains the executable, original documentation, and the example map.

## Command line

```text
LevelGen.exe --cli --image <map.png> --output <folder> --prefix <name> [--no-links]
```
