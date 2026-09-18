# SQLite, vendored for Windows only

`fsainotecache.cpp` caches notecard text in SQLite. **macOS ships libsqlite3 in
the SDK and Linux has it everywhere**, so on those two it is one linker flag and
these files are not compiled at all. Windows ships nothing, so the include fails
and the build stops — which is the first failure the Windows port was expected
to hit.

This is SQLite's official **amalgamation**: the whole library as one `.c` and one
`.h`, no build system and no dependency for anyone to install. That is the same
reasoning the ChatGPT bridge is `sh` rather than Node — nothing a person has to
go and fetch first.

    version   3.50.4 (2025-07-30)
    source    https://sqlite.org/2025/sqlite-amalgamation-3500400.zip
    sha256    1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    licence   public domain (https://sqlite.org/copyright.html)

`shell.c` and `sqlite3ext.h` from that archive are deliberately **not** here:
the first is the command-line tool, the second is for loadable extensions, and
neither is wanted inside a viewer.

These are added files, not modified upstream ones, so they cost nothing against
a Firestorm merge.

To update: download the new amalgamation, replace both files, and record the
version and checksum above. Nothing else changes.
