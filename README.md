# ducat
Disk catalog

A fast disk usage analyzer in the spirit of `du`, `duc` and `ncdu` for Linux.
The core logic lives in the `libducat` library; the command-line tool and the
Qt GUI are thin frontends over it.

Work in progress: the tool can scan a directory tree, store it in a database,
refresh one branch of it and list directories from it.

## Usage

```sh
ducat scan [-x] [-j N] [-a] [-d FILE] PATH
ducat ls [-d FILE] [PATH]
ducat trees [-d FILE]
ducat compact [-d FILE]
ducat strip [-d FILE] [PATH]
ducat find [-d FILE]... [PATH]... [CRITERIA] [--sort KEY] [--limit N]
ducat dups [-d FILE]... [PATH]... [--time] [--min-size SIZE] [--limit N]
ducat dups --dirs [-d FILE]... [PATH]... [--threshold PERCENT] [--names] [--min-files N] [--limit N]
```

`scan` scans the tree at `PATH`, stores it in the database and prints its disk
usage (`st_blocks * 512`), its logical size (`st_size`), the number of files
and directories, and the entries of `PATH` sorted by disk usage. Symlinks are
not followed (except `PATH` itself), and a file with several hard links is
counted once. Directory sizes count their own size as well, as GNU `du` does.
`-x` (`--one-file-system`) stays on the file system of `PATH`. The scan runs
on several threads, by default twice the number of logical cores but at most
32; `-j N` (`--threads`) sets the number.

Scans write each directory, once it and all below it are read, to a
temporary file next to the database, so that a large tree need not fit in
memory (a scan of `/usr` peaks at 80 MiB instead of 200 MiB); the database
itself is written at the end, in one go, from that file. Archives (`-a`) are
read as their directories complete. The temporary file has no name and goes
away with the scan.

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
they need; for a scan of `/usr` this takes some 0.1 s. `compact` rebuilds it
at once and prints the file size before and after; it also compresses a
database made before ducat compressed them.

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

`dups` lists probable duplicates among the stored entries, found by names and
sizes only, without reading the files:

- Files with the same name and logical size, with `--time` also the same
  modification time, in groups sorted by the space taken by the copies beyond
  the first. Files smaller than `--min-size` (1 byte) are left out, and so are
  hard links counted elsewhere, which take no space of their own.
- With `--dirs`, pairs of directories sharing at least `--threshold` percent
  (80) of the files below the directory with more files. Files are compared
  by name and size, or with `--names` by name only, so that they may differ
  in size. Pairs start from directories similar by the files directly in
  them, and go up through their parents as long as the two have the same
  name: copies of `A/gitmy/GAG/.git` and `B/gitmy/GAG/.git` lead to `GAG`
  with `GAG` and `gitmy` with `gitmy`. Two directories share the files
  directly in both and, recursively, those of their subdirectories of the
  same names, so a subfolder weighs as much as the files in it, and a
  repository never pairs with its own `.git`. Directories with fewer than
  `--min-files` files (5) are left out, a directory is never paired with one
  inside it, and only the topmost pairs are listed: `A/x` with `B/x` is left
  out when `A` and `B` pair, unless `--nested` (in the GUI, Also pairs inside
  listed pairs) lists them too. Pairs are sorted by the disk usage of the
  smaller directory.

On `/usr` (870,000 entries) either takes about 0.4 s and some 100 MB of
memory.

`find` and `dups` work on several databases and paths at once: `-d` and
`PATH` may be repeated, and each `PATH` is looked for in each database. A
`PATH` that is not stored itself but holds stored trees, such as the mount
directory of several scanned drives, stands for those trees. With more than
one database, paths are shown as `DATABASE-FILE:PATH`. For example, with the
home directory in one database and removable drives in another:

```sh
ducat dups -d ~/home.ducat -d ~/drives.ducat          # also across the two
ducat dups --dirs -d ~/drives.ducat /run/media/$USER  # only among the drives
```

### Archives

