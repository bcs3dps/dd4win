# dd4win -- raw block copy for Windows disks and files

`dd4win` is a small, standalone, dependency-free command-line tool that copies
raw blocks between Windows physical disks and files, with `dd`-style arguments.
It images a whole raw disk to a file, carves a sector range out, and writes a
range back -- with no GUI and no third-party tool. Reading (imaging / backup)
is the primary, safe path; writing to a physical device is guarded behind
explicit flags, because a mistyped target is the single biggest footgun of a
tool like this.

It is pure Win32 (kernel32 / the UCRT) and builds to a single static `.exe`.
The job it was written for is an embedded device whose eMMC is exposed over
USB as a disk: image it, edit the image (`ext4edit`), write it back. The full
reference, with the recovery workflow, is [`MANUAL.md`](MANUAL.md).

## Requirements

| Need | Detail |
|---|---|
| Windows | Windows 7 or later. |
| Privilege | **Administrator** to READ or WRITE a `\\.\PhysicalDriveN` or `\\.\X:` device. `--list` and a plain file-to-file copy need no elevation; neither does a refused write, which is decided before the device is opened for access. |
| Dependencies | **None.** A static single `.exe` importing only `KERNEL32` and the UCRT. |
| Shell | Run it from `cmd.exe`, PowerShell or a `.bat`, so device paths survive (see "Devices and paths"). |

## Usage

```
dd4win --list
dd4win if=<src> of=<dst> [bs=N] [count=N] [skip=N] [seek=N]
       [status=progress] [conv=notrunc] [--yes] [--force]
dd4win --help
dd4win --version
```

Arguments come in two forms, `dd`-style `key=value` tokens and `--flags`, in
any order. A copy needs both `if=` and `of=`. Any other token is an error:
the tool prints its usage and exits non-zero rather than guessing, and so does
a bare run.

## Options

| Argument | Meaning |
|---|---|
| `if=<src>` | Input: a file path, or a device `\\.\PhysicalDriveN` / `\\.\X:`. Required for a copy. |
| `of=<dst>` | Output: a file path, or a device. Required for a copy. |
| `bs=N` | Block size in bytes; accepts a binary suffix `k`/`K` (1024), `m`/`M` (1024^2), `g`/`G` (1024^3). Default `4M`; must be below 4 GiB. Must be a multiple of the device's sector size (512 on most disks, 4096 on a native-4K one; `--list` shows it) whenever that side is a device. |
| `count=N` | Copy at most N blocks (blocks of `bs`). Default: to the end of the input. |
| `skip=N` | Skip N blocks (of `bs`) at the start of the **input** before the first read. |
| `seek=N` | Seek N blocks (of `bs`) into the **output** before the first write. |
| `status=progress` | Print a self-overwriting throughput line to stderr about once a second: `<bytes> bytes  <MiB> MiB  <seconds> s  <MiB/s> MiB/s`. |
| `conv=notrunc` | Do not cut an existing output **file** at the seek point: keep all of it and overwrite in place. Needed for a range write-back into an image. Harmless on a device, which has no length to change. |
| `--yes` | REQUIRED to write to a disk or volume device -- that write ERASES data on that disk. |
| `--force` | Additionally REQUIRED to write to the disk that hosts the running Windows. |
| `--list` | List the machine's physical drives (index, bytes, GiB, sector size, bus, whether it is the system disk, where its volumes are mounted, model) and exit. No elevation needed. |
| `--help`, `-h` | Print the usage to stderr and exit 0. |
| `--version` | Print `dd4win <version>` to stdout and exit 0. |

`count=`, `skip=` and `seek=` are plain decimal block counts with no suffix; a
block is `bs` bytes, so `bs=512 skip=2048 count=2048` skips the first MiB of
the input and copies the next MiB. All four values are range-checked, and so
are the `skip * bs` and `seek * bs` products, so an over-long or non-numeric
value, or a pair whose product would not fit a 64-bit offset, is rejected
rather than silently wrapping. A `skip=` or `seek=` that starts past the end
of a device is refused before anything is opened.

