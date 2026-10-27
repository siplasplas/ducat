# ducat
Disk catalog

A fast disk usage analyzer in the spirit of `du`, `duc` and `ncdu` for Linux.
The core logic lives in the `libducat` library; the command-line tool and the
Qt GUI are thin frontends over it.

Work in progress: the tool can scan a directory tree, store it in a database,
refresh one branch of it and list directories from it.

## Usage

```sh
ducat scan [-x] [-j N] [-d FILE] PATH
ducat ls [-d FILE] [PATH]
```

`scan` scans the tree at `PATH`, stores it in the database and prints its disk
usage (`st_blocks * 512`), its logical size (`st_size`), the number of files
and directories, and the entries of `PATH` sorted by disk usage. Symlinks are
not followed (except `PATH` itself), and a file with several hard links is
counted once. Directory sizes count their own size as well, as GNU `du` does.
`-x` (`--one-file-system`) stays on the file system of `PATH`. The scan runs
on several threads, by default twice the number of logical cores but at most
32; `-j N` (`--threads`) sets the number.

A new scan of a stored `PATH` replaces the stored data. When `PATH` lies inside
a stored tree, only that branch is scanned: its entry in the directory above
and the totals of all stored directories above are updated. A `PATH` that no
longer exists is removed from the database the same way. Hard links are then
counted once within the rescanned branch only.

Entries that cannot be read are reported on standard error and the scan goes
on; the exit status is then 1.

`ls` prints a stored directory the same way, without scanning; `PATH` defaults
to the current directory and must lie inside a scanned tree.

The database is `~/.cache/ducat/ducat.tkt` (under `$XDG_CACHE_HOME` when set);
`-d FILE` (`--database`) selects another file.

### GUI

```sh
ducat-gui [-d FILE] [PATH]
```

Browses the stored directories: `PATH`, or the first stored directory. Entries
can be sorted by any column, by default by disk usage. Enter or a double click
opens a folder, Alt+Up or Backspace goes to the parent, F5 reads the directory
from the database again, and a path typed into the bar opens that directory.
The GUI does not scan yet; use `ducat scan`. It opens the database only for the
moment of each read, so `ducat scan` can run while the GUI is open.

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

The GUI `ducat-gui` (in `build/gui`) is built when Qt 6 is found
(`sudo apt install qt6-base-dev`); `-DDUCAT_BUILD_GUI=OFF` skips it.

Pass `-DBUILD_SHARED_LIBS=ON` to build `libducat` as a shared library.

## Tests

Unit tests are built by default (`-DDUCAT_BUILD_TESTS=OFF` turns them off) and
take well under a second:

```sh
ctest --test-dir build --output-on-failure
```
