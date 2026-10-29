/*-------------------------------------------------------------------------------*/
/* dd4win.c                                                                      */
/*                                                                               */
/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later                              */
/* Copyright (c) 2026 B. C. Services                                             */
/*                                                                               */
/* Dual-licensed at your option: MIT or GPL-2.0-or-later.  See LICENSE and       */
/* GPL-2.0.txt beside this source.                                               */
/*                                                                               */
/* dd4win -- a small, standalone, dependency-free raw block reader/writer for    */
/* Windows physical disks and files, with dd-style arguments.  It exists to      */
/* image a whole raw disk to a file, carve a byte/sector range out, and write    */
/* a range back, from a plain command line with no GUI and no third-party        */
/* tool.  READING (imaging / backup) is the primary, safe path; WRITING to a     */
/* physical device is the secondary path and is guarded behind explicit          */
/* flags because a mistyped target is the single biggest footgun of a tool       */
/* like this.                                                                    */
/*                                                                               */
/* Pure Win32 API (kernel32 only); builds to a single static .exe with the       */
/* WinLibs / MinGW-w64 gcc.  No external libraries.                              */
/*                                                                               */
/* THE GUARDS KEY ON WHAT WAS OPENED, NEVER ON WHAT WAS TYPED.  Each end is      */
/* first opened with no access at all and asked what it is                       */
/* (IOCTL_STORAGE_GET_DEVICE_NUMBER), so \\.\PhysicalDrive0, \\.\physicaldrive0, */
/* //./PhysicalDrive0, \\?\PhysicalDrive0 and \\.\C: all reach the same          */
/* refusals, the same announcement and the same volume locks -- which no test    */
/* on the spelling could promise, since Windows accepts more spellings of a      */
/* device than any list knows.                                                   */
/*                                                                               */
/* BOOLEANS.  The tool's own flags are int holding TRUE or FALSE -- the          */
/* classic form, since no library here requires <stdbool.h> -- and the two       */
/* names are windows.h's own (1 and 0), the same ones the Windows API's BOOL     */
/* results use; a flag is tested as a truth value and never compared with        */
/* TRUE.  Every BOOL in this file is a Windows API result and is marked so       */
/* where it is declared.                                                         */
/*                                                                               */
/* NAMES.  Every function here is private to this file (static), so each         */
/* carries its group prefix with only its initial capital (Log_, Util_, Drv_,    */
/* Dd_, Arg_); the ALL-CAPS prefix is for the constants and the one table.       */
/*                                                                               */
/* Build (from cmd.exe or PowerShell, the WinLibs gcc on PATH):                  */
/*     gcc -O2 -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes     */
/*         -Wmissing-prototypes -Wcast-qual -Wdeclaration-after-statement        */
/*         -Wvla -static -o dd4win.exe dd4win.c                                  */
/* (the build script build.bat runs exactly this; it must compile with no        */
/* warning at all).                                                              */
/*-------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Feature-test macros, then the includes.                                    */
/*                                                                            */
/* _WIN32_WINNT is pinned to Windows 7 so the storage-query, volume-extent    */
/* and disk-length structures in <winioctl.h> are all declared.               */
/*----------------------------------------------------------------------------*/
#define _WIN32_WINNT 0x0601         // target floor: Windows 7 (declares the IOCTL structs)
#define WIN32_LEAN_AND_MEAN         // trim <windows.h> to what we use

/* What each header is used for here: windows.h for the handles and the        */
/* calls on them (CreateFileW, ReadFile / WriteFile, SetFilePointerEx,         */
/* DeviceIoControl, FlushFileBuffers), VirtualAlloc for the sector-aligned     */
/* buffer, FormatMessageA for the error text, GetTickCount64 for the           */
/* progress clock, the volume enumeration and the narrow / wide conversions;   */
/* winioctl.h for the IOCTL and FSCTL control codes and their reply            */
/* structures (geometry, device number, disk extents, the property query);     */
/* stdio.h for the table, the diagnostics and the usage text; stdlib.h for     */
/* malloc / free; stdarg.h for Log_Err's variadic arguments; string.h for      */
/* the argument parsing and the buffer copies; wchar.h for wcslen.             */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

/*----------------------------------------------------------------------------*/
/* Definitions -- documentation table.                                        */
/*----------------------------------------------------------------------------*/
#define DD_VERSION       "1.3.0"                  // tool version, printed by --version
#define DD_SECTOR_SIZE      512ULL                // fallback sector size when a device will not report its geometry (bytes)
#define DD_DEFAULT_BS       (4ULL * 1024 * 1024)  // default block size when bs= omitted (bytes)
#define DD_MAX_BS           0xFFFFFFFFULL         // bs= must be BELOW this: ReadFile / WriteFile take a 32-bit length
#define DD_MAX_PHYS_DRIVE   64                    // highest \\.\PhysicalDriveN index --list probes
#define DD_PROGRESS_MS      1000UL                // status=progress update interval (milliseconds)
#define DD_MAX_VOLUMES      64                    // volumes (lettered or not) we can hold locked on one disk write
#define DD_VOLPATH_CCH      64                    // characters reserved for one \\?\Volume{GUID}\ path (they are 49)
#define DD_MODEL_CCH        128                   // characters reserved for a drive's model string

#define DD_SUCCESS       0           // process exit code: success
#define DD_FAILURE       1           // process exit code: usage error or I/O failure

#define DRV_BUS_OTHER    "other"     // --list's bus label for any bus type the table below does not name

/*----------------------------------------------------------------------------*/
/* Dd_strctProbe:                                                             */
/*                                                                            */
/* What a path turned out to BE when it was opened with no access and asked,  */
/* as opposed to what its spelling suggested.  Filled by Dd_ProbePath before  */
/* either end of a copy is opened for real; every guard, the alignment rule,  */
/* the read bound and the announcement read from here.                        */
/*----------------------------------------------------------------------------*/
typedef struct Dd_strctProbe
{
    int                 bDevice;     // TRUE when the path opened as a storage device (a disk, a volume, a partition)
    int                 iDisk;       // the physical disk number behind it (PhysicalDriveN's N), -1 when not a device
    int                 iPartition;  // 0 for a whole disk; the partition number for a \\.\X: volume or partition device
    unsigned long long  ullLength;   // the device's byte length from its geometry, 0 when unknown or not a device
    unsigned long long  ullSector;   // the device's bytes per sector from its geometry; DD_SECTOR_SIZE when it would not say
} Dd_strctProbe;

/*----------------------------------------------------------------------------*/
/* Dd_strctConfig:                                                            */
/*                                                                            */
/* One parsed command line.  Every field is filled by Arg_Parse and read-only */
/* afterward; the paths point into argv and are never freed.                  */
/*----------------------------------------------------------------------------*/
typedef struct Dd_strctConfig
{
    const char         *pszIf;      // if= source path (file or \\.\device), NULL if absent
    const char         *pszOf;      // of= destination path (file or \\.\device), NULL if absent
    unsigned long long  ullBs;      // block size in bytes (default DD_DEFAULT_BS)
    unsigned long long  ullCount;   // max blocks to copy; 0 means "until end of input"
    unsigned long long  ullSkip;    // input blocks to skip before the first read
    unsigned long long  ullSeek;    // output blocks to seek before the first write
    int                 bCount;     // count= was given (so ullCount 0 is meaningful if ever set)
    int                 bProgress;  // status=progress requested
    int                 bNoTrunc;   // conv=notrunc requested (never truncate an of= file)
    int                 bYes;       // --yes: caller accepts a WRITE to a physical device
    int                 bForce;     // --force: caller accepts writing the SYSTEM disk
    int                 bList;      // --list: enumerate physical drives and exit
    int                 bHelp;      // --help / -h: print usage and exit
    int                 bVersion;   // --version: print the version and exit
} Dd_strctConfig;

/*----------------------------------------------------------------------------*/
/* Drv_strctBusName:                                                          */
/*                                                                            */
/* One row of the bus-name table: a bus type as the storage stack reports it  */
/* and the short label --list prints for it.                                  */
/*----------------------------------------------------------------------------*/
typedef struct Drv_strctBusName
{
    STORAGE_BUS_TYPE  enmBus;     // the bus type, as STORAGE_DEVICE_DESCRIPTOR.BusType reports it
    const char       *pszName;    // the label printed in --list's bus column
} Drv_strctBusName;

/*----------------------------------------------------------------------------*/
/* Globals -- the one table.                                                  */
/*----------------------------------------------------------------------------*/

/* The bus-name table -- documentation table.  Drv_BusName walks it for the   */
/* --list column; a bus type not listed here prints as DRV_BUS_OTHER.  The    */
/* labels are short on purpose, so the column stays one word wide.            */
static const Drv_strctBusName DRV_aBusNames[] =
{
    { BusTypeUsb,               "USB"   },    // USB mass storage (external drive, card reader, USB-bridged device)
    { BusTypeSata,              "SATA"  },    // Serial ATA fixed disk or SSD
    { BusTypeScsi,              "SCSI"  },    // SCSI -- also many USB bridges that report as SCSI
    { BusTypeAta,               "ATA"   },    // legacy parallel ATA / IDE
    { BusTypeNvme,              "NVMe"  },    // NVM Express PCIe SSD
    { BusTypeSd,                "SD"    },    // SD card (SDHC / SDXC) through a reader
    { BusTypeMmc,               "MMC"   },    // MMC or eMMC accessed directly, not via a USB card-reader bridge
    { BusTypeRAID,              "RAID"  },    // a RAID set presented to Windows as one disk
    { BusTypeFileBackedVirtual, "vfile" },    // file-backed virtual disk (a mounted VHD / ISO)
};

/*----------------------------------------------------------------------------*/
/* The file-private functions, declared ahead -- documentation table.  Every  */
/* function in this file is static; each is defined below under its group's   */
/* section header, with its boxed header on the definition.                   */
/*----------------------------------------------------------------------------*/
static void         Log_Err(const char *pszFmt, ...);                                         // one diagnostic line to stderr
static void         Log_WinErr(const char *pszWhat, DWORD dwErr);                             // a diagnostic with the decoded Win32 error
static int          Util_LooksLikeDevicePath(const char *psz);                                // is the path SPELLED as a device path
static int          Util_ParseU64(const char *psz, unsigned long long *pullOut);              // a plain unsigned decimal, strictly
static int          Util_ParseSize(const char *psz, unsigned long long *pullOut);             // a byte size with a k / M / G suffix
static wchar_t     *Util_WidenPath(const char *psz);                                          // a command-line path as a wide string
static const char  *Drv_BusName(STORAGE_BUS_TYPE enmBus);                                     // a bus type's --list label
static int          Drv_DeviceGeometry(HANDLE hDev, unsigned long long *pullLength,
                                       unsigned long long *pullSector);                       // a device's length and sector size
static int          Drv_DeviceNumber(HANDLE hDev, int *piDisk, int *piPartition);             // which disk and partition a handle is
static void         Drv_QueryModelBus(HANDLE hDev, char *pszModel, size_t cchModel,
                                      const char **ppszBus);                                  // a drive's model text and bus label
static int          Drv_VolumeDiskNumbers(HANDLE hVol, int *aiDisks, int iDisksMax,
                                          int *piDisks);                                      // the disks backing a volume
static int          Drv_SystemDiskNumber(void);                                               // the disk that hosts Windows, or -1
static int          Drv_VolumesOnDisk(int iDrive, wchar_t awszVols[][DD_VOLPATH_CCH],
                                      int iVolsMax);                                          // every volume on a disk, by GUID path
static void         Drv_LettersOnDisk(int iDrive, char *pszOut, size_t cchOut);               // where a disk's volumes are mounted
static void         Drv_LockDismountVolumesOnDisk(int iDrive, HANDLE *ahVols,
                                                  int iVolsMax, int *piVols);            // take a disk's volumes offline
static int          Dd_List(void);                                                       // the --list table
static int          Dd_ProbePath(const wchar_t *pwszPath, Dd_strctProbe *pProbe);        // what a path IS, with no access
static void         Dd_AllowWholeVolume(HANDLE hDev);                                    // lift a volume's filesystem-end cut
static HANDLE       Dd_OpenSource(const wchar_t *pwszPath, int bDevice);                 // open if= for reading
static HANDLE       Dd_OpenDest(const wchar_t *pwszPath, int bDevice);                   // open of= for writing
static int          Dd_Seek(HANDLE h, unsigned long long ullOff, const char *pszWhich);  // skip= / seek= on a handle
static int          Dd_Copy(const Dd_strctConfig *pCfg);                                 // the guarded copy loop
static void         Arg_Usage(void);                                                     // the usage text, to stderr
static int          Arg_Parse(int argc, char **argv, Dd_strctConfig *pCfg);              // the command line into a config

/*----------------------------------------------------------------------------*/
/* Logging -- every diagnostic the tool prints goes through these two.        */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Log_Err:                                                                   */
/*                                                                            */
/* Error-only logging to stderr with a fixed "dd4win: " prefix and a trailing */
/* newline.  There is deliberately no success / info logging (global coding   */
/* style: error-only); the copy's own result line and --list's table are the  */
/* tool's normal output, not log lines.                                       */
/*                                                                            */
/* Arguments:                                                                 */
/*     pszFmt : printf-style format string.                                   */
/*     ...    : the format's arguments.                                       */
/*                                                                            */
/* Returns:                                                                   */
/*     void : writes one line to stderr.                                      */
/*----------------------------------------------------------------------------*/
static void Log_Err(const char *pszFmt, ...)
{
    va_list args;

    /* The line is emitted as THREE separate stdio writes rather than composed     */
    /* into one local buffer first.  That means no fixed-size scratch buffer and   */
    /* so no truncation of a long message, and it is safe because the program is   */
    /* single-threaded -- nothing can interleave between the three writes and      */
    /* split one diagnostic across two lines.                                      */

    /* Write 1: the fixed prefix that tags every diagnostic as this tool's. */
    fputs("dd4win: ", stderr);

    /* Write 2: the caller's printf-style message, expanded straight onto    */
    /* stderr through the va_list -- va_start / va_end bracket exactly the   */
    /* vfprintf that consumes the variadic arguments.                        */
    va_start(args, pszFmt);
    vfprintf(stderr, pszFmt, args);
    va_end(args);

    /* Write 3: the terminating newline.  Callers pass their message WITHOUT a   */
    /* trailing '\n', so it is added in exactly one place here.                  */
    fputc('\n', stderr);
}