Writing to a **file** without `conv=notrunc` cuts it at the `seek=` point and
writes from there, as `dd` does (with no `seek=` that is a plain recreate, the
normal imaging case); with `conv=notrunc` the file is opened if it exists and
overwritten from the `seek=` offset without shortening the rest.

## Safety

The dangerous act is writing the wrong physical disk, and several gates stand
in front of it. Reading is never gated beyond the Administrator requirement.

- **Every gate keys on what the destination OPENS AS, not on how it was
  typed.** Before either end is opened for access, the destination is opened
  with no rights and the storage stack asked which disk and partition it is,
  so `\\.\physicaldrive0`, `//./PhysicalDrive0`, `\\?\PhysicalDrive0` and
  `\\.\C:` are all refused exactly as `\\.\PhysicalDrive0` is, and a refused
  target is never opened for writing.
- **A device WRITE needs `--yes`**:
  `dd4win: refusing to write <of=> (PhysicalDriveN) without --yes (this ERASES data on that disk)`.
- **The system disk needs `--force` on top of that.** The disk hosting the
  running Windows is found from the Windows directory's volume; a write to it,
  or to any volume on it, is refused even with `--yes` unless `--force` is also
  given: `dd4win: refusing to write the SYSTEM disk <of=> (PhysicalDriveN) without --force`.
- **The target is announced before a sector is written**, to stderr:
  `dd4win: writing PhysicalDriveN  <bytes> bytes  bus <bus>  model <model>`
  (or `writing partition M of PhysicalDriveN ...` for a volume). Compare it
  against `--list`.
- **Every volume on the target disk is locked and dismounted first** --
  lettered, mounted on a folder, or mounted nowhere -- for the duration of the
  write, and remounted after. After a whole-disk write Windows is asked to
  re-read the partition table.

**Run `--list` first, every time**, and confirm the index, size, bus and model
of the disk you mean to write.

## Devices and paths

- A device is `\\.\PhysicalDriveN` (a whole disk) or `\\.\X:` (one volume,
  read or written across its whole partition). The index N is the one `--list`
  prints.
