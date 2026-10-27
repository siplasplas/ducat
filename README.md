# ducat
Disk catalog

A fast disk usage analyzer in the spirit of `du`, `duc` and `ncdu` for Linux.
The core logic lives in the `libducat` library; the command-line tool and the
planned GUI are thin frontends over it.

Work in progress: the tool can scan a directory tree, store it in a database
and list directories from it. Refreshing one branch of a stored tree is not
implemented yet.

## Usage

```sh
ducat scan [-x] [-d FILE] PATH
ducat ls [-d FILE] [PATH]
```

`scan` scans the tree at `PATH`, stores it in the database and prints its disk
usage (`st_blocks * 512`), its logical size (`st_size`), the number of files
and directories, and the entries of `PATH` sorted by disk usage. Symlinks are
not followed (except `PATH` itself), and a file with several hard links is
counted once. Directory sizes count their own size as well, as GNU `du` does.
`-x` (`--one-file-system`) stays on the file system of `PATH`. A new scan of
the same `PATH` replaces the stored one; the stored directories above `PATH`
keep their old totals for now.

Entries that cannot be read are reported on standard error and the scan goes
on; the exit status is then 1.

`ls` prints a stored directory the same way, without scanning; `PATH` defaults
to the current directory and must lie inside a scanned tree.

The database is `~/.cache/ducat/ducat.tkt` (under `$XDG_CACHE_HOME` when set);
`-d FILE` (`--database`) selects another file.

## Building

Requirements: CMake 3.20 or newer, a C++20 compiler and `make`.

```sh
cmake -S . -B build
cmake --build build
./build/cli/ducat --version
```

The database uses [Tkrzw](https://dbmx.net/tkrzw/). The first build downloads
Tkrzw 1.0.34 (checked by SHA-256) and builds it as a static library inside the
build directory, which needs network access once. To use the system library
instead, configure with `-DDUCAT_SYSTEM_TKRZW=ON`; configuration then checks
that it can remove records, which the Ubuntu package `libtkrzw-dev` 1.0.32
cannot.

Optional: Qt 6 (`sudo apt install qt6-base-dev`) for the planned GUI.

Pass `-DBUILD_SHARED_LIBS=ON` to build `libducat` as a shared library.

## Tests

Unit tests are built by default (`-DDUCAT_BUILD_TESTS=OFF` turns them off) and
take well under a second:

```sh
ctest --test-dir build --output-on-failure
```
