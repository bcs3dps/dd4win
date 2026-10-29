# dd4win -- User and Reference Manual

`dd4win` is a small, standalone, dependency-free command-line tool that copies
raw blocks between Windows physical disks and image files, using `dd`-style
arguments. It images a whole raw disk to a file, carves a sector range out of an
image, and writes a range back onto a disk -- with no GUI and no third-party
tool. It is pure Win32 (kernel32 plus the C runtime), builds to a single static
`.exe`, and imports only system DLLs.

This manual is the full reference. The `README.md` beside it is the one-screen
quick start; everything here is the detail behind it.

- Version: **1.2.1**
- Source: `dd4win.c` beside this manual (one translation unit)
- Licence: **MIT OR GPL-2.0-or-later** (dual, at your choice)

---

## 1. What it is, and why it exists

`dd4win` does on Windows what `dd` does on Linux for the two jobs a disk rescue
actually needs: **read a raw block device to a file** (imaging / backup) and
**write a file back to a raw block device or to a range of one** (restore). It
reads and writes the device directly, so it does not care whether Windows can
mount the partitions on it -- an ext4, squashfs, or GPT disk that shows up in
Disk Management as "not initialized" images perfectly.

It was written for an **embedded-device recovery**: once the device's eMMC is
exposed to the PC as a USB mass-storage disk by its bootloader (U-Boot's
`ums 0 mmc 0`), `dd4win` images the whole eMMC, carves one partition out of
that image, and -- for a surgical fix -- writes the edited partition back. It
is also a general-purpose raw imager worth keeping, and is released to the
community.

**Why not an existing tool.** The old Japanese "DD for Windows" is abandoned
(last built around 2019) and Win32DiskImager is a GUI that still has to be driven
by hand. `dd4win` is a single scriptable binary with nothing to install, so the
whole recovery can run from one command line.

**Reading is the safe, primary path. Writing to a physical disk is guarded**
behind explicit flags, because a mistyped output device is the single biggest way
to destroy data with a tool like this. See section 6.

---

## 2. Synopsis

```
dd4win --list
dd4win if=<src> of=<dst> [bs=N] [count=N] [skip=N] [seek=N]
       [status=progress] [conv=notrunc] [--yes] [--force]
dd4win --help
dd4win --version
```

Run a device copy from an **elevated** `cmd.exe` or PowerShell (section 5);
`--list` and a file-to-file copy work from any prompt. A copy requires both
`if=` and `of=`. With no arguments `dd4win` prints its usage and exits non-zero,
so a bare run is never silent.

---

## 3. Requirements

| Need | Detail |
|---|---|
| Windows | Windows 7 or later (the source pins `_WIN32_WINNT` to `0x0601`). |
| Privilege | **Administrator** to READ or WRITE a `\\.\PhysicalDriveN` or `\\.\X:` device. `--list` and a plain file-to-file copy need no elevation; neither does a refused write, which is decided before the device is opened for access. |
| Dependencies | **None.** Static single `.exe`; imports only `KERNEL32` and the UCRT `api-ms-win-crt-*` stubs. |
| Shell | Invoke it from `cmd.exe`, PowerShell, or a `.bat` so device paths survive (section 5). |

---

## 4. Options

Arguments come in two forms: `dd`-style `key=value` tokens, and `--flags`. Order
does not matter. Any token that is not one of those below is an error, and the
tool prints the usage and exits non-zero rather than guessing.

| Argument | Meaning |
|---|---|
| `if=<src>` | Input: a file path, or a device `\\.\PhysicalDriveN` / `\\.\X:`. Required for a copy. |
| `of=<dst>` | Output: a file path, or a device. Required for a copy. |
| `bs=N` | Block size in bytes; accepts a binary suffix `k`/`K`, `m`/`M`, `g`/`G`. Default `4M`; must be below 4 GiB. Must be a multiple of the device's sector size (512 on most disks, 4096 on a native-4K one; `--list` shows it) whenever that side is a device. |
| `count=N` | Copy at most N blocks (blocks of `bs`). Default: to the end of the input. |
| `skip=N` | Skip N blocks (of `bs`) at the start of the **input** before the first read. |
| `seek=N` | Seek N blocks (of `bs`) into the **output** before the first write. |
| `status=progress` | Print a throughput line to stderr about once a second while copying. |
| `conv=notrunc` | Do not cut an existing output **file** at the seek point (keep all of it and overwrite in place). Needed for a range write-back. |
| `--yes` | REQUIRED to write to a disk or volume device -- that write ERASES data on that disk. |
| `--force` | Additionally REQUIRED to write to the disk that hosts Windows. |
| `--list` | List the machine's physical drives and exit. |
| `--help` / `-h` | Print usage to stderr and exit 0. |
| `--version` | Print the version to stdout and exit 0. |

