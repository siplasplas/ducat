# ducat
Disk catalog

A fast disk usage analyzer in the spirit of `du`, `duc` and `ncdu` for Linux.
The core logic lives in the `libducat` library; the command-line tool and the
planned GUI are thin frontends over it.

Work in progress: the tool can scan a directory tree into memory and print
its totals; storing scans in a database is not implemented yet.

## Usage

```sh
ducat scan [-x] PATH
```

Scans the tree at `PATH` and prints its disk usage (`st_blocks * 512`), its
logical size (`st_size`), the number of files and directories, and the entries
of `PATH` sorted by disk usage. Symlinks are not followed (except `PATH`
itself), and a file with several hard links is counted once. Directory sizes
count their own size as well, as GNU `du` does. `-x` (`--one-file-system`)
stays on the file system of `PATH`.

Entries that cannot be read are reported on standard error and the scan goes
on; the exit status is then 1.

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

## Tests

Unit tests are built by default (`-DDUCAT_BUILD_TESTS=OFF` turns them off) and
take well under a second:

```sh
ctest --test-dir build --output-on-failure
```
