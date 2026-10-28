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
ducat trees [-d FILE]
ducat compact [-d FILE]
ducat find [-d FILE] [PATH] [CRITERIA] [--sort KEY] [--limit N]
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

A file with several hard links (common in Anaconda or Nix, for example) has
its data counted once, at the first link found. Listings still show each link
with the file's own size, marked `(=)` by `ls` and in italics by the GUI where
the data is counted at another link; such a link adds nothing to the totals
of its folder.

`ls` prints a stored directory the same way, without scanning; `PATH` defaults
to the current directory and must lie inside a scanned tree.

`trees` lists the stored trees, that is the scanned directories not lying
inside another stored tree, with their disk usage and size.

The database file does not reuse the space of replaced records by itself, so
`scan` (and the GUI) rebuilds it when the records take over twice the space
they need; for a scan of `/usr` this takes some 25 ms. `compact` rebuilds it
at once and prints the file size before and after.

`find` searches the stored entries below `PATH`, or in all stored trees,
without touching the disk. All given criteria must hold:

- `-t f` or `-t d`: files or directories only;
- `-e EXT[,EXT…]`: files with one of these extensions, in any case;
- `-n PATTERN`: names matching a shell pattern such as `'*.log'`, in any case;
- `--min-size SIZE`, `--max-size SIZE`: disk usage limits such as `500M` or
  `2G`; a directory counts with all its contents; `--apparent` compares the
  logical size instead;
- `--min-files N`, `--max-files N`: directories with that many files inside,
  at any depth;
- `--newer WHEN`, `--older WHEN`: modified at or after, or before, a local
  date `YYYY-MM-DD[ HH:MM]` or a period ago such as `30d`, `2w`, `6m` or `1y`.
  A directory's time is its own, not that of its contents.

Matches are sorted by size, largest first, or with `--sort files|time|path`;
`--limit N` prints only the first N. Subtrees whose totals rule out a match
are skipped, so looking for large directories or ones with many files reads
little of the database. Examples:

```sh
ducat find -t d --min-size 10G            # directories of 10 GiB or more
ducat find --min-files 100000 --sort files
ducat find ~ -e iso,img --min-size 1G
ducat find /var/log -n '*.log' --older 1y
```

The database is `~/.cache/ducat/ducat.tkt` (under `$XDG_CACHE_HOME` when set);
`-d FILE` (`--database`) selects another file.

### GUI

```sh
ducat-gui [-d FILE] [PATH]
```

Browses the stored directories: `PATH`, or the first stored directory. A bar
shows each entry's share of the directory's disk usage. Entries can be sorted
by any column, by default by disk usage. Enter or a double click
opens a folder, Alt+Up or Backspace goes to the parent, a path typed into the
bar opens that directory, and the Trees menu lists the stored trees to go to.

Find (Ctrl+F) searches the stored entries with the criteria of `ducat find`:
type, name pattern, extensions, size and number of files inside with lower and
upper limits, and modification time, below the shown directory or in all
stored trees. The matches are listed with their folder and can be sorted like
a directory; Enter or a double click shows the match in the main window. At
most 200,000 matches are listed.

Scan Folder (Ctrl+O) scans a chosen folder into the database, and Refresh (F5)
scans the shown directory again, which updates the directories above it as
`ducat scan` does. Scans run in the background, with the number of entries
read and, when refreshing, the progress against the stored count; the button
next to them stops a scan without changing the database. One file system,
on by default, keeps scans from descending into other file systems.

Database… (Ctrl+D) switches to another database file, or names a new one,
which the first Scan Folder creates; the window title shows the file. The GUI
starts with the database given by `-d`, else with the one chosen last if it
still exists, else with the default one.

The GUI opens the database only for the moment of each read or write, so
`ducat scan` can run while the GUI is open.

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

## License

ducat is free software under the GNU Lesser General Public License, version 3
or (at your option) any later version — see [COPYING.LESSER](COPYING.LESSER)
and [COPYING](COPYING), the GNU General Public License it builds on. Other
programs may so use the library `libducat`, for example its search.