- **Pass device paths from `cmd.exe`, PowerShell or a `.bat`.** A POSIX-style
  shell on Windows rewrites the `\\.\` prefix before the program sees it.
- A device is read and written unbuffered and sector-aligned, which is why
  `bs=` must be a multiple of its sector size; a file-to-file copy has no such
  constraint.
- Paths are taken through the ANSI code page, the character set a Windows
  command shell hands a program; a byte the page leaves undefined is refused
  rather than silently mangled.

## Output and exit codes

Data and the `--list` table go to stdout; progress, the result summary,
warnings and errors go to stderr, prefixed `dd4win: `. Every copy ends with
one summary line, `dd4win: <bytes> bytes (<blocks> blocks of <bs>) copied in
<seconds> s`, counting the data copied (not any sector padding). A Win32
failure carries the decoded OS error text.

| Exit code | Meaning |
|---|---|
| `0` | Success: a complete copy, or `--list` / `--help` / `--version`. |
| `1` | A usage error (a bad or missing argument, a refused write) or an I/O failure -- including a flush that failed on a device destination. |

A short or failed write stops the copy at once rather than retrying; on a
device destination a failed final flush is fatal (the one signal that the data
may not have reached the media), on a file it is only warned. A **file**
source whose length is not a multiple of the output **device's** sector size
has its final block zero-padded to the next sector, with one warning
(`dd4win: warning: final block zero-padded <n> bytes to a sector boundary`);
the pad is not counted in the summary.

## Examples

List the drives (always do this before a write):

```
dd4win --list
```

Back up a whole disk to an image file, with progress (elevated prompt):

```
dd4win if=\\.\PhysicalDrive2 of=disk-full.img bs=4M status=progress
```

Read just the first megabyte of a disk (a quick header grab):

```
dd4win if=\\.\PhysicalDrive2 of=head.img bs=512 count=2048
```

Carve a sector range out of an image (no elevation needed):

```
dd4win if=disk-full.img of=part.img bs=512 skip=<START> count=<COUNT>
```

Copy an image file to another file:

```
dd4win if=disk-full.img of=copy.img bs=1M
```

Patch one range into an existing image in place (`conv=notrunc` keeps the rest):

```
dd4win if=part.img of=disk-full.img bs=512 seek=<START> conv=notrunc
```

Write a range back to a disk (dangerous -- note `--yes`):

```
dd4win if=part.img of=\\.\PhysicalDrive2 bs=512 seek=<START> conv=notrunc --yes
```

Restore a whole image to a disk (erases it -- `--yes`):

```
dd4win if=disk-full.img of=\\.\PhysicalDrive2 bs=4M status=progress --yes
```

Write to the system disk (refused without both flags; think twice):

```
dd4win if=disk-full.img of=\\.\PhysicalDrive0 bs=4M --yes --force
```

Image one volume rather than the whole disk:

```
dd4win if=\\.\E: of=volume.img bs=4M status=progress
```

Edit the image between a read and a write-back with `ext4edit` (in
`..\ext4edit\`), which lists and edits files in an ext2/3/4 image file.

## Limitations

- **Administrator is required** to read or write a device (`--list` is not).
- **No resume / retry.** A read or write error stops the copy; there is no
  bad-block skipping.
- **No hashing.** Verify an image separately (compare sizes, or re-read and
  compare) if you need integrity assurance.
- **Paths are limited to the ANSI code page** (above).
- **A device that will not report its geometry** (an empty card reader, say)
  is copied unbounded, as a file would be, with 512-byte alignment assumed.
- **A `bs=` at or above 4 GiB is refused**; one I/O cannot be that large.
- **`--list` shows a volume's FIRST mount path only**, and a disk with 64 or
  more volumes gets a warning (a write locks the first 64).

## Building

### Tools required

One tool, the compiler, and nothing else -- no shell beyond `cmd.exe`, no
CMake, no Visual Studio, no MSYS2 packages, no SDK beyond the headers the
compiler ships with (the program is pure Win32).

| Tool | What it is for | How to install it |
|---|---|---|
| **WinLibs MinGW-w64 GCC** (64-bit, UCRT runtime, POSIX threads) | the C compiler and linker; the only compiler the build is held to | `winget install BrechtSanders.WinLibs.POSIX.UCRT`, then put its `mingw64\bin` folder on `PATH` -- winget places it under `%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin`. Or download the "UCRT runtime, POSIX threads" 64-bit zip from `https://winlibs.com`, extract it anywhere, and put its `mingw64\bin` on `PATH`. Check with `gcc --version`: it names `MinGW-W64 x86_64-ucrt-posix-seh`; the build here uses release 15.2.0. |

### The build

From `cmd.exe` or PowerShell, in this folder, with `gcc` on `PATH`:

```
.\build.bat
```

(The `.\` is what PowerShell requires for a script in the current folder, and
what `cmd.exe` requires on a machine set not to search the current folder for
programs; it is never wrong.) `build.bat` changes to its own folder, so it runs
from anywhere, and runs one compile of the one source, held to the strict flag
set every C source of this project uses -- C99 with no compiler extensions and
every warning class named -- producing a static `dd4win.exe` in this folder;
a failed compile ends with `BUILD FAILED`. It builds with no warning. By hand,
from the same prompt, the same command:

```
gcc -O2 -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wdeclaration-after-statement -Wvla -static -o dd4win.exe dd4win.c
```

The result imports only `KERNEL32` and the UCRT, so nothing has to be present
on the machine it runs on. Verify a build by the executable's timestamp, and a
rebuild by behaviour rather than by hash: the PE carries a timestamp. There
is no `clean` target; delete `dd4win.exe` by hand.

### The documents

`README.html` and `MANUAL.html` are rendered from the Markdown files beside
them by a Python 3 script kept with the development tooling (it needs the
`markdown` package: `pip install --user markdown`). The HTML is shipped so
that nothing is needed to read the documents; edit the `.md` and re-render.

## Licence

Dual **MIT OR GPL-2.0-or-later**, at your choice. See `LICENSE` and
`GPL-2.0.txt` in this directory.