### 4.1 `bs=`, and the units the count arguments use

`bs=` is the one argument that accepts a size suffix, and the suffixes are
**binary** (powers of 1024), matching `dd`:

| Suffix | Multiplier | Example | Bytes |
|---|---|---|---|
| (none) | 1 | `bs=512` | 512 |
| `k` / `K` | 1024 | `bs=64k` | 65,536 |
| `m` / `M` | 1024 * 1024 | `bs=4M` | 4,194,304 (the default) |
| `g` / `G` | 1024 * 1024 * 1024 | `bs=1G` | 1,073,741,824 |

`count=`, `skip=`, and `seek=` are **plain decimal block counts with no suffix**,
and a block is `bs` bytes. So `skip=2048 count=2048 bs=512` skips the first 1 MiB
of the input and copies the next 1 MiB. All four values are range-checked, and
so are the `skip * bs` and `seek * bs` products, so an over-long or non-numeric
value -- or a pair whose product would not fit a 64-bit offset -- is rejected
rather than silently wrapping. A `skip=` or `seek=` that starts past the end of
a device is refused before anything is opened.

### 4.2 `conv=notrunc` and the two kinds of output

- Writing to a **file without** `conv=notrunc` cuts it at the `seek=` point and
  writes from there, as `dd` does: the bytes before `seek=` are kept, and
  everything from it on is replaced by the copy. With no `seek=` that is a
  plain recreate -- the normal imaging case.
- Writing to a **file with** `conv=notrunc` opens the file if it exists and
  overwrites from the `seek=` offset without shortening the rest -- this is how
  you patch a range into an existing image.
- Writing to a **device** never truncates (a device has no length to change);
  `conv=notrunc` is harmless there.

---

## 5. Devices, paths, and the shell trap

### 5.1 Device paths

- `\\.\PhysicalDriveN` -- a whole raw disk, where N is the index from `--list`
  (and from Windows Disk Management). This is what you image and, for a restore,
  what you write back to.
- `\\.\X:` -- a mounted volume by drive letter. Opened raw and unbuffered like a
  disk, and spanning the whole partition (the filesystem's own end-of-volume
  cut, one sector short on NTFS, is lifted). The `--yes` / `--force` guards of
  section 6 apply to it exactly as to the disk it sits on.
- **What a path IS is decided by opening it, not by its spelling.** Each end is
  first opened with no access rights and asked to identify itself, so
  `\\.\physicaldrive0`, `//./PhysicalDrive0`, `\\?\PhysicalDrive0`, `\\.\C:` and
  `\\.\HarddiskNPartitionM` are all recognised as the device they name. Anything
  that does not identify itself as a device is treated as an ordinary **file
  path**; a path spelled as a device that cannot be opened is an error, never a
  file to create.

### 5.2 Pass device paths from cmd.exe or PowerShell, never a POSIX-style shell

