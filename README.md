# ducat
Disk catalog

A fast disk usage analyzer in the spirit of `du`, `duc` and `ncdu` for Linux.
The core logic lives in the `libducat` library; the command-line tool and the
planned GUI are thin frontends over it.

Work in progress: the command-line tool currently only supports `--help` and
`--version`.

## Building

Requirements: CMake 3.20 or newer and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
./build/cli/ducat --version
```

Optional dependencies, needed by later stages (the database and the GUI):

```sh
sudo apt install libtkrzw-dev qt6-base-dev
```

Pass `-DBUILD_SHARED_LIBS=ON` to build `libducat` as a shared library.
