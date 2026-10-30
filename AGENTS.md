# Installing kindle-mtp-mount: instructions for AI coding agents

You're helping a person mount their Kindle as a Finder drive on their Mac.
Follow these steps in order. Some steps can only be done by the human. Stop and
ask them, and wait for them to confirm before you continue.

## What this is

A small macFUSE file system that shows a USB-connected Kindle Colorsoft (1st
gen, USB ID `1949:9981`) as `/Volumes/Kindle`. It talks to the Kindle over
MTP with libmtp 1.1.23. It's **read-write by default, and deletes on the
Kindle are permanent** (there's no Trash on the device).

## Rules for you, the agent

- **Never change System Settings, privacy (TCC) settings or security settings**,
  and never install system or kernel extensions. Those steps belong to the human.
- **Never tell the human to enable kernel extensions or lower security in
  Recovery Mode.** It isn't needed: this tool uses macFUSE's FSKit backend.
- **Don't use sudo.** `install.sh` doesn't need it.
- **Don't write, rename or delete anything on the Kindle** except inside a
  scratch folder you create for testing (for example `/Volumes/Kindle/documents/_mount_test`),
  and remove that folder afterwards. Never touch existing books, `/koreader`,
  `/system` or files at the Kindle's root. Use `kindle-mount --ro` when you
  only need to read.
- **Don't reset, unplug or "fix" the USB connection.** If the Kindle is busy,
  stop and tell the human (usually Calibre needs to be quit).
- Output masks the Kindle's serial number. Keep it that way when you report
  logs: don't print the raw log in other ways.

## Steps

1. **Check the Mac.** Run `sw_vers -productVersion` and `uname -m`. You need
   macOS 15.4 or later on `arm64`. Check for Homebrew with `command -v brew`.
   If it's missing, ask the human to install it from https://brew.sh.

2. **macFUSE (human step).** If `/Library/Filesystems/macfuse.fs` doesn't
   exist, ask the human to download macFUSE 5.x from https://macfuse.github.io/,
   run the installer and approve it when macOS asks. Don't install macFUSE for
   them.

3. **Get the code and run the installer.**
   ```sh
   git clone https://github.com/voantony/kindle-mtp-mount.git
   cd kindle-mtp-mount
   ./install.sh                  # or: ./install.sh --prefix /some/writable/bin
   ```
   It's safe to run more than once. It installs libmtp with Homebrew if needed,
   checks for libmtp 1.1.23 and macFUSE 5+, builds, runs the unit tests,
   installs `kindle-mount`, `kindle-unmount` and `kindle-mtpfs` to
   `~/.local/bin`, and does a dry check. If it says `~/.local/bin` isn't on
   PATH, tell the human the line to add to `~/.zshrc`, or call the tools by
   their full path.

4. **Turn on the file system extension (human step, once).** Ask the human to
   open **System Settings > General > Login Items & Extensions > File System
   Extensions** and switch on **macFUSE**. Don't do this for them. Don't rely
   on `pluginkit` to confirm it: it may not show the module as enabled even
   when it works. The mount in step 6 is the real test.

5. **Quit Calibre (human step).** Calibre, Android File Transfer and OpenMTP
   all grab the Kindle. Only one program can hold it at a time. Ask the
   human to quit them, then plug in the Kindle and unlock it.

6. **Verify, read-only first.**
   ```sh
   kindle-mount --ro
   ls /Volumes/Kindle            # expect documents, koreader, ...
   ls /Volumes/Kindle/documents | head
   kindle-unmount
   ```
   If `kindle-mount` prints "File system extension not enabled", step 4 isn't
   done yet. If it says the Kindle is busy, go back to step 5.

7. **Optional write test, in a scratch folder only.**
   ```sh
   kindle-mount
   T=/Volumes/Kindle/documents/_mount_test
   mkdir "$T" && echo hello > "$T/hello.txt" && cat "$T/hello.txt"
   mv "$T/hello.txt" "$T/renamed.txt" && rm "$T/renamed.txt" && rmdir "$T"
   kindle-unmount
   ```

8. **Hand over.** Tell the human:
   - `kindle-mount` mounts read-write and `kindle-mount --ro` read-only.
   - The drive is at `/Volumes/Kindle` and in Finder's sidebar.
   - Deletes there are permanent.
   - Eject it in Finder or run `kindle-unmount` before using Calibre again.
   - The log is at `~/Library/Logs/kindle-mount.log`.

## Mac app (optional)

If the human wants the menu-bar app, build it with `app/build-app.sh`. This
needs Xcode or the command line tools, and it writes `dist/Mount Kindle.app`.
Copy the app to `/Applications` only if the human agrees. You can drive it
with `open mountkindle://mount`, `open mountkindle://unmount` and
`open mountkindle://quit`, and its log is `~/Library/Logs/MountKindle.log`.
The same rules apply: don't touch System Settings, and the human does the
file system extension step.

## Uninstall

```sh
kindle-unmount 2>/dev/null; rm -f ~/.local/bin/kindle-mount ~/.local/bin/kindle-unmount ~/.local/bin/kindle-mtpfs
```
(Leave macFUSE and libmtp alone unless the human asks you to remove them.)