Files whose names show an archive (zip, 7z, rar, tar and compressed tar, cpio,
iso, cab, deb, rpm) are marked as such. A database can hold the contents of an
archive under its path, like a folder: `…/photos.zip/2019/img.jpg`. These
contents take no space of their own, so they are not in the totals of the
folders above.

`ducat scan -a` (`--archives`) stores the contents of the archives in the
scanned tree, read with libarchive (`libarchive-dev`; the build leaves the
option out without it, `-DDUCAT_ARCHIVES=OFF` too). Scanning archives is
slower, so it is meant for the trees where it matters. A later scan, with or
without `-a`, keeps the stored contents of archives whose size and time did not
change. Archives inside archives are listed as plain files. An unreadable
archive is reported and the scan goes on. Directories missing from an
archive's list are made up from the paths; an entry's disk usage is its size
rounded up to 512 bytes.

`ducat find -a` and, in the GUI, Find with Also inside stored archives look
into stored contents. In the GUI, Enter on an archive opens its stored
contents, or else, with the archive plugin, reads the archive on disk and
shows its contents without storing them; Refresh (Ctrl+R) there stores them.
Options > Read Archives When Scanning makes the GUI's scans store archives as
`scan -a` does. The database format "ducat 2" for this reads older databases
and marks them as format 2 when they are written.

Stored contents can be removed again to make the database smaller, the
archives staying as files: `ducat strip [PATH]` removes them below `PATH`, or
in the whole database, and compacts it; in the GUI, File > Remove Archive
Contents Here does so below the shown folder.

### Databases

The default database is `~/.local/share/siplasplas/ducat/drives.ducat` (under
`$XDG_DATA_HOME` when set). `-d FILE` (`--database`), placed after the command,
selects another file, by convention with the extension `.ducat`. A leading `~`
stands for the home directory, and a directory, given with a trailing `/` or
existing as one, stands for `drives.ducat` in it.

A database records the format it was written in. ducat refuses, without
changing it, a file that is not a ducat database or comes from a version with
another format; such drives have to be scanned into a new database. Databases
of versions before the `.ducat` files, in `~/.cache/ducat`, are not read.

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
upper limits, and modification time. The matches are listed with their folder,
and their database when several are searched, and can be sorted like a
directory; Enter or a double click shows the match in the main window. At
most 200,000 matches are listed.

The matches can be worked on as files, when they are on disk now. Click,
Shift or Ctrl with a click, or Ctrl+A for all, selects them; then Copy (F5)
and Move (F6) put them into one folder, made when missing, and Move to Trash
(F8) moves them to the trash, e.g. all `cmake-build-*` folders found. A match
inside another selected folder goes with that folder. Matches of the same
name go into subfolders named after the folders holding them:
`abc/a/name1` and `def/b/name1` become `DIR/a/name1` and `DIR/b/name1`, and
when those names repeat too, `DIR/a_1/name1` and `DIR/a_2/name1`. Each
database searched, and the shown one, is updated where it stores the folders
concerned. Matches moved or trashed leave the list, with those inside them;
copied ones stay. F4 edits a match, updating its database on saving.

Both Find and Duplicates look in the trees checked under Look in: the stored
trees of the shown database and of further databases, e.g. one of the home
directory and one of removable drives, which Add Database… adds and Remove
takes away. The database shown before another one was opened stays among
them. Unchecked trees and the further databases are remembered; Only the
shown folder looks below the folder shown in the main window instead. A match
in another database opens that database in the main window.

Scan Folder (Ctrl+O) scans a chosen folder into the database, and Refresh
(Ctrl+R) scans the shown directory again, which updates the directories above it as
`ducat scan` does. Scans run in the background, with the number of entries
read and, when refreshing, the progress against the stored count; the button
next to them stops a scan without changing the database. Options > One File
System, on by default, keeps scans from descending into other file systems.
All actions are in the menus File, Go, Tools and Options; the toolbar holds
the frequent ones.