/*----------------------------------------------------------------------------*/
/* Log_WinErr:                                                                */
/*                                                                            */
/* Print a context message followed by the decoded Win32 error the caller     */
/* captured, so a failure names both what we were doing and why the OS        */
/* refused.  The code is a parameter rather than read here because the        */
/* caller must take GetLastError() AT the failure site: any call made after   */
/* it, a stdio write included, may overwrite the thread's last error, and a   */
/* message decoded from a stale code would name the wrong failure.            */
/*                                                                            */
/* Arguments:                                                                 */
/*     pszWhat : short description of the operation that failed.              */
/*     dwErr   : the GetLastError() value captured at the failure site.       */
/*                                                                            */
/* Returns:                                                                   */
/*     void : writes one line to stderr.                                      */
/*----------------------------------------------------------------------------*/
static void Log_WinErr(const char *pszWhat, DWORD dwErr)
{
    char  *pszMsg;
    DWORD  dwLen;

    /* FormatMessageA allocates the buffer and points pszMsg at it; start it   */
    /* NULL so the "no text" branch is taken if the call writes nothing.       */
    pszMsg = NULL;

    /* Ask the OS for a human-readable string for this error code.  The three   */
    /* flags each matter: ALLOCATE_BUFFER has the call allocate the text with   */
    /* LocalAlloc and store the pointer THROUGH the buffer argument -- which    */
    /* is why pszMsg's address is passed, cast to the string type the           */
    /* parameter is declared as -- and that is why LocalFree, not free,         */
    /* releases it below; FROM_SYSTEM looks the code up in the system message   */
    /* table; IGNORE_INSERTS keeps a message that holds %1-style insertion      */
    /* marks from failing for want of arguments, since none are supplied.       */
    /* Language 0 is the caller's default.  The return is the length written    */
    /* without the terminator, 0 on failure.                                    */
    dwLen = FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, dwErr, 0, (LPSTR)&pszMsg, 0, NULL);

    if (dwLen != 0 && pszMsg != NULL)
    {   /* The OS supplied text: it carries its own trailing CR/LF, so emit   */
        /* it as-is after our own context.                                    */
        fprintf(stderr, "dd4win: %s: error %lu: %s", pszWhat,
                (unsigned long)dwErr, pszMsg);
        LocalFree(pszMsg);
    }
    else
    {   /* No text available for this code: report the bare number. */
        fprintf(stderr, "dd4win: %s: error %lu\n", pszWhat,
                (unsigned long)dwErr);
    }
}

/*----------------------------------------------------------------------------*/
/* Utilities -- the number parsers and the path conversions.                  */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Util_LooksLikeDevicePath:                                                  */
/*                                                                            */
/* Whether a path is SPELLED as a Windows device or object-namespace path:    */
/* "\\.\" or "\\?\", with either slash direction, so \\.\PhysicalDrive0,      */
/* //./PhysicalDrive0 (the forward-slash spelling) and \\?\C: all count.      */
/*                                                                            */
/* This is a HINT and nothing more.  It decides exactly one thing: when such  */
/* a path cannot be opened, the tool reports that rather than trying to       */
/* CREATE a file of that name.  Nothing that protects a disk reads it -- the  */
/* guards, the unbuffered open, the alignment rule and the volume locks all   */
/* key on Dd_ProbePath's answer about what the path actually opened as,       */
/* because a text test cannot know every spelling Windows accepts.            */
/*                                                                            */
/* Arguments:                                                                 */
/*     psz : the path to test (narrow, as given on the command line).         */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE when psz starts with a device / object prefix, else FALSE.  */
/*----------------------------------------------------------------------------*/
static int Util_LooksLikeDevicePath(const char *psz)
{
    if (psz == NULL)
    {   /* No path at all. */
        return(FALSE);
    }

    /* Prefix shape: separator, separator, '.' or '?', separator -- where each   */
    /* separator may be either slash, since Windows accepts both.  The four      */
    /* tests run left to right and && stops at the first that fails, so a path   */
    /* shorter than four characters is never read past its terminating NUL:      */
    /* the NUL fails the test at its own position before the next is reached.    */
    if ((psz[0] == '\\' || psz[0] == '/') &&
        (psz[1] == '\\' || psz[1] == '/') &&
        (psz[2] == '.'  || psz[2] == '?') &&
        (psz[3] == '\\' || psz[3] == '/'))
    {   /* Spelled as a device or object-namespace path. */
        return(TRUE);
    }

    /* Spelled as an ordinary file path. */
    return(FALSE);
}

/*----------------------------------------------------------------------------*/
/* Util_ParseU64:                                                             */
/*                                                                            */
/* Parse a plain unsigned decimal integer (used for count= / skip= / seek=,   */
/* which are block counts and carry no size suffix).                          */
/*                                                                            */
/* Arguments:                                                                 */
/*     psz     : the text to parse.                                           */
/*     pullOut : receives the parsed value on success.                        */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE on a clean parse, FALSE on empty / non-digit / overflow.    */
/*----------------------------------------------------------------------------*/
static int Util_ParseU64(const char *psz, unsigned long long *pullOut)
{
    unsigned long long ullVal;
    const char        *p;

    /* Accumulate into ullVal, scanning from the start of the token. */
    ullVal = 0;
    p      = psz;

    if (p == NULL || *p == '\0')
    {   /* An empty string is not a number: reject it rather than fall through   */
        /* to a silent 0, so a missing value such as "count=" is an error, not   */
        /* a zero-block copy that looks like it did nothing on purpose.          */
        return(FALSE);
    }

    /* Accumulate left to right, validating EACH digit before it is folded   */
    /* in.  The strictness is deliberate: count / skip / seek are turned     */
    /* straight into byte offsets (value * bs) against a raw disk, so a      */
    /* value that wrapped or quietly stopped at the first bad character      */
    /* would seek to the WRONG place -- the one error class this tool must   */
    /* never commit silently.                                                */
    while (*p != '\0')
    {   /* One digit per iteration. */
        if (*p < '0' || *p > '9')
        {   /* A non-digit anywhere (a sign, a space, a stray suffix) voids   */
            /* the whole token.  strtoul-style "parse as far as it goes" is   */
            /* NOT done, precisely so "12x" cannot be accepted as 12.         */
            return(FALSE);
        }

        /* Overflow guard applied BEFORE the multiply-add, so the wrap can never   */
        /* actually occur: if ullVal already exceeds (2^64-1 - digit) / 10, then   */
        /* ullVal*10 + digit would pass 2^64-1 and wrap to a small number.  On a   */
        /* raw-disk offset a wrapped count is the dangerous case, so reject it.    */
        if (ullVal > (0xFFFFFFFFFFFFFFFFULL - (unsigned long long)(*p - '0')) / 10ULL)
        {   /* The next digit would overflow a 64-bit accumulator. */
            return(FALSE);
        }

        ullVal = ullVal * 10ULL + (unsigned long long)(*p - '0');
        p++;
    }

    /* Every character was a digit and nothing overflowed: hand back the value. */
    *pullOut = ullVal;

    /* A clean parse. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Util_ParseSize:                                                            */
/*                                                                            */
/* Parse a byte size with an optional single-letter binary suffix, as dd      */
/* accepts for bs=: a bare number is bytes; a trailing k/K, m/M or g/G        */
/* multiplies by 1024, 1024^2 or 1024^3 respectively.                         */
/*                                                                            */
/* Arguments:                                                                 */
/*     psz     : the text to parse (e.g. "512", "4M", "1g").                  */
/*     pullOut : receives the parsed byte count on success.                   */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE on a clean parse, FALSE on empty / bad digit / bad suffix / */
/*           overflow.                                                        */
/*----------------------------------------------------------------------------*/
static int Util_ParseSize(const char *psz, unsigned long long *pullOut)
{
    unsigned long long ullVal;
    unsigned long long ullMul;
    size_t             cch;
    char               cSuffix;
    char               szNum[32];

    if (psz == NULL || *psz == '\0')
    {   /* Nothing to parse. */
        return(FALSE);
    }

    /* Measure the token and take its LAST character.  The empty-string      */
    /* check above guarantees cch >= 1, so psz[cch - 1] is in bounds.  The   */
    /* multiplier defaults to 1 and is raised below only if that last        */
    /* character is a suffix.                                                */
    cch     = strlen(psz);
    ullMul  = 1ULL;
    cSuffix = psz[cch - 1];

    /* A size may end in a binary-unit letter, as dd's bs= does.  Inspect    */
    /* the LAST character: k/K, m/M or g/G sets the 1024-power multiplier    */
    /* and is then dropped from the length (cch--), so only the digits       */
    /* reach the integer parser below.  No suffix leaves the multiplier at   */
    /* 1 and the length untouched.                                           */
    if (cSuffix == 'k' || cSuffix == 'K')
    {   /* Kibibytes. */
        ullMul = 1024ULL;
        cch--;
    }
    else if (cSuffix == 'm' || cSuffix == 'M')
    {   /* Mebibytes. */
        ullMul = 1024ULL * 1024ULL;
        cch--;
    }
    else if (cSuffix == 'g' || cSuffix == 'G')
    {   /* Gibibytes. */
        ullMul = 1024ULL * 1024ULL * 1024ULL;
        cch--;
    }

    if (cch == 0 || cch >= sizeof(szNum))
    {   /* cch==0 is a bare suffix with no number ("M"); cch>=sizeof(szNum)   */
        /* is a token too long to be a real size -- rejected HERE, before     */
        /* the copy, so the memcpy into szNum below is always within          */
        /* bounds.                                                            */
        return(FALSE);
    }

    /* Copy the numeric part (suffix excluded) into a NUL-terminated          */
    /* scratch buffer and let the shared, strict Util_ParseU64 validate and   */
    /* convert it -- one integer parser for the whole tool rather than a      */
    /* second digit loop here.                                                */
    memcpy(szNum, psz, cch);
    szNum[cch] = '\0';

    if (!Util_ParseU64(szNum, &ullVal))
    {   /* The numeric part was not a clean integer (bad digit or overflow). */
        return(FALSE);
    }

    /* Guard the multiply the same way Util_ParseU64 guards its              */
    /* accumulation: if ullVal exceeds (2^64-1)/mul the product would wrap   */
    /* to a tiny byte count, which as a block size would silently            */
    /* under-read the device -- so reject it.  (Skipped when mul==1, where   */
    /* there is no multiply to overflow.)                                    */
    if (ullMul != 1ULL && ullVal > 0xFFFFFFFFFFFFFFFFULL / ullMul)
    {   /* value * multiplier would exceed a 64-bit range. */
        return(FALSE);
    }

    /* A clean number within range, scaled by any suffix: the byte size. */
    *pullOut = ullVal * ullMul;

    /* A clean parse. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Util_WidenPath:                                                            */
/*                                                                            */
/* Convert a narrow command-line path to a wide string for the W-form Win32   */
/* calls (CreateFileW and friends).  The conversion uses the process ANSI     */
/* code page, which covers device paths and ordinary ASCII paths, and refuses */
/* a byte that page leaves undefined.  The narrow argv is the input by        */
/* design: a Windows command shell cannot type a character its code page does */
/* not hold, and one passed by a Unicode launcher (PowerShell) arrives as the */
/* C runtime's '?', which converts cleanly here and fails harmlessly at the   */
/* open as "not found" -- nothing wrong is opened.  So the refusal exists for */
/* the page's undefined bytes, and no Unicode command line is wanted.         */
/*                                                                            */
/* Arguments:                                                                 */
/*     psz : the narrow path.                                                 */
/*                                                                            */
/* Returns:                                                                   */
/*     wchar_t * : a heap buffer the caller frees with free(), or NULL on     */
/*                 failure (with the Win32 error already reported).           */
/*----------------------------------------------------------------------------*/
static wchar_t *Util_WidenPath(const char *psz)
{
    wchar_t *pwsz;
    int      cchNeeded;
    int      cchWritten;

    /* MultiByteToWideChar is used in the standard TWO-PASS way: first with   */
    /* a NULL output and 0 length to ASK how many wide chars the result       */
    /* needs (the -1 input length means "psz is NUL-terminated", so the       */
    /* count INCLUDES the terminating NUL), then again below to convert       */
    /* once a buffer that size exists.  CP_ACP is the process ANSI code       */
    /* page -- enough for device paths and plain ASCII file paths -- and      */
    /* MB_ERR_INVALID_CHARS makes a byte the code page cannot map a hard      */
    /* FAILURE rather than a silent substitution, because a path quietly      */
    /* mangled here would open the wrong device, or none.  On a single-byte   */
    /* code page almost every byte maps to something, so the flag guards      */
    /* against the page's few undefined bytes; a character outside the page   */
    /* never arrives as such (the header says what arrives instead, and why   */
    /* that is harmless).                                                     */
    cchNeeded = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, psz, -1,
                                    NULL, 0);
    if (cchNeeded <= 0)
    {   /* A byte in the path is not representable in the ANSI code page. */
        Log_WinErr("cannot convert path to Unicode", GetLastError());
        return(NULL);
    }

    pwsz = (wchar_t *)malloc((size_t)cchNeeded * sizeof(wchar_t));
    if (pwsz == NULL)
    {   /* Out of memory for the converted path. */
        Log_Err("out of memory converting path");
        return(NULL);
    }

    /* Second pass: the same call with the real buffer performs the           */
    /* conversion.  It is checked too -- not assumed to succeed just          */
    /* because the sizing pass did -- so a short write frees the buffer and   */
    /* fails rather than handing back a half-filled path.                     */
    cchWritten = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, psz, -1,
                                     pwsz, cchNeeded);
    if (cchWritten <= 0)
    {   /* Should not happen after the sizing pass, but never trust it. */
        Log_WinErr("cannot convert path to Unicode", GetLastError());
        free(pwsz);
        return(NULL);
    }

    /* The wide path; the caller frees it with free(). */
    return(pwsz);
}