A POSIX-style shell on Windows **rewrites the `\\.\` prefix** (its path
mangling turns `\\.\PhysicalDrive2` into something else) before `dd4win` ever
sees it, and the open then fails or -- worse for a write -- could name the wrong
object. **Run `dd4win` from `cmd.exe`, PowerShell, or a `.bat`**, where an
argument reaches the program as typed.

In `cmd.exe` the device path is written plainly:

```
dd4win if=\\.\PhysicalDrive2 of=emmc.img bs=4M status=progress
```

### 5.3 Blocks vs bytes

`skip=`, `seek=`, and `count=` are measured in `bs`-sized blocks, not bytes, so
their meaning changes with `bs`. To address a partition by its sector numbers
(which is what U-Boot's `part list` prints), use `bs=512` so one block is one
512-byte sector and `skip` / `seek` / `count` are sector counts directly.

---

## 6. Safety model

The whole design assumes that the dangerous act is **writing the wrong physical
disk**, and puts several gates in front of it. Reading is never gated beyond the
Administrator requirement.

**Every gate keys on what the destination OPENS AS, never on how it was typed.**
Before either end is opened for access, `dd4win` opens the destination with no
rights at all and asks the storage stack which disk and partition it is. So a
spelling the tool has never seen reaches the same refusals as the canonical one,
and a refused target is never opened for writing -- which also means a refusal
reads the same from an ordinary prompt as from an elevated one.

1. **Administrator.** Reading or writing any device needs elevation. Without it
   the open for access fails with a Win32 access error.
2. **`--yes` for any device write.** A copy whose `of=` is a disk or a volume on
   one is refused unless `--yes` is present:
   `dd4win: refusing to write <of=> (PhysicalDriveN) without --yes (this ERASES data on that disk)`.
3. **`--force` for the system disk.** `dd4win` finds the physical disk that hosts
   the running Windows installation (it resolves the Windows directory to its
   volume and reads that volume's backing disk). A write to that disk -- or to
   any volume on it -- is refused even with `--yes`, unless `--force` is also
   given:
   `dd4win: refusing to write the SYSTEM disk <of=> (PhysicalDriveN) without --force`.
   If the system disk cannot be identified, nothing is treated as the system disk
   -- so `--yes` alone still governs, and the announcement in step 4 is your
   check.
4. **It announces the target before writing a sector.** For a device write it
   prints, to stderr,
   `dd4win: writing PhysicalDriveN  <bytes> bytes  bus <bus>  model <model>`
   (or `writing partition M of PhysicalDriveN ...` for a volume) so a wrong
   index is visible before any data moves. Compare it against `--list`.
5. **It locks and dismounts every volume on the target disk first.** Every
   volume Windows knows on the destination disk -- with a drive letter, mounted
   on a folder, or mounted nowhere, such as an EFI system partition -- is locked
   and dismounted for the duration of the write, then remounted after. On a disk
   carrying only filesystems Windows cannot mount (ext4, squashfs) this finds
   nothing and is a no-op; it is there so the tool is safe on a disk that does
   carry Windows volumes.

**Use `--list` first, every time, and confirm the index, size, bus, and model of
the disk you mean to write.** That one habit prevents the mistake the guards are
there to catch.

---

## 7. Output and exit codes

`dd4win` follows the Unix convention: **data and the `--list` table go to stdout;
progress, the result summary, warnings, and errors go to stderr.** This lets you
redirect an image to stdout-like tooling without progress noise contaminating it,
though the normal use names `of=` directly.

- **`--list`** prints a table to stdout:

  ```
  Physical drives on this machine:
    index            bytes      GiB sector bus    system volumes    model
    0        512110190592   476.94    512 NVMe   YES    C: *       <vendor> <product>
    2          7818182656     7.28    512 USB    -      -          <vendor> <product>

  Tip: image a drive with   dd4win if=\\.\PhysicalDriveN of=out.img bs=4M status=progress
  ```

  Columns: the `PhysicalDriveN` index, the size in bytes and in GiB, the sector
  size in bytes (what `bs=` must divide by), the bus type (USB, SATA, NVMe, SD,
  MMC, ...), whether the disk hosts Windows (`YES` / `-`), where each volume on
  it is mounted -- a drive letter as `C:`, a folder mount point as its path,
  `*` for a volume mounted nowhere (an EFI or recovery partition, say), or `-`
  when Windows knows no volume on the disk -- and the model string. A drive
  index with no disk is skipped; indexes 0 through 63 are probed, and if none
  can be opened the table says so. No elevation is needed.

- **`status=progress`** prints a single, self-overwriting line to stderr about
  once a second:
  `<bytes> bytes  <MiB> MiB  <seconds> s  <MiB/s> MiB/s`.

- **The result summary** is one line to stderr at the end of every copy:
  `dd4win: <bytes> bytes (<blocks> blocks of <bs>) copied in <seconds> s`. The
  byte count is the real data copied, not any sector padding (section 8.3).

- **Errors** are one line to stderr, prefixed `dd4win: `. A Win32 failure also
  carries the decoded OS error text, so a failure names both what was being done
  and why the OS refused.

| Exit code | Meaning |
|---|---|
| `0` | Success (a complete copy, or `--list` / `--help` / `--version`). |
| `1` | A usage error (bad or missing argument, a refused write) or an I/O failure -- including a flush that failed on a device destination. |

---

## 8. Behaviour notes

### 8.1 Unbuffered, sector-aligned device I/O

A device is opened with `FILE_FLAG_NO_BUFFERING`, which bypasses the Windows
cache and requires every offset, length, and buffer to be aligned to the
device's sector size -- 512 bytes on most disks, 4096 on a native-4K one, read
from the device's geometry and shown by `--list`. `dd4win` satisfies this by
(a) requiring `bs` to be a multiple of each device's sector size, and (b)
allocating its I/O buffer with `VirtualAlloc`, which is page-aligned and
therefore sector-aligned. A file (no device on either side) is opened normally
and has no alignment constraint, so you can use any `bs` for file-to-file work.

### 8.2 How much is copied

A copy runs until the input ends or until `count=` blocks have been copied,
whichever comes first. A **file** ends with a read that returns zero bytes. A
**device** does not end that way -- a read that runs past a raw disk's end is
refused whole rather than returned short -- so for a device source `dd4win`
takes the length from the device's geometry and asks for no more than remains,
ending the copy exactly at the device's last byte. Imaging a whole device
therefore needs no `count=`, whatever its size and whatever `bs` is. `skip=`
and `seek=` are applied once, before the loop.

### 8.3 The final-block zero-pad (file source -> device destination only)

Unbuffered device writes must be whole sectors. The only way a short, unaligned
block can occur is a **file** source whose total length is not a multiple of the
output device's sector size, copied to a **device**. In that one case `dd4win`
zero-pads the final block up to the next sector boundary so the write is legal,
and warns once:
`dd4win: warning: final block zero-padded <n> bytes to a sector boundary`. The
pad bytes are written to the device but are **not** counted in the result
summary's byte total. Device-to-file and device-to-device copies always move
whole sectors and never pad.

### 8.4 Flush, and what a failed write means

After the loop `dd4win` calls `FlushFileBuffers` on the output so the data is on
the media before it reports success. **On a device destination a flush failure
is fatal** (exit 1): it is the one signal that the data may not have reached the
media, so a script checking the exit code must not see success. On a file it is
only warned, the data being in the system cache on its way to disk. A **short or
failed write stops the copy immediately** with an error -- a partial write to a
raw disk is treated as unreliable rather than retried; a short write is reported
with both byte counts. After a whole-disk write `dd4win` asks Windows to re-read
the partition table, so Explorer and Disk Management show the new layout without
the card being re-seated.

### 8.5 Path character set

Paths are converted from the command line to UTF-16 through the process ANSI code
page, which is the character set a Windows command shell hands a program.
Device paths and ordinary paths are fine; a path holding a byte the code page
leaves undefined is rejected with an error rather than silently mangled. A
character the code page cannot hold cannot be typed at that shell; one passed
from PowerShell reaches the tool as `?` and the open then fails as "not found",
so no wrong file or device is ever opened.

---

## 9. An embedded-device recovery workflow

This is the job `dd4win` was written for: an embedded device whose eMMC can be
exposed to the PC as a USB mass-storage disk by its bootloader. `dd4win`'s part
is steps b-d below, run in an **elevated** `cmd.exe`.

1. (Bench) At the bootloader prompt run U-Boot's `ums 0 mmc 0` (or the
   equivalent) to expose the whole eMMC to the PC as a USB disk. Windows offers
   to "format" or "initialize" it -- **cancel**; the data is there, Windows just
   cannot read the filesystem.

   a. **Find the disk.** The eMMC shows as a USB disk with no drive letter:

      ```
      dd4win --list
      ```

      Note its index (say 2) and confirm the size and bus (USB) against what
      you know of the device.

   b. **Image the whole eMMC** (the undo for everything after):

      ```
      dd4win if=\\.\PhysicalDrive2 of=emmc-full.img bs=4M status=progress
      ```

   c. **Carve the partition you need out** of the image, using the start LBA and
      sector count the bootloader's partition listing (`part list mmc 0`)
      reported for it (`bs=512` makes the block numbers equal sector numbers):

      ```
      dd4win if=emmc-full.img of=part.img bs=512 skip=<START> count=<COUNT>
      ```

      Edit `part.img` with a filesystem editor for its format (for ext2/3/4,
      `ext4edit` beside this tool).

   d. **Write the edited partition back** onto the `ums`-exposed eMMC, at the same
      sector range (dangerous -- note `--yes`):

      ```
      dd4win if=part.img of=\\.\PhysicalDrive2 bs=512 seek=<START> conv=notrunc --yes
      ```

      Then end the `ums` session on the device's console and reboot it.

---

## 10. More examples

List the drives (always do this before a write):

```
dd4win --list
```

Back up a disk to a file, with progress:

```
dd4win if=\\.\PhysicalDrive2 of=disk-full.img bs=4M status=progress
```

Copy an image file to another file (no elevation needed):

```
dd4win if=disk-full.img of=copy.img bs=1M
```

Read just the first megabyte of a disk (a quick header grab):

```
dd4win if=\\.\PhysicalDrive2 of=head.img bs=512 count=2048
```

Restore a whole image to a disk (erases it -- `--yes`):

```
dd4win if=disk-full.img of=\\.\PhysicalDrive2 bs=4M status=progress --yes
```

Patch one range into an existing image in place (`conv=notrunc`):

```
dd4win if=part.img of=disk-full.img bs=512 seek=<START> conv=notrunc
```

---

## 11. Building

From `cmd.exe` or PowerShell with the **WinLibs MinGW-w64 gcc 15.2.0** on `PATH`
(as plain `gcc`; the README's "Building" section says how it is installed):

```
.\build.bat
```

`build.bat` changes to its own folder and runs the one compile below; nothing
else is needed -- no other shell, no make, no SDK. The compile is a single,
static translation unit, held to the strict flag set every C source of this
project uses -- C99 with no compiler extensions and every warning class named:

```
gcc -O2 -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wdeclaration-after-statement -Wvla -static -o dd4win.exe dd4win.c
```

It builds clean with no warnings. The result is a standalone `dd4win.exe` that
imports only `KERNEL32` and the UCRT runtime stubs; nothing else needs to be
present on the target machine. (The build is not byte-reproducible -- the PE
carries a timestamp -- so verify a rebuild by behaviour, not by hash.)

---

## 12. Verification status

`dd4win` 1.2.1 -- 1.2.0 with its two findings fixed: the fall-back bus label is
the one constant, and `--list` fetches a volume's mount-path list again when
the first buffer is too small for it -- has been verified **device-free, from
an ordinary (unelevated) prompt** -- the checks that need no disk to be read
or written (1.2.1 itself was checked with `--version` and `--list`; the
longer-list path of `--list` needs a volume with enough folder mount points
to overflow 260 characters, which no machine here has):

- `--list` enumerates the host's disks with correct sizes, sector sizes, buses,
  and volumes, including a volume with no drive letter.
- A file-to-file copy is byte-identical to its source; a new output file, an
  existing one, and `NUL` all work.
- A `skip=` / `count=` range read lands at the right offset and length.
- A `seek=` + `conv=notrunc` write patches a range of an existing file in place;
  a `seek=` write WITHOUT `conv=notrunc` keeps the bytes before the seek point
  and cuts the file after the copy, as `dd` does.
- The write guards fire for every spelling of the system disk tried --
  `\\.\PhysicalDrive0`, `\\.\physicaldrive0`, `//./PhysicalDrive0`,
  `\\?\PhysicalDrive0`, `\\.\C:` and `//./C:` -- refused without `--yes`, and
  refused as the SYSTEM disk with `--yes` but no `--force`; a device that is
  not there is reported, not created as a file.