In every list, of a folder, of search matches or of duplicates, the columns
can be widened and narrowed with the mouse; until one is, they fit their
contents, and Fit Columns in the header's context menu makes them fit again.
The list's context menu has Copy (Ctrl+C), which copies the selected rows as
tab-separated values of the shown columns, e.g. for a spreadsheet, and Copy
with Column Names, which adds the column names as the first line.

File operations change the disk and keep the database up to date, without
scanning the whole folder again. Copy (F5) and Move (F6) ask where to put the
selected entries (Ctrl or Shift with a click selects several), offering the
folder used last: into an existing folder, for a single entry also to a new
path, which renames it, and for several into a new folder, which is created.
An entry already at the target is overwritten or skipped as answered, once or
for all; overwriting a folder merges the folders. The work runs with progress
and can be stopped; copies keep permissions and modification times, and a
move across file systems copies and then removes only what was copied whole.
The database is updated for the sources of a move and for the targets, where
their folders are stored: moving from a scanned USB drive to an unscanned
home folder removes the entries from the drive's tree only. New Folder (F7)
creates a folder in the shown one, and Move to Trash (F8) moves the selected
entries to the desktop's trash, never deleting them for good. All need the shown folder on disk, that is its drive mounted, and work
neither inside archives nor during a scan. The library function
`ducat::update_entry` brings one changed path up to date in a database, when
the folder holding it is stored there.

Duplicates… (Ctrl+Shift+D) finds probable duplicates among the stored entries
as `ducat dups` does, also across drives and databases: files with the same
name and size, listed in groups with
their copies, and folders sharing most of their files, listed in pairs that
can be sorted by any column. Enter or a double click shows a copy, or the
folder of the clicked half of a pair, in the main window; F3 views a copy. At
most 10,000 groups or pairs are listed, the largest first.

Copy (F5), Move (F6) and Move to Trash (F8) work there as among search
matches, on the selected copies of files, or on one folder of each selected
pair: the one on the side of the column holding the cursor. Trashing every
copy of a file is asked about once more. Copies moved or trashed leave the
list, and so do groups left with a single copy and pairs with a folder gone.

View (F3), in the main window and among search matches, shows the selected
file when it is on disk now, that is when its drive is mounted, read-only:
files up to 5 MiB (the setting `viewer/small_file_mib`, at most a tenth of
the memory) in an editor with syntax colours and encoding detection, larger
ones in a viewer of the memory-mapped file. Keys 1 and 3 switch between text
and hex, as in Total Commander's lister; Ctrl+F, F3 and Shift+F3 search; Escape
closes the viewer.

Edit (F4), in the main window and among search matches, opens the selected
file, when it is on disk now, in qceditor's editor window from the same
plugin: tabs, search and replace, encodings, syntax colours and spelling.
Each save updates the file's entry in the shown database, when its folder is
stored there. Closing ducat asks about unsaved files first.