/*----------------------------------------------------------------------------*/
/* Drives and volumes -- the storage-stack queries, the system disk, and the  */
/* volume walk that the write guards and the --list table rest on.            */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Drv_BusName:                                                               */
/*                                                                            */
/* The short --list label for a bus type, looked up in DRV_aBusNames; a bus   */
/* the table does not name gets DRV_BUS_OTHER rather than a number, so the    */
/* column always reads as a word and a new bus type Windows reports is never  */
/* an error.                                                                  */
/*                                                                            */
/* Arguments:                                                                 */
/*     enmBus : the value from STORAGE_DEVICE_DESCRIPTOR.BusType.             */
/*                                                                            */
/* Returns:                                                                   */
/*     const char * : the table's label, or DRV_BUS_OTHER; static text,       */
/*                    never freed.                                            */
/*----------------------------------------------------------------------------*/
static const char *Drv_BusName(STORAGE_BUS_TYPE enmBus)
{
    size_t i;

    /* The table is small and the lookup runs once per listed drive, so a       */
    /* straight walk is the right cost; the row count is derived from the       */
    /* table itself, so a row added there is found without a count to update.   */
    for (i = 0; i < sizeof(DRV_aBusNames) / sizeof(DRV_aBusNames[0]); i++)
    {   /* One table row per iteration. */
        if (DRV_aBusNames[i].enmBus == enmBus)
        {   /* The bus is in the table: its label. */
            return(DRV_aBusNames[i].pszName);
        }
    }

    /* Not a bus the table names. */
    return(DRV_BUS_OTHER);
}