- `bs=4G` is refused; `bs=3G` is accepted; a `skip=` or `seek=` whose product
  with `bs` would overflow is refused; a `bs` that does not divide a device's
  sector size is refused before anything is opened.

**Not yet run against a real device, and worth a bench check first:** a
whole-disk image of a disk whose size is not a multiple of `bs` (the bounded
read of section 8.2), a write to a disk carrying an unlettered volume (the lock
of section 6 step 5), the fatal flush, and the partition-table refresh. Each is
written from Windows' documented rules; none has been exercised on hardware.

---

## 13. Limitations

- **Administrator is required** to read or write a device (`--list` is not).
- **No resume / retry.** A read or write error stops the copy; there is no bad-
  block skipping or retry loop.
- **No hashing.** Verify an image separately (compare sizes, or re-read and
  compare) if you need integrity assurance.
- **Paths are limited to the ANSI code page** (section 8.5).
- **A device that will not report its geometry** (an empty card reader, say)
  is copied unbounded, as a file would be, with 512-byte alignment assumed.
- **A `bs=` at or above 4 GiB is refused**; one I/O cannot be that large.
- **`--list` shows a volume's FIRST mount path only**, and a disk with 64 or
  more volumes gets a warning (a write locks the first 64).

---

## 14. Licence

Dual-licensed **MIT OR GPL-2.0-or-later**, at your choice. The full texts are in
`LICENSE` and `GPL-2.0.txt` in this directory, and the SPDX header is at the top
of `dd4win.c`.

---

## 15. See also

- `README.md` -- the one-screen quick start for this tool.
- `ext4edit` (the sibling tool) -- lists and edits files inside an ext2/3/4
  image, such as a partition carved out with `dd4win`.