Tools > Open in gemini-commander (Ctrl+Shift+G) opens the folder of the
selected entry in [gemini-commander](https://github.com/siplasplas/gemini-commander),
with the entry selected, when it is on disk now; an entry inside an archive
stands for the archive. A running gemini-commander gets it over D-Bus, else it
is started; the setting `gemini/command` names another program. On Wayland an
xdg-activation token, which Qt gives only through its private API
(`qt6-base-private-dev` at build time), lets its window come to the front.

Database… (Ctrl+D) switches to another database file, or names a new one,
which the first Scan Folder creates; the window title shows the file. The GUI
starts with the database given by `-d`, else with the one chosen last if it
is still a ducat database, else with the default one. Its settings are kept in
`~/.config/siplasplas/ducat.conf`.

The GUI opens the database only for the moment of each read or write, so
`ducat scan` can run while the GUI is open.

## Building

Requirements: CMake 3.20 or newer, a C++20 compiler and `make`.

```sh
cmake -S . -B build
cmake --build build
./build/cli/ducat --version
```

`ducat --version`, `ducat-gui --version` and Help > About in the GUI name
the Git commit built and its time, with `+changes` when the tracked files
differed from it, e.g. `0.1.0 (commit a03c85d, 2026-10-28 19:45)`; every
build checks the commit again. About also shows where the program runs
from, so an installed copy is told apart from one in the build tree.

The database uses [Tkrzw](https://dbmx.net/tkrzw/). The first build downloads
Tkrzw 1.0.34 (checked by SHA-256) and builds it as a static library inside the
build directory, which needs network access once. To use the system library
instead, configure with `-DDUCAT_SYSTEM_TKRZW=ON`; configuration then checks
that it can remove records, which the Ubuntu package `libtkrzw-dev` 1.0.32
cannot.

The database is compressed with Zstandard (`sudo apt install libzstd-dev`),
which makes it some 2.4 times smaller (14 MiB instead of 33 MiB for `/usr`)
and costs a few hundredths of a second for scanning or searching it. Without
libzstd the build warns and makes uncompressed databases; such a build cannot
read compressed ones.

The GUI `ducat-gui` (in `build/gui`) is built when Qt 6 is found
(`sudo apt install qt6-base-dev`); `-DDUCAT_BUILD_GUI=OFF` skips it.

### Plugins

Features with further dependencies come as plugins of the GUI, so that
`ducat-gui` itself depends only on Qt and works without them. Configuring
fetches their dependencies from GitHub, which needs network access once;
`-DDUCAT_BUILD_PLUGINS=OFF` skips the plugins. The plugins are:

- `ducat-filedialog`: the file dialogs of
  [qt-extra](https://github.com/siplasplas/qt-extra) (`QxFileDialog`) for Scan
  Folder and Database; without it, Qt's own dialogs are used.
- `ducat-viewer`: the F3 viewer and the F4 editor, as in
  [gemini-commander](https://github.com/siplasplas/gemini-commander), with
  [qceditor](https://github.com/siplasplas/qceditor) and
  [qcodeedit](https://github.com/siplasplas/qcodeedit) for small files and
  [textviewers](https://github.com/ideorg/textviewers) for large ones. It needs
  `sudo apt install libicu-dev libhunspell-dev libbotan-2-dev`. Kate syntax
  definitions are read from, and downloaded to, `~/.local/share/kate-data`,
  shared with the other programs using qcodeedit. Without the plugin, F3 does
  nothing but say so.
- `ducat-archive-plugin`: reading archives with libarchive, built when
  libarchive is found: Enter on an archive on disk and scans with archives.

`ducat-gui` loads plugins from the directories in `$DUCAT_PLUGIN_PATH`
(separated by `:`), from `lib/ducat/plugins` of its installation and, when run
from the build directory, from `build/plugins`. `ducat-gui --plugins` lists the
directories and what was loaded, or why not.

Pass `-DBUILD_SHARED_LIBS=ON` to build `libducat` as a shared library.

## Installing

```sh
sudo cmake --install build                  # to /usr/local
cmake --install build --prefix ~/.local     # for the current user only
```

This installs `ducat` and `ducat-gui` to `bin`, and with the GUI also
`share/applications/ducat.desktop` and the icon
`share/icons/hicolor/scalable/apps/ducat.svg`, so ducat appears in the menu of
the desktop. Plugins go to `lib/ducat/plugins`, the encoding detection data of
the viewer to `share/ducat/cpg`, and a shared `libducat` to `lib`, where the
installed programs find it. `--strip` makes the plugins much smaller. Remove the files listed in `build/install_manifest.txt` to uninstall.

Install again after every build you want to use from the menu or from
`ducat-gui` on the `PATH`: the installed files are copies, and the build tree
does not change them (running `build/gui/ducat-gui` directly needs no install).

The install also refreshes the icon cache (`gtk-update-icon-cache`) and the
desktop file database (`update-desktop-database`) of the prefix, so the icon
shows without logging out; this is skipped when `DESTDIR` is set, for packages.
If the icon is still missing, restart the program, or run these by hand:

```sh
sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
sudo update-desktop-database /usr/local/share/applications
```

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