/*----------------------------------------------------------------------------*/
/* Drv_DeviceGeometry:                                                        */
/*                                                                            */
/* Read the byte length and the sector size of an open storage device via     */
/* IOCTL_DISK_GET_DRIVE_GEOMETRY_EX.  The geometry query is the one chosen    */
/* because it ANSWERS ON A HANDLE OPENED WITH NO ACCESS RIGHTS AT ALL, which  */
/* is what lets --list and the pre-open probe work without Administrator;     */
/* IOCTL_DISK_GET_LENGTH_INFO, the other length query, is refused on such a   */
/* handle with access denied.  On a whole disk DiskSize is the raw            */
/* length independent of any partition table or filesystem (so a disk Windows */
/* cannot mount still shows a size); on a \\.\X: volume handle it is the      */
/* volume's own length.                                                       */
/*                                                                            */
/* Arguments:                                                                 */
/*     hDev       : an open handle to a disk, volume or partition device.     */
/*     pullLength : receives the device length in bytes on success.           */
/*     pullSector : receives the bytes per sector on success.                 */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE on success, FALSE when the device would not report (a card  */
/*           reader with no card inserted, say); the outputs are untouched.   */
/*----------------------------------------------------------------------------*/
static int Drv_DeviceGeometry(HANDLE hDev, unsigned long long *pullLength,
                              unsigned long long *pullSector)
{
    BYTE              abBuf[512];
    DISK_GEOMETRY_EX *pGeo;
    DWORD             dwRet;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL              bOk;

    /* The reply is a DISK_GEOMETRY_EX whose Data[] tail may carry partition and   */
    /* detection records beyond the fixed head, so hand it a buffer well past      */
    /* sizeof(DISK_GEOMETRY_EX) and let it write what it has.                      */
    ZeroMemory(abBuf, sizeof(abBuf));
    dwRet = 0;

    /* The query itself: no input, the reply into abBuf, its size in dwRet.   */
    /* Synchronous (no OVERLAPPED), as every control call in this file is.    */
    bOk = DeviceIoControl(hDev, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                          abBuf, sizeof(abBuf), &dwRet, NULL);
    if (!bOk || dwRet < sizeof(DISK_GEOMETRY_EX))
    {   /* No geometry, or fewer bytes than the fixed head -- nothing to trust. */
        return(FALSE);
    }

    /* Overlay the struct and take the two figures a copy needs: the whole        */
    /* length, and the sector size that every unbuffered I/O must be a multiple   */
    /* of (4096 on a native-4K disk, where an assumed 512 would fail late with    */
    /* ERROR_INVALID_PARAMETER).                                                  */
    pGeo = (DISK_GEOMETRY_EX *)abBuf;

    if (pGeo->Geometry.BytesPerSector == 0)
    {   /* A sector size of zero is not a geometry; refuse rather than   */
        /* divide by it.                                                 */
        return(FALSE);
    }

    *pullLength = (unsigned long long)pGeo->DiskSize.QuadPart;
    *pullSector = (unsigned long long)pGeo->Geometry.BytesPerSector;

    /* Both figures delivered. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Drv_DeviceNumber:                                                          */
/*                                                                            */
/* Ask an open handle WHAT IT IS via IOCTL_STORAGE_GET_DEVICE_NUMBER: the     */
/* physical disk number behind it and, for a volume or partition device, the  */
/* partition number (0 for a whole disk).  This is the identity every guard   */
/* keys on, because it is the same whatever the path was spelled as -- the    */
/* storage stack answers for \\.\PhysicalDrive0, \\.\physicaldrive0,          */
/* //./PhysicalDrive0, \\?\PhysicalDrive0, \\.\C: and \\.\HarddiskNPartitionM */
/* alike -- and it answers on a handle opened with no access rights, so the   */
/* question can be asked BEFORE anything is opened for writing.  A plain      */
/* file's handle does not support the control code, which is how a file is    */
/* told apart from a device.                                                  */
/*                                                                            */
/* Arguments:                                                                 */
/*     hDev       : an open handle to anything.                               */
/*     piDisk     : receives the physical disk number on success.             */
/*     piPartition: receives the partition number (0 = whole disk).           */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE when the handle is a storage device and both numbers were   */
/*           read; FALSE for a plain file or anything else that will not say. */
/*----------------------------------------------------------------------------*/
static int Drv_DeviceNumber(HANDLE hDev, int *piDisk, int *piPartition)
{
    STORAGE_DEVICE_NUMBER sdn;
    DWORD                 dwRet;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL                  bOk;

    /* Zero the reply and the bytes-returned count before the query. */
    ZeroMemory(&sdn, sizeof(sdn));
    dwRet = 0;

    /* The identity query: a fixed-size reply, so the struct itself is the   */
    /* buffer and a short reply is one that did not fill it.                 */
    bOk = DeviceIoControl(hDev, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                          &sdn, sizeof(sdn), &dwRet, NULL);
    if (!bOk || dwRet < sizeof(sdn))
    {   /* Not a storage device (a plain file), or one that will not   */
        /* identify itself.                                            */
        return(FALSE);
    }

    /* DeviceNumber is PhysicalDriveN's N for a disk device; PartitionNumber is   */
    /* 0 for the whole disk and the partition index for a volume on it.           */
    *piDisk      = (int)sdn.DeviceNumber;
    *piPartition = (int)sdn.PartitionNumber;

    /* A storage device, identified. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Drv_QueryModelBus:                                                         */
/*                                                                            */
/* Read a drive's model text and bus type via IOCTL_STORAGE_QUERY_PROPERTY.   */
/* Both are best-effort: on any failure the model is set to "unknown" and the */
/* bus to DRV_BUS_OTHER, so --list still prints a row.                        */
/*                                                                            */
/* Arguments:                                                                 */
/*     hDev       : an open handle to a \\.\PhysicalDriveN device.            */
/*     pszModel   : buffer receiving the NUL-terminated model string.         */
/*     cchModel   : size of pszModel in chars.                                */
/*     ppszBus    : receives a static bus-name label.                         */
/*                                                                            */
/* Returns:                                                                   */
/*     void : fills the out parameters; never fails (falls back to defaults). */
/*----------------------------------------------------------------------------*/
static void Drv_QueryModelBus(HANDLE hDev, char *pszModel, size_t cchModel,
                              const char **ppszBus)
{
    STORAGE_PROPERTY_QUERY    query;
    STORAGE_DEVICE_DESCRIPTOR *pDesc;
    BYTE                       abBuf[1024];
    DWORD                      dwRet;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL                       bOk;
    const char                *pszVendor;
    const char                *pszProduct;

    /* Set the fall-back answers FIRST, before anything that can fail, so   */
    /* every early exit below still leaves the caller a printable row       */
    /* rather than an uninitialised buffer.  --list must show a line for    */
    /* every disk even when its descriptor cannot be read, so "unknown /    */
    /* other" is a valid result here, not an error to propagate.            */
    lstrcpynA(pszModel, "unknown", (int)cchModel);
    *ppszBus = DRV_BUS_OTHER;

    /* Form the property query.  StorageDeviceProperty selects the DEVICE     */
    /* DESCRIPTOR -- the record that carries the vendor / product strings     */
    /* and the bus type -- and PropertyStandardQuery asks for that DATA, as   */
    /* opposed to PropertyExistsQuery which would only report whether the     */
    /* property is supported.  The struct is zeroed first so no stray bytes   */
    /* are passed in its AdditionalParameters tail, which the IOCTL reads     */
    /* as part of the request.                                                */
    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    /* The descriptor is VARIABLE LENGTH -- the strings sit past its fixed    */
    /* head -- so hand the IOCTL a generous 1 KB buffer and let it write as   */
    /* much as fits; dwRet comes back as the number of bytes it actually      */
    /* produced.                                                              */
    ZeroMemory(abBuf, sizeof(abBuf));
    dwRet = 0;

    /* The property query: the request struct in, the descriptor out.  A      */
    /* descriptor longer than the buffer is cut, not refused -- the strings   */
    /* past the cut are simply absent -- which the offset checks below        */
    /* tolerate because an offset is only followed when it is non-zero.       */
    bOk = DeviceIoControl(hDev, IOCTL_STORAGE_QUERY_PROPERTY,
                          &query, sizeof(query),
                          abBuf, sizeof(abBuf), &dwRet, NULL);
    if (!bOk || dwRet < sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {   /* The query failed, or returned fewer bytes than even the fixed     */
        /* header -- too little to trust the byte offsets read below -- so   */
        /* keep the "unknown / other" defaults set above and return.         */
        return;
    }

    /* The reply filled abBuf with a STORAGE_DEVICE_DESCRIPTOR; overlay the   */
    /* struct on the buffer and map its bus type to a label straight away.    */
    pDesc    = (STORAGE_DEVICE_DESCRIPTOR *)abBuf;
    *ppszBus = Drv_BusName((STORAGE_BUS_TYPE)pDesc->BusType);

    /* VendorIdOffset / ProductIdOffset are BYTE OFFSETS into this same      */
    /* buffer (0 means the field is absent), NOT pointers -- a               */
    /* descriptor-ism that is easy to misread as addresses.  Resolve each    */
    /* to a string inside abBuf, or to "" when absent, so the compose step   */
    /* below never dereferences a null.                                      */
    if (pDesc->VendorIdOffset != 0)
    {   /* The vendor string is present: it starts that many bytes into the   */
        /* reply.                                                             */
        pszVendor = (const char *)(abBuf + pDesc->VendorIdOffset);
    }
    else
    {   /* No vendor string: an empty one, so the compose below never sees NULL. */
        pszVendor = "";
    }

    if (pDesc->ProductIdOffset != 0)
    {   /* The product string is present, at its own offset into the reply. */
        pszProduct = (const char *)(abBuf + pDesc->ProductIdOffset);
    }
    else
    {   /* No product string: empty likewise. */
        pszProduct = "";
    }

    if (pszVendor[0] != '\0' || pszProduct[0] != '\0')
    {   /* At least one field is present: compose "<vendor> <product>",       */
        /* then force the terminating NUL -- _snprintf does NOT terminate     */
        /* when it truncates, so a model longer than cchModel would           */
        /* otherwise leave pszModel unterminated and the later printf would   */
        /* run off the end.                                                   */
        _snprintf(pszModel, cchModel, "%s %s", pszVendor, pszProduct);
        pszModel[cchModel - 1] = '\0';
    }
}

/*----------------------------------------------------------------------------*/
/* Drv_VolumeDiskNumbers:                                                     */
/*                                                                            */
/* For an open VOLUME handle, read which physical disk number(s) back it, via */
/* IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS.  A simple volume has one extent on   */
/* one disk; a spanned / striped volume has several.                          */
/*                                                                            */
/* Arguments:                                                                 */
/*     hVol      : an open handle to a \\.\X: volume.                         */
/*     aiDisks   : array receiving the disk numbers.                          */
/*     iDisksMax : capacity of aiDisks.                                       */
/*     piDisks   : receives how many numbers were written.                    */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE on success, FALSE if the extents could not be read.         */
/*----------------------------------------------------------------------------*/
static int Drv_VolumeDiskNumbers(HANDLE hVol, int *aiDisks, int iDisksMax,
                                 int *piDisks)
{
    BYTE                 abBuf[1024];
    VOLUME_DISK_EXTENTS *pExt;
    DWORD                dwRet;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL                 bOk;
    DWORD                i;
    int                  iOut;

    /* Zero the reply buffer and the bytes-returned count; iOut tracks how many   */
    /* disk numbers have been written into the caller's array.                    */
    ZeroMemory(abBuf, sizeof(abBuf));
    dwRet = 0;
    iOut  = 0;

    /* IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS maps a VOLUME (a drive letter)    */
    /* to the physical disk(s) whose sectors back it.  The reply is a         */
    /* variable-length VOLUME_DISK_EXTENTS -- a count followed by that many   */
    /* Extents[] -- so a fixed 1 KB buffer is handed over, big enough for     */
    /* the header plus far more extents than any real volume has.             */
    bOk = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                          abBuf, sizeof(abBuf), &dwRet, NULL);
    if (!bOk)
    {   /* Not a disk-backed volume (a network share or a subst drive, say). */
        return(FALSE);
    }

    /* Overlay the extents struct on the reply buffer: a count, then that many   */
    /* Extents[], each naming one backing disk plus its offset and length.       */
    pExt = (VOLUME_DISK_EXTENTS *)abBuf;

    for (i = 0; i < pExt->NumberOfDiskExtents; i++)
    {   /* One backing disk extent per iteration. */
        if (iOut >= iDisksMax)
        {   /* The volume spans more disks than the caller's array can hold   */
            /* -- only a striped / spanned volume would -- so take what       */
            /* fits and stop: the write guards only need to know SOME         */
            /* physical disk the volume uses.  This cap also bounds the       */
            /* READ to iDisksMax extents (well within the 1 KB buffer) even   */
            /* if NumberOfDiskExtents came back implausibly large.            */
            break;
        }

        /* Record this extent's physical-disk number, then advance. */
        aiDisks[iOut] = (int)pExt->Extents[i].DiskNumber;
        iOut++;
    }

    /* The physical-disk numbers backing this volume (iOut of them). */
    *piDisks = iOut;

    /* The extents were read, even if the array could not hold them all. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Drv_SystemDiskNumber:                                                      */
/*                                                                            */
/* Find the physical-disk number that hosts the running Windows installation, */
/* so a WRITE to it can be refused without --force.  Resolves the Windows     */
/* directory to its volume root, opens that volume, and reads its extent's    */
/* disk number.                                                               */
/*                                                                            */
/* Arguments:                                                                 */
/*     None.                                                                  */
/*                                                                            */
/* Returns:                                                                   */
/*     int : the system disk's PhysicalDrive index, or -1 if it could not be  */
/*           determined (in which case the write guard treats nothing as the  */
/*           system disk, so the caller still needs --yes).                   */
/*----------------------------------------------------------------------------*/
static int Drv_SystemDiskNumber(void)
{
    wchar_t wszWinDir[MAX_PATH];
    wchar_t wszVolDev[8];
    HANDLE  hVol;
    int     aiDisks[8];
    int     iDisks;
    UINT    uLen;

    /* Resolve the running system to its physical disk so Dd_Copy's write    */
    /* guard can refuse it without --force.  GetWindowsDirectoryW returns    */
    /* e.g. "C:\Windows"; only the drive letter is needed -- wszWinDir[0]    */
    /* holds it and wszWinDir[1] == ':' confirms a drive-rooted path.  Any   */
    /* failure here returns -1 ("unknown"), which is the SAFE degradation:   */
    /* the guard then matches no disk as the system disk and still demands   */
    /* --yes for every physical write.                                       */
    uLen = GetWindowsDirectoryW(wszWinDir, MAX_PATH);
    if (uLen == 0 || uLen >= MAX_PATH || wszWinDir[1] != L':')
    {   /* No usable Windows directory path; cannot identify the system disk. */
        return(-1);
    }

    /* Build the volume device path "\\.\C:" from that drive letter by hand, so   */
    /* there is no format string to mis-size.                                     */
    wszVolDev[0] = L'\\';
    wszVolDev[1] = L'\\';
    wszVolDev[2] = L'.';
    wszVolDev[3] = L'\\';
    wszVolDev[4] = wszWinDir[0];
    wszVolDev[5] = L':';
    wszVolDev[6] = L'\0';

    /* Open the system volume with ZERO desired access -- enough to issue the     */
    /* extents query below without any read/write right -- and full sharing, so   */
    /* merely opening the live, in-use system volume disturbs nothing.            */
    hVol = CreateFileW(wszVolDev, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE)
    {   /* Cannot open the system volume to query it; give up on detection. */
        return(-1);
    }

    /* Ask which physical disk(s) back the system volume. */
    iDisks = 0;

    if (!Drv_VolumeDiskNumbers(hVol, aiDisks, 8, &iDisks) || iDisks == 0)
    {   /* The system volume reported no backing disk; cannot identify it. */
        CloseHandle(hVol);
        return(-1);
    }

    /* Done with the volume handle (the disk numbers are already copied out). */
    CloseHandle(hVol);

    /* The first extent's disk is the one carrying \Windows -- the system   */
    /* disk the caller guards against.  (A striped system volume is         */
    /* unusual; its first extent is still a disk a raw write would wreck,   */
    /* so it is the right answer.)                                          */
    return(aiDisks[0]);
}

/*----------------------------------------------------------------------------*/
/* Drv_VolumesOnDisk:                                                         */
/*                                                                            */
/* Find EVERY volume Windows knows that sits on a given physical disk, by     */
/* walking the volume GUID namespace (FindFirstVolumeW / FindNextVolumeW)     */
/* rather than the drive letters A: to Z:.  The difference is the whole       */
/* point: a volume mounted on a folder, an EFI system partition Windows has   */
/* touched, or a volume whose letter was removed has NO letter, so a letter   */
/* walk never sees it -- and a raw write that reaches a mounted-but-unlisted  */
/* volume dies part-way with access denied, leaving the disk half written.    */
/*                                                                            */
/* Each volume is opened with no access rights and asked for its disk         */
/* extents; the ones backed by iDrive are copied out as "\\?\Volume{GUID}"    */
/* paths WITHOUT the trailing backslash the enumerator supplies, because the  */
/* trailing form names the mounted filesystem root while the bare form names  */
/* the volume device, and it is the device that can be locked and dismounted. */
/*                                                                            */
/* Arguments:                                                                 */
/*     iDrive    : the PhysicalDrive index to match.                          */
/*     awszVols  : array of DD_VOLPATH_CCH-character buffers receiving the    */
/*                 device paths of the volumes on that disk.                  */
/*     iVolsMax  : capacity of awszVols.                                      */
/*                                                                            */
/* Returns:                                                                   */
/*     int : how many volume paths were written (0 for a disk Windows has no  */
/*           volume on, or when the enumeration itself failed).               */
/*----------------------------------------------------------------------------*/
static int Drv_VolumesOnDisk(int iDrive, wchar_t awszVols[][DD_VOLPATH_CCH],
                             int iVolsMax)
{
    HANDLE  hFind;
    HANDLE  hVol;
    wchar_t wszVol[DD_VOLPATH_CCH];
    size_t  cch;
    int     aiDisks[8];
    int     iDisks;
    int     iOut;
    int     k;
    int     bOnThisDisk;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL    bMore;

    iOut = 0;

    /* Start the enumeration; it hands back the first volume's GUID path in the   */
    /* trailing-backslash form, e.g. \\?\Volume{...}\, or fails on a machine      */
    /* with no volume at all.                                                     */
    hFind = FindFirstVolumeW(wszVol, DD_VOLPATH_CCH);
    if (hFind == INVALID_HANDLE_VALUE)
    {   /* Nothing to enumerate: report no volumes on the disk. */
        return(0);
    }

    /* The first volume is already in wszVol; the loop runs until the   */
    /* enumerator says there is no next one.                            */
    bMore = TRUE;

    while (bMore)
    {   /* One volume GUID path per iteration, until FindNextVolumeW runs out. */
        if (iOut >= iVolsMax)
        {   /* The caller's array is full.  A disk carrying more volumes   */
            /* than DD_MAX_VOLUMES is not a real case; this bounds the     */
            /* array write and the lock caller warns if it ever happens.   */
            break;
        }

        /* Strip the trailing backslash so the path names the volume DEVICE. */
        cch = wcslen(wszVol);

        if (cch > 0 && wszVol[cch - 1] == L'\\')
        {   /* The enumerator's form ends in a backslash; drop it. */
            wszVol[cch - 1] = L'\0';
        }

        /* Open with ZERO desired access and full sharing: the extents    */
        /* query below needs no read or write right, so probing a live,   */
        /* in-use volume disturbs nothing and needs no elevation.         */
        hVol = CreateFileW(wszVol, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (hVol != INVALID_HANDLE_VALUE)
        {   /* The volume opened: ask which physical disk(s) back it and   */
            /* compare.                                                    */
            iDisks      = 0;
            bOnThisDisk = FALSE;

            if (Drv_VolumeDiskNumbers(hVol, aiDisks, 8, &iDisks))
            {   /* Got this volume's backing disk number(s); check whether   */
                /* ours is one.                                              */
                for (k = 0; k < iDisks; k++)
                {   /* One backing disk of this volume per iteration. */
                    if (aiDisks[k] == iDrive)
                    {   /* This volume is backed by the disk in question. */
                        bOnThisDisk = TRUE;
                        break;
                    }
                }
            }

            /* Done querying this volume; the handle served only the   */
            /* extents query.                                          */
            CloseHandle(hVol);

            if (bOnThisDisk)
            {   /* Record its device path for the caller. */
                lstrcpynW(awszVols[iOut], wszVol, DD_VOLPATH_CCH);
                iOut++;
            }
        }

        /* Advance to the next volume; FALSE ends the loop                    */
        /* (ERROR_NO_MORE_FILES, or any other failure, which is treated the   */
        /* same way -- the list so far is what there is).                     */
        bMore = FindNextVolumeW(hFind, wszVol, DD_VOLPATH_CCH);
    }

    /* Release the enumeration handle. */
    FindVolumeClose(hFind);

    /* The number of volume device paths written for this disk. */
    return(iOut);
}

/*----------------------------------------------------------------------------*/
/* Drv_LettersOnDisk:                                                         */
/*                                                                            */
/* Build a short string naming where each volume on a given physical disk is  */
/* mounted, for the --list table: a drive letter as "C:", a folder mount      */
/* point as its path, and "*" for a volume Windows knows but has mounted      */
/* nowhere -- so the column tells the truth about what sits on the disk       */
/* rather than only what has a letter.                                        */
/*                                                                            */
/* Arguments:                                                                 */
/*     iDrive    : the PhysicalDrive index to match.                          */
/*     pszOut    : buffer receiving a string like "C: D:" or "-" if none.     */
/*     cchOut    : size of pszOut in chars.                                   */
/*                                                                            */
/* Returns:                                                                   */
/*     void : fills pszOut.                                                   */
/*----------------------------------------------------------------------------*/
static void Drv_LettersOnDisk(int iDrive, char *pszOut, size_t cchOut)
{
    wchar_t  awszVols[DD_MAX_VOLUMES][DD_VOLPATH_CCH];
    wchar_t  wszDev[DD_VOLPATH_CCH + 1];
    wchar_t  wszNames[MAX_PATH];
    wchar_t *pwszList;
    wchar_t *pwszHeap;
    char     szName[MAX_PATH];
    DWORD    dwLen;
    size_t   cchName;
    size_t   cchDev;
    size_t   cchUsed;
    int      iVols;
    int      i;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL     bOk;

    /* Start the output empty and track chars written in cchUsed so every append   */
    /* below stays within cchOut.                                                  */
    pszOut[0] = '\0';
    cchUsed   = 0;

    /* Every volume on the disk, letter or not. */
    iVols = Drv_VolumesOnDisk(iDrive, awszVols, DD_MAX_VOLUMES);

    for (i = 0; i < iVols; i++)
    {   /* One volume on the disk per iteration: find where it is mounted. */

        /* GetVolumePathNamesForVolumeNameW wants the GUID path WITH its   */
        /* trailing backslash (the mounted-root form), which               */
        /* Drv_VolumesOnDisk stripped, so put it back on a copy.           */
        cchDev = wcslen(awszVols[i]);
        lstrcpynW(wszDev, awszVols[i], DD_VOLPATH_CCH + 1);
        wszDev[cchDev]     = L'\\';
        wszDev[cchDev + 1] = L'\0';

        /* The reply is a double-NUL-terminated list of mount paths ("C:\", then   */
        /* any folder mount points); only the first is shown.  The fixed buffer    */
        /* holds the common case of a letter and a few mount points; the list is   */
        /* read from it unless the call says it needs more room.                   */
        ZeroMemory(wszNames, sizeof(wszNames));
        dwLen    = 0;
        pwszList = wszNames;
        pwszHeap = NULL;
        bOk      = GetVolumePathNamesForVolumeNameW(wszDev, wszNames, MAX_PATH, &dwLen);

        if (!bOk && GetLastError() == ERROR_MORE_DATA && dwLen > MAX_PATH)
        {   /* More folder mount points than the fixed buffer holds: the call    */
            /* failed with the size it needs (in characters, both terminators    */
            /* included) in dwLen, so the list is fetched again into a buffer    */
            /* of that size and the volume still shows its first path.  An       */
            /* allocation failure leaves bOk false, and the volume then shows    */
            /* as mounted nowhere -- a mislabelled DISPLAY row, nothing worse.   */
            pwszHeap = (wchar_t *)malloc((size_t)dwLen * sizeof(wchar_t));

            if (pwszHeap != NULL)
            {   /* The second call, into the buffer sized by the first. */
                pwszHeap[0] = L'\0';
                pwszList    = pwszHeap;
                bOk         = GetVolumePathNamesForVolumeNameW(wszDev, pwszHeap, dwLen, &dwLen);
            }
        }

        if (!bOk || pwszList[0] == L'\0')
        {   /* No mount path: the volume is known to Windows but mounted   */
            /* nowhere.                                                    */
            lstrcpynA(szName, "*", (int)sizeof(szName));
        }
        else
        {   /* Convert the first mount path and drop its trailing backslash,   */
            /* so a letter shows as "C:" and a folder mount point as its       */
            /* bare path.  The conversion is not checked: this is display      */
            /* text only, and a folder name the ANSI page cannot hold shows    */
            /* the runtime's '?' rather than failing the whole row.            */
            WideCharToMultiByte(CP_ACP, 0, pwszList, -1, szName,
                                (int)sizeof(szName), NULL, NULL);
            szName[sizeof(szName) - 1] = '\0';
            cchName = strlen(szName);

            if (cchName > 1 && szName[cchName - 1] == '\\')
            {   /* Strip the root backslash. */
                szName[cchName - 1] = '\0';
            }
        }

        /* The sized list, when one was needed, is finished with (free of a   */
        /* NULL is a no-op, so the common case needs no test).                */
        free(pwszHeap);

        /* The entry's length, whichever branch made it. */
        cchName = strlen(szName);

        if (cchUsed + cchName + 2 < cchOut)
        {   /* Append only when the entry, a separating space and the NUL   */
            /* all fit; otherwise it is silently dropped, which at most     */
            /* shortens a DISPLAY string in --list -- never an overrun.     */
            if (cchUsed > 0)
            {   /* Not the first entry: separate it from the previous. */
                pszOut[cchUsed++] = ' ';
            }

            memcpy(pszOut + cchUsed, szName, cchName);
            cchUsed += cchName;
            pszOut[cchUsed] = '\0';
        }
    }

    if (pszOut[0] == '\0')
    {   /* Nothing was appended: no volume Windows knows sits on this disk    */
        /* -- a raw or foreign-formatted disk -- so write "-" as the --list   */
        /* placeholder rather than leave the column empty.                    */
        lstrcpynA(pszOut, "-", (int)cchOut);
    }
}

/*----------------------------------------------------------------------------*/
/* Drv_LockDismountVolumesOnDisk:                                             */
/*                                                                            */
/* Before writing raw sectors to a physical disk, take EVERY volume on it     */
/* offline -- lettered or not: open each, FSCTL_LOCK_VOLUME then              */
/* FSCTL_DISMOUNT_VOLUME, and KEEP the handle open so the lock holds for the  */
/* duration of the write.  The volumes come from Drv_VolumesOnDisk's GUID     */
/* walk, so a folder-mounted volume or an EFI system partition is covered;    */
/* a walk of the drive letters would miss those, and a write that reaches     */
/* one fails part-way with access denied.  On a disk carrying no filesystem   */
/* Windows can mount this finds nothing and is a no-op.                       */
/*                                                                            */
/* Arguments:                                                                 */
/*     iDrive    : the PhysicalDrive index being written.                     */
/*     ahVols    : array receiving the open, locked volume handles.           */
/*     iVolsMax  : capacity of ahVols.                                        */
/*     piVols    : receives how many handles were opened (to close later).    */
/*                                                                            */
/* Returns:                                                                   */
/*     void : opens handles into ahVols; the caller closes them after the     */
/*            copy, which releases the locks.                                 */
/*----------------------------------------------------------------------------*/
static void Drv_LockDismountVolumesOnDisk(int iDrive, HANDLE *ahVols,
                                          int iVolsMax, int *piVols)
{
    wchar_t awszVols[DD_MAX_VOLUMES][DD_VOLPATH_CCH];
    HANDLE  hVol;
    int     iVols;
    int     i;
    int     iOpen;
    DWORD   dwRet;

    /* A raw sector write to a physical disk lands UNDERNEATH whatever       */
    /* filesystem Windows has mounted on it, so before the first sector      */
    /* goes down every volume living on the target disk is locked and        */
    /* dismounted and its handle kept open for the write.  Skip this and     */
    /* the filesystem driver can flush cached metadata back over our         */
    /* sectors, or our write can corrupt a live volume -- the one hazard a   */
    /* raw writer must defend against.                                       */
    iOpen = 0;
    iVols = Drv_VolumesOnDisk(iDrive, awszVols, DD_MAX_VOLUMES);

    if (iVols >= DD_MAX_VOLUMES)
    {   /* The enumeration filled its array, so a volume beyond it may exist and   */
        /* stay mounted; say so rather than let the write proceed in silence.      */
        Log_Err("warning: disk %d has %d or more volumes; only the first %d are locked",
                iDrive, DD_MAX_VOLUMES, DD_MAX_VOLUMES);
    }

    for (i = 0; i < iVols; i++)
    {   /* One volume on the target disk per iteration. */
        if (iOpen >= iVolsMax)
        {   /* ahVols is full; bounds the array write (iVolsMax is            */
            /* DD_MAX_VOLUMES, the same size the enumeration above used, so   */
            /* this does not trip).                                           */
            break;
        }

        /* Open the volume for READ|WRITE.  The lock and dismount control   */
        /* codes below are privileged: a read-only handle fails them with   */
        /* access denied, so write access is required to take the volume    */
        /* offline even though not a single byte is written through this    */
        /* handle.                                                          */
        hVol = CreateFileW(awszVols[i], GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE)
        {   /* Cannot open this volume for locking: say which, and carry on   */
            /* -- the write may still fail on reaching it, and that error     */
            /* names itself.                                                  */
            Log_Err("warning: could not open volume %ls on disk %d to lock it",
                    awszVols[i], iDrive);
            continue;
        }

        /* The two FSCTLs below return no data; dwRet only receives the     */
        /* (ignored) bytes-returned count that DeviceIoControl requires a   */
        /* slot for.                                                        */
        dwRet = 0;

        /* Lock BEFORE dismounting, in that order deliberately.            */
        /* FSCTL_LOCK_VOLUME takes exclusive ownership and FAILS if any    */
        /* file is open on the volume -- the gentle first step, which      */
        /* refuses rather than forcing -- and then FSCTL_DISMOUNT_VOLUME   */
        /* takes the filesystem offline so its driver stops touching the   */
        /* sectors we are about to overwrite.                              */
        if (!DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0,
                             &dwRet, NULL))
        {   /* Lock failed (a file is open on the volume): WARN, do not abort.     */
            /* The operator already accepted this disk with --yes, a lock often    */
            /* fails for benign reasons, and the write may still succeed -- so     */
            /* refusing here would block a legitimate write.  The alternative,     */
            /* aborting on a lock failure, was rejected as too strict for a disk   */
            /* the user has explicitly told the tool to write.                     */
            Log_Err("warning: could not lock volume %ls on disk %d",
                    awszVols[i], iDrive);
        }

        if (!DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0,
                             &dwRet, NULL))
        {   /* Dismount failed: WARN for the same reason.  A volume left   */
            /* mounted while raw sectors are written beneath it can be     */
            /* corrupted, so name the exact volume and disk for the        */
            /* operator.                                                   */
            Log_Err("warning: could not dismount volume %ls on disk %d",
                    awszVols[i], iDrive);
        }

        /* Keep the handle OPEN.  The lock lasts only as long as the handle   */
        /* that holds it, so closing it here would let Windows remount the    */
        /* volume in the middle of the write -- the very race this function   */
        /* exists to stop.  The caller closes every handle in ahVols AFTER    */
        /* the copy, which is what finally releases the locks and lets the    */
        /* volumes remount.                                                   */
        ahVols[iOpen] = hVol;
        iOpen++;
    }

    /* How many volumes were locked -- the count of handles the caller must   */
    /* close.                                                                 */
    *piVols = iOpen;
}

/*----------------------------------------------------------------------------*/
/* The commands -- the --list table and the guarded copy with its opens.      */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* Dd_List:                                                                   */
/*                                                                            */
/* Enumerate the machine's physical drives and print one table row each:      */
/* index, byte size and GiB, sector size, bus type, whether it hosts the      */
/* system volume, the volumes on it, and the model.  This is the --list       */
/* command, whose whole purpose is to let the operator pick the right disk    */
/* before any write -- the single biggest footgun of a raw-disk tool.         */
/*                                                                            */
/* It needs NO elevation: each drive is opened with zero access rights, and   */
/* every query it then makes (geometry, descriptor, volume extents) answers   */
/* on such a handle.  A read-access open would fail for every drive from an   */
/* ordinary prompt and leave the table empty with nothing to say why -- for   */
/* the one step the manual tells the user to run first.                       */
/*                                                                            */
/* Arguments:                                                                 */
/*     None.                                                                  */
/*                                                                            */
/* Returns:                                                                   */
/*     int : DD_SUCCESS always (a drive that cannot be opened is simply not   */
/*           listed; that is not a tool failure).                             */
/*----------------------------------------------------------------------------*/
static int Dd_List(void)
{
    int                iSysDisk;
    int                i;
    int                iListed;
    wchar_t            wszDev[40];
    char               szModel[DD_MODEL_CCH];
    char               szLetters[128];
    const char        *pszBus;
    HANDLE             hDev;
    unsigned long long ullBytes;
    unsigned long long ullSector;
    double             dblGiB;

    /* Find the system disk once, so each row can flag it; -1 (unknown) flags      */
    /* nothing, which is the safe default.  iListed counts the rows printed so a   */
    /* table with none can say so.                                                 */
    iSysDisk = Drv_SystemDiskNumber();
    iListed  = 0;

    /* Print the title and the column header. */
    printf("Physical drives on this machine:\n");
    printf("  %-5s %16s %8s %6s %-6s %-6s %-10s %s\n",
           "index", "bytes", "GiB", "sector", "bus", "system", "volumes", "model");

    for (i = 0; i < DD_MAX_PHYS_DRIVE; i++)
    {   /* One PhysicalDrive index 0 .. DD_MAX_PHYS_DRIVE-1 probed per   */
        /* iteration.                                                    */

        /* Build "\\.\PhysicalDriveN" as a wide path. */
        _snwprintf(wszDev, 40, L"\\\\.\\PhysicalDrive%d", i);
        wszDev[39] = L'\0';

        /* Open with ZERO desired access and full sharing, just to query it.  A   */
        /* gap in the numbering (an index with no drive) fails to open and is     */
        /* skipped.                                                               */
        hDev = CreateFileW(wszDev, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
        if (hDev == INVALID_HANDLE_VALUE)
        {   /* No drive at this index. */
            continue;
        }

        /* Read the raw device length and sector size; show 0 when the   */
        /* device will not report them (an empty card reader, say).      */
        ullBytes  = 0;
        ullSector = 0;

        if (!Drv_DeviceGeometry(hDev, &ullBytes, &ullSector))
        {   /* Geometry unavailable: show zeros. */
            ullBytes  = 0;
            ullSector = 0;
        }

        /* Gather the rest of this disk's descriptive fields -- model + bus,   */
        /* and the volumes that sit on it -- convert the raw byte count to     */
        /* GiB for the readable column, and print the one table row.  The      */
        /* "system" column shows YES only for the disk that hosts \Windows,    */
        /* so the operator can see at a glance which disk NOT to overwrite     */
        /* -- the whole reason --list exists before any write.                 */
        Drv_QueryModelBus(hDev, szModel, DD_MODEL_CCH, &pszBus);
        Drv_LettersOnDisk(i, szLetters, sizeof(szLetters));

        /* The size in GiB (1024-based, as disk tools quote it) for the eye. */
        dblGiB = (double)ullBytes / (1024.0 * 1024.0 * 1024.0);

        /* The row, then one more drive counted as listed. */
        printf("  %-5d %16llu %8.2f %6llu %-6s %-6s %-10s %s\n",
               i, ullBytes, dblGiB, ullSector, pszBus,
               (i == iSysDisk) ? "YES" : "-",
               szLetters, szModel);
        iListed++;

        /* Done with this drive; close it before probing the next index. */
        CloseHandle(hDev);
    }

    if (iListed == 0)
    {   /* Not one index opened.  Say so, so an empty table is never   */
        /* mistaken for a machine with no disks.                       */
        printf("  (no physical drive could be opened)\n");
    }

    /* A copy-paste example so the operator has the exact imaging command form. */
    printf("\nTip: image a drive with   dd4win if=\\\\.\\PhysicalDriveN of=out.img bs=4M status=progress\n");

    /* Always success: --list is a best-effort survey, and a drive that   */
    /* could not be opened was skipped above rather than failing the      */
    /* command.                                                           */
    return(DD_SUCCESS);
}

/*----------------------------------------------------------------------------*/
/* Dd_ProbePath:                                                              */
/*                                                                            */
/* Find out what a path IS before it is opened for real: open it with no      */
/* access rights at all, ask the storage stack for its identity               */
/* (Drv_DeviceNumber) and its geometry (Drv_DeviceGeometry), and close it     */
/* again.  A path that identifies itself as a storage device is a device,     */
/* whatever it was spelled as; one that opens but will not identify itself is */
/* a plain file; one that will not open at all (an output file that does not  */
/* exist yet, or a device that is not there) is reported as not a device with */
/* the probe itself returning FALSE, and the real open decides what to do.    */
/*                                                                            */
/* Asking with no access rights is what makes this safe to do BEFORE the      */
/* guards: a target that is then refused was never opened for writing, and    */
/* it is also what lets the probe succeed without Administrator, so a refusal */
/* reads the same from any prompt.                                            */
/*                                                                            */
/* Arguments:                                                                 */
/*     pwszPath : the path, already widened.                                  */
/*     pProbe   : receives the answer (fully filled on every return).         */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE when the path opened (so pProbe->bDevice is a real answer), */
/*           FALSE when it could not be opened at all.                        */
/*----------------------------------------------------------------------------*/
static int Dd_ProbePath(const wchar_t *pwszPath, Dd_strctProbe *pProbe)
{
    HANDLE hProbe;

    /* Start from "a plain file of unknown size": every field below is only   */
    /* raised from this when the device answers.                              */
    pProbe->bDevice    = FALSE;
    pProbe->iDisk      = -1;
    pProbe->iPartition = 0;
    pProbe->ullLength  = 0;
    pProbe->ullSector  = DD_SECTOR_SIZE;

    /* Zero desired access, full sharing, no flags: enough for the two queries,   */
    /* never enough to read or write a byte through it.                           */
    hProbe = CreateFileW(pwszPath, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (hProbe == INVALID_HANDLE_VALUE)
    {   /* Nothing of that name can be opened: the caller decides what that   */
        /* means.                                                             */
        return(FALSE);
    }

    if (Drv_DeviceNumber(hProbe, &pProbe->iDisk, &pProbe->iPartition))
    {   /* It is a storage device.  Take its length and sector size too; a   */
        /* device that will not report them keeps 0 and the DD_SECTOR_SIZE   */
        /* fallback, and the copy then runs unbounded, with the alignment    */
        /* rule at that size.                                                */
        pProbe->bDevice = TRUE;
        Drv_DeviceGeometry(hProbe, &pProbe->ullLength, &pProbe->ullSector);
    }

    /* The probe handle has done its job; the real open comes later with the   */
    /* access and flags the copy needs.                                        */
    CloseHandle(hProbe);

    /* The path opened, so pProbe holds a real answer. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Dd_AllowWholeVolume:                                                       */
/*                                                                            */
/* On a VOLUME handle (a \\.\X: or partition device), lift the filesystem's   */
/* end-of-volume cut with FSCTL_ALLOW_EXTENDED_DASD_IO.  Without it a raw     */
/* read or write through a volume handle stops where the FILESYSTEM says the  */
/* volume ends, which for NTFS is one sector short of the partition -- the    */
/* backup boot sector -- so an image of \\.\C: came out one sector short and  */
/* a write-back could not reach it.  With it the handle spans the whole       */
/* partition.  A whole-disk handle does not support the code; the failure is  */
/* expected there and ignored.                                                */
/*                                                                            */
/* Arguments:                                                                 */
/*     hDev : an open handle to a device.                                     */
/*                                                                            */
/* Returns:                                                                   */
/*     void : best effort; a refusal changes nothing the caller can act on.   */
/*----------------------------------------------------------------------------*/
static void Dd_AllowWholeVolume(HANDLE hDev)
{
    DWORD dwRet;

    /* The control code takes and returns no data; dwRet is the slot          */
    /* DeviceIoControl requires.  The result is deliberately not inspected:   */
    /* a disk handle refuses it with ERROR_INVALID_FUNCTION and that is       */
    /* correct.                                                               */
    dwRet = 0;
    DeviceIoControl(hDev, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0,
                    &dwRet, NULL);
}

/*----------------------------------------------------------------------------*/
/* Dd_OpenSource:                                                             */
/*                                                                            */
/* Open the if= path for reading.  A device (as the probe found it to be, not */
/* as it was spelled) is opened unbuffered, so I/O must be sector-aligned; a  */
/* file is opened normally.                                                   */
/*                                                                            */
/* Arguments:                                                                 */
/*     pwszPath : the if= path, already widened.                              */
/*     bDevice  : TRUE when Dd_ProbePath found the path to be a device.       */
/*                                                                            */
/* Returns:                                                                   */
/*     HANDLE : the open handle, or INVALID_HANDLE_VALUE on failure (error    */
/*              already reported).                                            */
/*----------------------------------------------------------------------------*/
static HANDLE Dd_OpenSource(const wchar_t *pwszPath, int bDevice)
{
    HANDLE hSrc;
    DWORD  dwFlags;

    /* Ordinary buffered-file flags unless the probe said device. */
    dwFlags = FILE_ATTRIBUTE_NORMAL;

    if (bDevice)
    {   /* A raw device read must bypass the cache and be sector-aligned. */
        dwFlags = FILE_FLAG_NO_BUFFERING;
    }

    /* Open the input for reading.  Full sharing lets an in-use file or a device   */
    /* another handle holds still open here.                                       */
    hSrc = CreateFileW(pwszPath, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, dwFlags, NULL);
    if (hSrc == INVALID_HANDLE_VALUE)
    {   /* Could not open the input. */
        Log_WinErr("cannot open input", GetLastError());
    }

    /* The open input handle, or INVALID_HANDLE_VALUE (already reported) for the   */
    /* caller to detect and abort on.                                              */
    return(hSrc);
}

/*----------------------------------------------------------------------------*/
/* Dd_OpenDest:                                                               */
/*                                                                            */
/* Open the of= path for writing.  A device (as the probe found it) is opened */
/* unbuffered and existing, never created.  A file is opened-and-kept: the    */
/* caller truncates it at the seek point afterwards unless conv=notrunc was   */
/* given, which is what dd does -- the bytes BEFORE seek= survive, and only   */
/* what lies past the seek point is cut.  CREATE_ALWAYS would throw those     */
/* leading bytes away, so a "seek=2" write onto an existing file would come   */
/* back with its first two blocks zeroed -- hence OPEN_ALWAYS here.           */
/*                                                                            */
/* Arguments:                                                                 */
/*     pwszPath : the of= path, already widened.                              */
/*     bDevice  : TRUE when Dd_ProbePath found the path to be a device.       */
/*                                                                            */
/* Returns:                                                                   */
/*     HANDLE : the open handle, or INVALID_HANDLE_VALUE on failure (error    */
/*              already reported).                                            */
/*----------------------------------------------------------------------------*/
static HANDLE Dd_OpenDest(const wchar_t *pwszPath, int bDevice)
{
    HANDLE hDst;
    DWORD  dwFlags;
    DWORD  dwCreate;

    /* Ordinary buffered-file flags and open-or-create, unless the probe said   */
    /* device.                                                                  */
    dwFlags  = FILE_ATTRIBUTE_NORMAL;
    dwCreate = OPEN_ALWAYS;

    if (bDevice)
    {   /* Raw device write: unbuffered, and the device already exists. */
        dwFlags  = FILE_FLAG_NO_BUFFERING;
        dwCreate = OPEN_EXISTING;
    }

    /* Open (or create) the output for read+write with the disposition chosen   */
    /* above.                                                                   */
    hDst = CreateFileW(pwszPath, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       dwCreate, dwFlags, NULL);
    if (hDst == INVALID_HANDLE_VALUE)
    {   /* Could not open / create the output. */
        Log_WinErr("cannot open output", GetLastError());
    }

    /* The open output handle, or INVALID_HANDLE_VALUE (already reported)   */
    /* for the caller to detect and abort on.                               */
    return(hDst);
}

/*----------------------------------------------------------------------------*/
/* Dd_Seek:                                                                   */
/*                                                                            */
/* Move a handle's file pointer to a byte offset with SetFilePointerEx.  Used */
/* for skip= (on the source) and seek= (on the destination).  For an          */
/* unbuffered device the offset must be sector-aligned, which it is because   */
/* the block size is required to be a multiple of the device's sector size    */
/* whenever a device is involved.                                             */
/*                                                                            */
/* Arguments:                                                                 */
/*     h        : the open handle to reposition.                              */
/*     ullOff   : the absolute byte offset to seek to.                        */
/*     pszWhich : "input"/"output", for any error message.                    */
/*                                                                            */
/* Returns:                                                                   */
/*     int : TRUE on success, FALSE on failure (reported).                    */
/*----------------------------------------------------------------------------*/
static int Dd_Seek(HANDLE h, unsigned long long ullOff, const char *pszWhich)
{
    LARGE_INTEGER liMove;

    if (ullOff == 0)
    {   /* Nothing to skip/seek. */
        return(TRUE);
    }

    /* SetFilePointerEx takes a SIGNED LARGE_INTEGER; the unsigned byte offset   */
    /* fits because a real disk / file offset never approaches 2^63.             */
    liMove.QuadPart = (LONGLONG)ullOff;

    if (!SetFilePointerEx(h, liMove, NULL, FILE_BEGIN))
    {   /* The offset is past the device/file, or not aligned. */
        Log_WinErr(pszWhich, GetLastError());
        return(FALSE);
    }

    /* The pointer is now at ullOff; the caller may begin reading or writing. */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* Dd_Copy:                                                                   */
/*                                                                            */
/* The core read/write loop: copy blocks from if= to of=, honouring bs,       */
/* count, skip, seek, status=progress and conv=notrunc, with the device write */
/* guards (--yes required, --force for the system disk, and a lock/dismount   */
/* of every volume on the target).                                            */
/*                                                                            */
/* The order of the steps is the safety argument, so it is spelled out:       */
/*     1. both ends are PROBED with no access rights -- what each IS, its     */
/*        length and sector size (Dd_ProbePath);                              */
/*     2. every argument is checked against those answers -- the alignment    */
/*        rule, the skip / seek products, the device bounds -- before a byte  */
/*        moves;                                                              */
/*     3. the write guards run on the probe's identity of the destination,    */
/*        so a refused target was never opened for writing;                   */
/*     4. only then are the two ends opened for real, announced, locked, and  */
/*        copied.                                                             */
/*                                                                            */
/* Arguments:                                                                 */
/*     pCfg : the parsed configuration (if/of/bs/count/skip/seek/flags).      */
/*                                                                            */
/* Returns:                                                                   */
/*     int : DD_SUCCESS on a complete copy, DD_FAILURE on any error.          */
/*----------------------------------------------------------------------------*/
static int Dd_Copy(const Dd_strctConfig *pCfg)
{
    HANDLE             hSrc;
    HANDLE             hDst;
    wchar_t           *pwszIf;
    wchar_t           *pwszOf;
    Dd_strctProbe      src;
    Dd_strctProbe      dst;
    int                iSysDisk;
    unsigned char     *pabBuf;
    unsigned long long ullBs;
    unsigned long long ullSkipBytes;
    unsigned long long ullSeekBytes;
    unsigned long long ullRemain;
    unsigned long long ullToRead;
    unsigned long long ullTotal;
    unsigned long long ullBlocks;
    unsigned long long ullTickStart;
    unsigned long long ullTickLast;
    unsigned long long ullNow;
    HANDLE             ahVols[DD_MAX_VOLUMES];
    LARGE_INTEGER      liSize;
    int                iVols;
    int                iRc;
    int                bWarnedPad;
    int                iClose;
    char               szModel[DD_MODEL_CCH];
    const char        *pszBus;
    DWORD              dwRead;
    DWORD              dwWritten;
    DWORD              dwToWrite;
    DWORD              dwPad;
    DWORD              dwRet;
    /* Windows' own BOOL, the API's result; the tool's flags are int. */
    BOOL               bOk;
    double             dblMiB;
    double             dblSec;
    double             dblRate;

    /* Start every handle invalid, every pointer NULL, every flag and        */
    /* counter at zero, and the result pessimistic (DD_FAILURE) until the    */
    /* copy actually completes -- so every error path's `goto cleanup`       */
    /* below releases only what was taken and the function returns failure   */
    /* unless the success line was reached.                                  */
    hSrc       = INVALID_HANDLE_VALUE;
    hDst       = INVALID_HANDLE_VALUE;
    pwszIf     = NULL;
    pwszOf     = NULL;
    iSysDisk   = -1;
    pabBuf     = NULL;
    ullBs      = pCfg->ullBs;
    ullTotal   = 0;
    ullBlocks  = 0;
    iVols      = 0;
    iRc        = DD_FAILURE;
    bWarnedPad = FALSE;

    /* STEP 1: widen both paths once (they are reused by the probe and the real   */
    /* open) and probe each end with no access rights.                            */
    pwszIf = Util_WidenPath(pCfg->pszIf);
    if (pwszIf == NULL)
    {   /* The input path could not be converted; already reported. */
        goto cleanup;
    }

    pwszOf = Util_WidenPath(pCfg->pszOf);
    if (pwszOf == NULL)
    {   /* The output path could not be converted; already reported. */
        goto cleanup;
    }

    /* The input must exist to be probed; if it will not open at all the real   */
    /* open below reports why, so the probe's FALSE is not an error here.       */
    Dd_ProbePath(pwszIf, &src);

    if (!Dd_ProbePath(pwszOf, &dst) && Util_LooksLikeDevicePath(pCfg->pszOf))
    {   /* The output could not be opened AND it is spelled as a device.        */
        /* Refuse now rather than fall through to the file path, which          */
        /* would try to CREATE something of that name: a device that is not     */
        /* there is an error, never a file to make.  The error code decoded     */
        /* here is still the probe's CreateFileW's: the spelling test between   */
        /* makes no system call, so nothing has overwritten it.                 */
        Log_WinErr("cannot open output", GetLastError());
        goto cleanup;
    }

    /* STEP 2: check every argument against what the probes found.                */
    /*                                                                            */
    /* A device is opened unbuffered (FILE_FLAG_NO_BUFFERING), which requires     */
    /* every read and write to be a whole number of ITS sectors -- 512 on most    */
    /* disks, 4096 on a native-4K one -- so the block size must divide by each    */
    /* device's sector size.  Checked here, before a single byte moves, because   */
    /* the alternative is each ReadFile / WriteFile failing with                  */
    /* ERROR_INVALID_PARAMETER mid-copy -- a confusing late failure for what is   */
    /* really a bad bs= argument.                                                 */
    if (src.bDevice && (ullBs % src.ullSector) != 0)
    {   /* bs does not divide by the input device's sector size. */
        Log_Err("bs=%llu is not a multiple of the input device's %llu-byte sector",
                ullBs, src.ullSector);
        goto cleanup;
    }

    if (dst.bDevice && (ullBs % dst.ullSector) != 0)
    {   /* bs does not divide by the output device's sector size. */
        Log_Err("bs=%llu is not a multiple of the output device's %llu-byte sector",
                ullBs, dst.ullSector);
        goto cleanup;
    }

    /* skip= and seek= become byte offsets as block count times block size.   */
    /* The product is guarded the way Util_ParseSize guards its own           */
    /* multiply: a count above (2^64-1)/bs would wrap to a SMALL offset,      */
    /* and Dd_Seek treats 0 as "no seek", so an unguarded wrap would read     */
    /* from the start of the input -- the one error class this tool must      */
    /* never commit silently.                                                 */
    if (pCfg->ullSkip > 0xFFFFFFFFFFFFFFFFULL / ullBs)
    {   /* skip * bs would exceed a 64-bit byte offset. */
        Log_Err("skip=%llu blocks of %llu bytes is beyond any 64-bit offset",
                pCfg->ullSkip, ullBs);
        goto cleanup;
    }

    if (pCfg->ullSeek > 0xFFFFFFFFFFFFFFFFULL / ullBs)
    {   /* seek * bs would exceed a 64-bit byte offset. */
        Log_Err("seek=%llu blocks of %llu bytes is beyond any 64-bit offset",
                pCfg->ullSeek, ullBs);
        goto cleanup;
    }

    /* The two byte offsets, safe to compute now that neither product can wrap. */
    ullSkipBytes = pCfg->ullSkip * ullBs;
    ullSeekBytes = pCfg->ullSeek * ullBs;

    /* A device's length is known from the probe (when it reported one), so a   */
    /* skip or seek that starts past the end is refused here with a plain       */
    /* message; left alone, SetFilePointerEx can accept such an offset on a     */
    /* device and the failure arrives later as a bare read or write error.      */
    if (src.bDevice && src.ullLength != 0 && ullSkipBytes > src.ullLength)
    {   /* skip= starts beyond the input device. */
        Log_Err("skip=%llu blocks (%llu bytes) is past the end of the input device (%llu bytes)",
                pCfg->ullSkip, ullSkipBytes, src.ullLength);
        goto cleanup;
    }

    if (dst.bDevice && dst.ullLength != 0 && ullSeekBytes > dst.ullLength)
    {   /* seek= starts beyond the output device. */
        Log_Err("seek=%llu blocks (%llu bytes) is past the end of the output device (%llu bytes)",
                pCfg->ullSeek, ullSeekBytes, dst.ullLength);
        goto cleanup;
    }

    /* STEP 3: the write guards, keyed on what the destination IS.  A whole disk   */
    /* and a volume on one are guarded alike, since a raw write through either     */
    /* reaches the same sectors.                                                   */
    if (dst.bDevice)
    {   /* The destination is a storage device -- a dangerous write target. */
        iSysDisk = Drv_SystemDiskNumber();

        if (!pCfg->bYes)
        {   /* A device write needs the explicit --yes acknowledgement. */
            Log_Err("refusing to write %s (PhysicalDrive%d) without --yes (this ERASES data on that disk)",
                    pCfg->pszOf, dst.iDisk);
            goto cleanup;
        }

        if (dst.iDisk == iSysDisk && !pCfg->bForce)
        {   /* The target's disk hosts Windows: refuse unless --force is   */
            /* also given.                                                 */
            Log_Err("refusing to write the SYSTEM disk %s (PhysicalDrive%d) without --force",
                    pCfg->pszOf, dst.iDisk);
            goto cleanup;
        }
    }

    /* STEP 4: open both ends for real, with the access and flags the copy   */
    /* needs.                                                                */
    hSrc = Dd_OpenSource(pwszIf, src.bDevice);
    if (hSrc == INVALID_HANDLE_VALUE)
    {   /* Input open failed (already reported). */
        goto cleanup;
    }

    hDst = Dd_OpenDest(pwszOf, dst.bDevice);
    if (hDst == INVALID_HANDLE_VALUE)
    {   /* Output open failed (already reported). */
        goto cleanup;
    }

    /* A volume handle (a partition, not a whole disk) is cut short by its     */
    /* filesystem unless told otherwise; lift that on each side that is one.   */
    if (src.bDevice && src.iPartition != 0)
    {   /* The input is a volume: read it to the partition's true end. */
        Dd_AllowWholeVolume(hSrc);
    }

    if (dst.bDevice && dst.iPartition != 0)
    {   /* The output is a volume: let the write reach the partition's true end. */
        Dd_AllowWholeVolume(hDst);
    }

    /* For a device destination, print what was chosen and take its disk's   */
    /* volumes offline before writing a single sector.                       */
    if (dst.bDevice)
    {   /* Device target: announce it, then lock/dismount every volume on   */
        /* its disk.                                                        */
        Drv_QueryModelBus(hDst, szModel, DD_MODEL_CCH, &pszBus);

        if (dst.iPartition != 0)
        {   /* A volume on the disk: name both, so a typo of either is visible. */
            fprintf(stderr, "dd4win: writing partition %d of PhysicalDrive%d  %llu bytes  bus %s  model %s\n",
                    dst.iPartition, dst.iDisk, dst.ullLength, pszBus, szModel);
        }
        else
        {   /* The whole disk. */
            fprintf(stderr, "dd4win: writing PhysicalDrive%d  %llu bytes  bus %s  model %s\n",
                    dst.iDisk, dst.ullLength, pszBus, szModel);
        }

        Drv_LockDismountVolumesOnDisk(dst.iDisk, ahVols, DD_MAX_VOLUMES, &iVols);
    }

    /* Allocate the I/O buffer with VirtualAlloc, not malloc:                */
    /* FILE_FLAG_NO_BUFFERING requires the buffer ITSELF to be               */
    /* sector-aligned, and VirtualAlloc always returns page-aligned memory   */
    /* (a page is a whole multiple of any sector size in use), which         */
    /* satisfies that for free.  malloc gives no such guarantee, so a        */
    /* device ReadFile into a malloc'd buffer could fail with                */
    /* ERROR_INVALID_PARAMETER.                                              */
    pabBuf = (unsigned char *)VirtualAlloc(NULL, (SIZE_T)ullBs,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pabBuf == NULL)
    {   /* Could not reserve the I/O buffer. */
        Log_WinErr("cannot allocate I/O buffer", GetLastError());
        goto cleanup;
    }

    /* Apply skip= / seek= before the loop, as the byte offsets computed and   */
    /* checked above.                                                          */
    if (!Dd_Seek(hSrc, ullSkipBytes, "cannot skip on input"))
    {   /* Source seek failed. */
        goto cleanup;
    }

    if (!Dd_Seek(hDst, ullSeekBytes, "cannot seek on output"))
    {   /* Destination seek failed. */
        goto cleanup;
    }

    if (!dst.bDevice && !pCfg->bNoTrunc)
    {   /* A FILE output without conv=notrunc is cut at the seek point, as   */
        /* dd does: the bytes before seek= are kept, everything from it on   */
        /* is replaced by what the copy writes.  With seek=0 this is a       */
        /* plain truncate-to-empty, the normal imaging case.  The cut is     */
        /* made only when the file actually extends past the seek point: a   */
        /* fresh file never does, and a pseudo-file such as NUL completes    */
        /* this path too (as found: of=NUL copies without error).            */
        liSize.QuadPart = 0;

        if (!GetFileSizeEx(hDst, &liSize) ||
            (unsigned long long)liSize.QuadPart > ullSeekBytes)
        {   /* Longer than the seek point (or of unknown length): cut it there. */
            if (!SetEndOfFile(hDst))
            {   /* The file could not be cut at the seek point. */
                Log_WinErr("cannot truncate output", GetLastError());
                goto cleanup;
            }
        }
    }

    /* Timestamp the start for the progress throughput and the final summary;     */
    /* ullTickLast tracks the last progress line so the loop can throttle them.   */
    ullTickStart = GetTickCount64();
    ullTickLast  = ullTickStart;

    /* The copy loop: read a block, write exactly what was read, repeat. */
    for (;;)
    {   /* One bs-sized block read-then-written per iteration (the final block   */
        /* may be short; count=, end-of-input or the device bound breaks out     */
        /* below).                                                               */
        if (pCfg->bCount && ullBlocks >= pCfg->ullCount)
        {   /* count= blocks have been copied; stop. */
            break;
        }

        /* How much to ask for.  A FILE ends with a zero-byte read, so a         */
        /* full block is always asked for.  A raw DEVICE does not end that       */
        /* way: a read that starts before its end and runs past it is            */
        /* refused WHOLE rather than returned short, and a read that starts      */
        /* at the end is an error too (as found on real disks; the Windows       */
        /* documentation does not promise either) -- so an unbounded request     */
        /* would stop a whole-disk image whose size is not a multiple of bs      */
        /* with "read failed" one partial block short.  With the length known    */
        /* from the probe, the request is bounded to what remains and the loop   */
        /* ends when nothing does.  (The remainder is a sector multiple: the     */
        /* length is, and so are skip and every block so far.)                   */
        ullToRead = ullBs;

        if (src.bDevice && src.ullLength != 0)
        {   /* Bounded by the input device's end. */
            ullRemain = src.ullLength - ullSkipBytes - ullTotal;

            if (ullRemain == 0)
            {   /* The device's end has been reached: the copy is complete. */
                break;
            }

            if (ullRemain < ullToRead)
            {   /* The final, partial block of the device. */
                ullToRead = ullRemain;
            }
        }

        /* Read up to one block.  ullToRead is below 2^32 because bs is    */
        /* (Arg_Parse refuses a larger one), so the DWORD cast is exact.   */
        dwRead = 0;

        bOk = ReadFile(hSrc, pabBuf, (DWORD)ullToRead, &dwRead, NULL);
        if (!bOk)
        {   /* A read error ends the copy. */
            Log_WinErr("read failed", GetLastError());
            goto cleanup;
        }

        if (dwRead == 0)
        {   /* End of input: the copy is complete. */
            break;
        }

        /* Write exactly what was read, unless the device padding below   */
        /* grows it.                                                      */
        dwToWrite = dwRead;

        /* An unbuffered device write must be a whole number of the OUTPUT         */
        /* device's sectors.  The only way a short, non-aligned block reaches      */
        /* here is a FILE source whose tail is not sector-aligned; pad that tail   */
        /* with zeros up to the next sector so the device write is legal (warn     */
        /* once, since it writes a few zero bytes past the real data).  The pad    */
        /* fits in the buffer because bs is a multiple of that sector size.        */
        if (dst.bDevice && (dwToWrite % dst.ullSector) != 0)
        {   /* Round the final partial block up to a whole sector. */
            dwPad = (DWORD)(dst.ullSector - (dwToWrite % dst.ullSector));
            ZeroMemory(pabBuf + dwToWrite, dwPad);
            dwToWrite += dwPad;

            if (!bWarnedPad)
            {   /* Tell the operator the last sector was zero-padded. */
                Log_Err("warning: final block zero-padded %lu bytes to a sector boundary",
                        (unsigned long)dwPad);
                bWarnedPad = TRUE;
            }
        }

        /* Write the block -- padded length for a device, exact length for   */
        /* a file.                                                           */
        dwWritten = 0;

        bOk = WriteFile(hDst, pabBuf, dwToWrite, &dwWritten, NULL);
        if (!bOk)
        {   /* A failed write ends the copy: the target is unreliable. */
            Log_WinErr("write failed", GetLastError());
            goto cleanup;
        }

        if (dwWritten != dwToWrite)
        {   /* A SHORT write with no error ends the copy too, reported in     */
            /* its own words with both counts -- there is no Win32 error to   */
            /* decode here, and decoding the stale last error would print     */
            /* "error 0: The operation completed successfully" for a write    */
            /* that had not.                                                  */
            Log_Err("short write: %lu of %lu bytes written", (unsigned long)dwWritten,
                    (unsigned long)dwToWrite);
            goto cleanup;
        }

        /* Count the REAL bytes read, not the padding, toward the total. */
        ullTotal += dwRead;
        ullBlocks++;

        if (pCfg->bProgress)
        {   /* Progress is enabled: throttle printed lines to one per interval. */
            ullNow = GetTickCount64();

            if ((ullNow - ullTickLast) >= DD_PROGRESS_MS)
            {   /* At least one interval has elapsed since the last report. */
                dblMiB = (double)ullTotal / (1024.0 * 1024.0);
                dblSec = (double)(ullNow - ullTickStart) / 1000.0;

                if (dblSec > 0.0)
                {   /* Time has passed: the rate is the plain quotient. */
                    dblRate = dblMiB / dblSec;
                }
                else
                {   /* A guard against dividing by zero.  It cannot fire    */
                    /* while the progress interval is a whole second (the   */
                    /* first report already has at least that much time     */
                    /* behind it); it stands so a shorter interval could    */
                    /* not make the first line a NaN.                       */
                    dblRate = 0.0;
                }

                fprintf(stderr, "\r%llu bytes  %.1f MiB  %.1f s  %.1f MiB/s   ",
                        ullTotal, dblMiB, dblSec, dblRate);
                fflush(stderr);
                ullTickLast = ullNow;
            }
        }
    }

    if (pCfg->bProgress)
    {   /* End the progress line with a newline so the summary is on its own. */
        fputc('\n', stderr);
    }

    /* Push any buffered output to the destination before declaring success. */
    if (!FlushFileBuffers(hDst))
    {   /* The flush failed. */
        if (dst.bDevice)
        {   /* On a DEVICE this is the one signal that the data may not be   */
            /* on the media, so it is fatal: a script checking the exit      */
            /* code must not see success for an image that may not have      */
            /* landed.                                                       */
            Log_WinErr("flush failed", GetLastError());
            goto cleanup;
        }

        /* On a file the data is in the system cache and will reach the disk;   */
        /* report it, but the copy itself is complete.                          */
        Log_WinErr("warning: flush failed", GetLastError());
    }

    if (dst.bDevice && dst.iPartition == 0)
    {   /* A whole disk was written, possibly with a new partition table.   */
        /* Ask Windows to re-read it now, so Explorer and Disk Management   */
        /* show the new layout without the card being re-seated.  Best      */
        /* effort: the data is on the disk whether or not Windows notices   */
        /* yet.                                                             */
        dwRet = 0;

        if (!DeviceIoControl(hDst, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0,
                             &dwRet, NULL))
        {   /* Windows declined to re-read; say so, since the stale layout   */
            /* can confuse.                                                  */
            Log_WinErr("warning: could not refresh the partition table", GetLastError());
        }
    }

    /* The final result line (dd-style), to stderr. */
    dblSec = (double)(GetTickCount64() - ullTickStart) / 1000.0;
    fprintf(stderr, "dd4win: %llu bytes (%llu blocks of %llu) copied in %.1f s\n",
            ullTotal, ullBlocks, ullBs, dblSec);

    /* The copy ran to completion: record success.  The cleanup below runs on      */
    /* this path and on every goto-cleanup error path alike, so it is the single   */
    /* place handles and the buffer are released.                                  */
    iRc = DD_SUCCESS;

cleanup:
    /* Release the buffer. */
    if (pabBuf != NULL)
    {   /* Free the page-aligned I/O buffer. */
        VirtualFree(pabBuf, 0, MEM_RELEASE);
    }

    /* Close the copy handles. */
    if (hSrc != INVALID_HANDLE_VALUE)
    {   /* Close the input. */
        CloseHandle(hSrc);
    }

    if (hDst != INVALID_HANDLE_VALUE)
    {   /* Close the output. */
        CloseHandle(hDst);
    }

    /* Release any volume locks last, remounting the volumes we took offline. */
    for (iClose = 0; iClose < iVols; iClose++)
    {   /* Closing the handle drops the lock and lets Windows remount it. */
        CloseHandle(ahVols[iClose]);
    }

    /* Free the widened paths (free(NULL) is a no-op for one never made). */
    free(pwszIf);
    free(pwszOf);

    /* DD_SUCCESS only if the copy ran to completion above; DD_FAILURE for any   */
    /* error path, each of which jumped here so this same cleanup still ran.     */
    return(iRc);
}

/*----------------------------------------------------------------------------*/
/* The command line -- the usage text and the parser, then main.              */
/*----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------*/
/* Arg_Usage:                                                                  */
/*                                                                             */
/* Print the one usage/help block to stderr.  Shown on --help, and on every    */
/* usage error (no arguments, an unknown token, a copy missing if= or of=),    */
/* so stderr is deliberate: the help reads fine while stdout is an image file. */
/* The command-line surface it documents:                                      */
/*                                                                             */
/*     if=<src>        read from this file or a \\.\device                     */
/*     of=<dst>        write to this file or a \\.\device                      */
/*     bs=N            block size in bytes; suffix k/M/G (default 4M)          */
/*     count=N         copy at most N blocks (default: to end of input)        */
/*     skip=N          skip N input blocks before the first read               */
/*     seek=N          seek N output blocks before the first write             */
/*     status=progress print a throughput line to stderr while copying         */
/*     conv=notrunc    do not cut an existing output FILE at the seek point    */
/*     --yes           REQUIRED to write a disk or volume device               */
/*     --force         additionally REQUIRED to write the system disk          */
/*     --list          list the physical drives and exit                       */
/*     --help / -h     print this block                                        */
/*     --version       print the version and exit                              */
/*                                                                             */
/* Arguments:                                                                  */
/*     None.                                                                   */
/*                                                                             */
/* Returns:                                                                    */
/*     void : writes the usage block to stderr.                                */
/*-----------------------------------------------------------------------------*/
static void Arg_Usage(void)
{
    /* One fprintf of the whole block, to stderr for the reason the header gives   */
    /* -- the help stays readable while stdout is redirected to an image file.     */
    /* The worked examples are part of the help on purpose: the dd=-style token    */
    /* shape is unusual enough that a backup / carve / write-back line is the      */
    /* fastest way for a reader to get the if=/of=/skip=/seek= ordering right.     */
    fprintf(stderr,
        "dd4win " DD_VERSION " -- raw block copy for Windows disks and files\n"
        "\n"
        "Usage:\n"
        "  dd4win --list\n"
        "  dd4win if=<src> of=<dst> [bs=N] [count=N] [skip=N] [seek=N]\n"
        "         [status=progress] [conv=notrunc] [--yes] [--force]\n"
        "\n"
        "  <src>/<dst> : a file path, or a device \\\\.\\PhysicalDriveN / \\\\.\\X:\n"
        "  bs=N        : block size in bytes; suffix k/M/G (default 4M, below 4G).\n"
        "                Must be a multiple of the sector size when a device is involved.\n"
        "  count=N     : copy at most N blocks (default: until end of input).\n"
        "  skip=N      : skip N blocks at the start of the input.\n"
        "  seek=N      : seek N blocks into the output before writing.\n"
        "  status=progress : print progress to stderr while copying.\n"
        "  conv=notrunc    : do not cut an existing output FILE at the seek point.\n"
        "  --yes       : REQUIRED to write a disk or volume device (it ERASES data).\n"
        "  --force     : additionally REQUIRED to write the system disk.\n"
        "  --list      : list physical drives (index, size, bus, volumes) and exit.\n"
        "  --help      : this text.   --version : print the version.\n"
        "\n"
        "Examples:\n"
        "  Back up a disk to a file:\n"
        "    dd4win if=\\\\.\\PhysicalDrive2 of=disk.img bs=4M status=progress\n"
        "  Carve a sector range out of an image:\n"
        "    dd4win if=disk.img of=part.img bs=512 skip=<START> count=<COUNT>\n"
        "  Write a range back to a disk (dangerous):\n"
        "    dd4win if=part.img of=\\\\.\\PhysicalDrive2 bs=512 seek=<START> conv=notrunc --yes\n");
}

/*------------------------------------------------------------------------------*/
/* Arg_Parse:                                                                   */
/*                                                                              */
/* Parse the command line into a Dd_strctConfig.  dd-style key=value tokens and */
/* --flags are both accepted; an unknown token is an error.                     */
/*                                                                              */
/* Arguments:                                                                   */
/*     argc : argument count from main.                                         */
/*     argv : argument vector from main.                                        */
/*     pCfg : the configuration to fill (zeroed by the caller first).           */
/*                                                                              */
/* Returns:                                                                     */
/*     int : TRUE on a clean parse, FALSE on an unknown or malformed token.     */
/*------------------------------------------------------------------------------*/
static int Arg_Parse(int argc, char **argv, Dd_strctConfig *pCfg)
{
    int         i;
    const char *pszArg;

    /* Defaults the zeroed struct does not already carry. */
    pCfg->ullBs = DD_DEFAULT_BS;

    for (i = 1; i < argc; i++)
    {   /* One command-line token per iteration (argv[0], the program name, is   */
        /* skipped by starting at 1).                                            */
        pszArg = argv[i];

        if (strncmp(pszArg, "if=", 3) == 0)
        {   /* Input path. */
            pCfg->pszIf = pszArg + 3;
        }
        else if (strncmp(pszArg, "of=", 3) == 0)
        {   /* Output path. */
            pCfg->pszOf = pszArg + 3;
        }
        else if (strncmp(pszArg, "bs=", 3) == 0)
        {   /* Block size with optional k/M/G suffix. */
            if (!Util_ParseSize(pszArg + 3, &pCfg->ullBs) || pCfg->ullBs == 0)
            {   /* Bad or zero block size. */
                Log_Err("invalid bs= value: %s", pszArg + 3);
                return(FALSE);
            }

            if (pCfg->ullBs >= DD_MAX_BS)
            {   /* ReadFile and WriteFile take a 32-bit length, so a bs of   */
                /* 4 GiB or more cannot be issued as one I/O; the DWORD      */
                /* cast would TRUNCATE it (bs=4G would become a 0-byte       */
                /* read, an instant "end of input", and a zero-length copy   */
                /* reporting success).  Refuse it here.                      */
                Log_Err("bs=%llu is too large: bs must be below 4 GiB", pCfg->ullBs);
                return(FALSE);
            }
        }
        else if (strncmp(pszArg, "count=", 6) == 0)
        {   /* Maximum block count. */
            if (!Util_ParseU64(pszArg + 6, &pCfg->ullCount))
            {   /* Not a clean integer. */
                Log_Err("invalid count= value: %s", pszArg + 6);
                return(FALSE);
            }

            pCfg->bCount = TRUE;
        }
        else if (strncmp(pszArg, "skip=", 5) == 0)
        {   /* Input blocks to skip. */
            if (!Util_ParseU64(pszArg + 5, &pCfg->ullSkip))
            {   /* Not a clean integer. */
                Log_Err("invalid skip= value: %s", pszArg + 5);
                return(FALSE);
            }
        }
        else if (strncmp(pszArg, "seek=", 5) == 0)
        {   /* Output blocks to seek. */
            if (!Util_ParseU64(pszArg + 5, &pCfg->ullSeek))
            {   /* Not a clean integer. */
                Log_Err("invalid seek= value: %s", pszArg + 5);
                return(FALSE);
            }
        }
        else if (strcmp(pszArg, "status=progress") == 0)
        {   /* Progress reporting. */
            pCfg->bProgress = TRUE;
        }
        else if (strcmp(pszArg, "conv=notrunc") == 0)
        {   /* Do not truncate an output file. */
            pCfg->bNoTrunc = TRUE;
        }
        else if (strcmp(pszArg, "--yes") == 0)
        {   /* Permit a physical-device write. */
            pCfg->bYes = TRUE;
        }
        else if (strcmp(pszArg, "--force") == 0)
        {   /* Permit writing the system disk. */
            pCfg->bForce = TRUE;
        }
        else if (strcmp(pszArg, "--list") == 0)
        {   /* List drives and exit. */
            pCfg->bList = TRUE;
        }
        else if (strcmp(pszArg, "--help") == 0 || strcmp(pszArg, "-h") == 0)
        {   /* Usage and exit. */
            pCfg->bHelp = TRUE;
        }
        else if (strcmp(pszArg, "--version") == 0)
        {   /* Version and exit. */
            pCfg->bVersion = TRUE;
        }
        else
        {   /* Anything else is a typo or an unsupported option. */
            Log_Err("unknown argument: %s", pszArg);
            return(FALSE);
        }
    }

    /* Every token parsed cleanly; pCfg is now fully populated for main to   */
    /* act on.                                                               */
    return(TRUE);
}

/*----------------------------------------------------------------------------*/
/* main:                                                                      */
/*                                                                            */
/* Parse the command line and dispatch: --help / --version / --list run and   */
/* exit; otherwise if= and of= are required and the copy runs.                */
/*                                                                            */
/* Arguments:                                                                 */
/*     argc : argument count.                                                 */
/*     argv : argument vector (narrow; device and file paths are converted    */
/*            to wide once, in Dd_Copy, before they are probed and opened).   */
/*                                                                            */
/* Returns:                                                                   */
/*     int : DD_SUCCESS (0) on success, DD_FAILURE (1) on any error.          */
/*----------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    Dd_strctConfig cfg;

    /* Zero the whole config so every flag starts FALSE and every count 0;   */
    /* Arg_Parse then sets ullBs's non-zero default and whatever the         */
    /* command line overrides.                                               */
    ZeroMemory(&cfg, sizeof(cfg));

    if (argc < 2)
    {   /* No arguments: show usage and fail, so a bare run is not silent. */
        Arg_Usage();
        return(DD_FAILURE);
    }

    if (!Arg_Parse(argc, argv, &cfg))
    {   /* A bad argument was already reported; show usage and fail. */
        Arg_Usage();
        return(DD_FAILURE);
    }

    if (cfg.bHelp)
    {   /* --help: usage to stderr, success. */
        Arg_Usage();
        return(DD_SUCCESS);
    }

    if (cfg.bVersion)
    {   /* --version: the bare version to stdout. */
        printf("dd4win %s\n", DD_VERSION);
        return(DD_SUCCESS);
    }

    if (cfg.bList)
    {   /* --list: enumerate drives and exit. */
        return(Dd_List());
    }

    if (cfg.pszIf == NULL || cfg.pszOf == NULL)
    {   /* A copy needs both ends named. */
        Log_Err("both if= and of= are required for a copy");
        Arg_Usage();
        return(DD_FAILURE);
    }

    /* Run the copy and return its result code. */
    return(Dd_Copy(&cfg));
}
