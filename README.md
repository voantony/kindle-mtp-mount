# kindle-mtp-mount

Shows a Kindle that's plugged into a Mac by USB as a normal drive called
**Kindle** in Finder. You can browse it, copy books off it, and drag files
onto it.

Newer Kindles (for example the Kindle Colorsoft) connect over MTP, the same
phone-style protocol Android uses. macOS doesn't show MTP devices in Finder.
This small program bridges the gap. It talks MTP with
[libmtp](https://github.com/libmtp/libmtp) and presents the files to macOS
through [macFUSE](https://macfuse.github.io/).

**Read-write.** Copying, renaming, creating folders and deleting all work.
**Deletes are immediate and permanent.** There's no Trash on the Kindle, so
anything you delete there is gone.

Built for a Kindle Colorsoft (1st gen, USB ID `1949:9981`) on macOS 27 with
macFUSE 5.5 (FSKit backend) and libmtp 1.1.23. The device connection code has
been tested on that Kindle. The mounted drive itself has so far only been
tested with the unit tests (`make test`), because macOS's file system
extension switch hadn't been turned on yet.

## Requirements

- macOS 15.4 or later, on Apple silicon
- [macFUSE](https://macfuse.github.io/) 5.x. Turn on its file system extension
  once in **System Settings → General → Login Items & Extensions → File System
  Extensions → macFUSE**. No kernel extension or Recovery Mode change is needed,
  because the mount uses macFUSE's FSKit backend.
- libmtp 1.1.23 from Homebrew: `brew install libmtp`

## Build and install

```sh
make            # builds ./kindle-mtpfs
make test       # optional: unit tests against a fake in-memory device
make install    # copies kindle-mtpfs, kindle-mount, kindle-unmount to ~/kindle-stage/tools
                # (change with: make install PREFIX=/some/dir)
```

## Use

1. **Quit Calibre first** (and Android File Transfer, OpenMTP, or anything else
   that talks to the Kindle). Only one program can hold the Kindle at a time.
2. Plug in the Kindle and unlock it.
3. Run:

```sh
kindle-mount           # read-write
kindle-mount --ro      # read-only: every change is refused
kindle-unmount         # unmount and give the Kindle back to Calibre etc.
```

The drive appears at `/Volumes/Kindle` and in Finder's sidebar. Ejecting it
in Finder works too. Logs go to `~/Library/Logs/kindle-mount.log`.

If the Kindle is busy, `kindle-mount` says so and stops. It never retries in a
loop and never resets the USB connection.

## How it works

- Folder listings are read lazily and cached for 5 seconds.
- Small files are downloaded to a temp file the first time they're read. For
  big files (over 2 MB), the first couple of reads use MTP partial reads, so
  Finder previews don't pull the whole book. A full download happens only if a
  program keeps reading.
- New and changed files are uploaded when the file is closed, and the size on
  the Kindle is checked against the local copy. Replacing an existing file
  uploads `name.kmtmp` first, checks it, then deletes the old file and renames
  the new one into place, so a failed upload never loses the old copy.
- macOS clutter (`.DS_Store`, `._*` AppleDouble files, `.Trashes`,
  `.fseventsd`, `.Spotlight-V100`, …) is kept in memory only and never written
  to the Kindle. Extended attributes are accepted and dropped.
- The Kindle serial number is masked in everything the tool prints.
- On unmount, the Kindle is released without a USB reset.

## Limitations

- Only the Kindle Colorsoft (1st gen) USB ID is accepted. The code also relies
  on libmtp 1.1.23 internals to stop libmtp resetting USB on close, and checks
  that version at startup.
- Moving a file between folders uses MTP MoveObject. If the Kindle refuses
  that, the move fails with "cross-device link". Copy and delete instead.
- Timestamps and permissions can't be changed (MTP doesn't store them). Such
  requests succeed but do nothing.
- Files written from the Mac are held in a temp file until closed, so a very
  large copy needs that much free space on the Mac.
- Names typed in Finder with accents are stored in macOS's decomposed Unicode
  form.
- One request at a time. MTP is slow, so big copies take a while.

## License

MIT. See [LICENSE](LICENSE). This project links dynamically against libmtp
(LGPL-2.1) and macFUSE's libfuse. No code from other MTP filesystems was used.
