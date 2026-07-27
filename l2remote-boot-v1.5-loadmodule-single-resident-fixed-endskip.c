/*
 * l2remote.boot 1.5 - standalone LoadModule-compatible RDB bootstrap
 *
 * Purpose:
 *   After LoadModule has installed scsi.device and ptable.library, this
 *   COLDSTART resident opens ptable.library and calls:
 *
 *       BootScanRDB("scsi.device", 0)
 *
 * It is intentionally a separate single-Resident module.  rt_EndSkip points
 * immediately after its own Resident structure, matching the normalization
 * performed by Remus and the proven LoadModule-compatible scsi.device v43.22.
 *
 * Suggested loading order:
 *   LoadModule ptable.library scsi.device l2remote.boot
 *   Reboot
 *
 * Build:
 *   m68k-amigaos-gcc \
 *     -I/opt/amiga/m68k-amigaos/ndk-include \
 *     -Os -noixemul -nostartfiles -nostdlib -fno-builtin \
 *     -fomit-frame-pointer -fno-strict-aliasing \
 *     -ffunction-sections -fdata-sections -Wl,--gc-sections -s \
 *     -o l2remote.boot \
 *     l2remote-boot-v1.5-loadmodule-single-resident-fixed-endskip.c
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <proto/exec.h>

#ifndef L2_USED
#define L2_USED __attribute__((used))
#endif

#define L2BOOT_NAME       "l2remote.boot"
#define L2BOOT_VERSION    1
#define L2BOOT_REVISION   5
#define L2BOOT_PRIORITY   9
#define L2BOOT_VERSTRING  "$VER: l2remote.boot 1.5 (27.07.2026) standalone LoadModule RDB bootstrap fixed EndSkip"

#define L2SCSI_NAME       "scsi.device"
#define PTABLE_NAME       "ptable.library"
#define PTABLE_MINVER     1

static char l2boot_name[] = L2BOOT_NAME;
static char l2boot_verstring[] = L2BOOT_VERSTRING;
static char l2boot_ptable_name[] = PTABLE_NAME;
static char l2boot_device_name[] = L2SCSI_NAME;

/* Direct Exec calls keep this resident independent of startup code/libc. */
static void l2boot_Forbid(struct ExecBase *sys)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-132:W)" : : "r"(a6)
                      : "d0", "d1", "a0", "a1", "cc", "memory");
}

static void l2boot_Permit(struct ExecBase *sys)
{
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-138:W)" : : "r"(a6)
                      : "d0", "d1", "a0", "a1", "cc", "memory");
}

static struct Node *l2boot_FindName(struct ExecBase *sys,
                                    struct List *list,
                                    const char *name)
{
    register struct Node *d0 __asm("d0");
    register struct List *a0 __asm("a0") = list;
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-276:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a0), "r"(a1)
        : "d1", "cc", "memory");
    return d0;
}

static struct Resident *l2boot_FindResident(struct ExecBase *sys,
                                             const char *name)
{
    register struct Resident *d0 __asm("d0");
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-96:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static APTR l2boot_InitResident(struct ExecBase *sys,
                                struct Resident *resident)
{
    register APTR d0 __asm("d0");
    register APTR a0 __asm("a0") = (APTR)0;
    register struct Resident *a1 __asm("a1") = resident;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-102:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a0), "r"(a1)
        : "d1", "cc", "memory");
    return d0;
}

static struct Library *l2boot_OpenLibrary(struct ExecBase *sys,
                                           const char *name,
                                           ULONG version)
{
    register struct Library *d0 __asm("d0");
    register ULONG rd0 __asm("d0") = version;
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-552:W)"
        : "=r"(d0)
        : "r"(a6), "r"(rd0), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static void l2boot_CloseLibrary(struct ExecBase *sys, struct Library *lib)
{
    register struct Library *a1 __asm("a1") = lib;
    register struct ExecBase *a6 __asm("a6") = sys;
    __asm__ volatile ("jsr a6@(-414:W)"
        :
        : "r"(a6), "r"(a1)
        : "d0", "d1", "a0", "cc", "memory");
}

/* ptable.library BootScanRDB() is LVO -30 in the proven integration. */
static LONG l2boot_BootScanRDB(struct Library *ptable,
                               const char *device_name,
                               ULONG unit)
{
    register LONG d0 __asm("d0") = (LONG)unit;
    register const char *a1 __asm("a1") = device_name;
    register struct Library *a6 __asm("a6") = ptable;
    __asm__ volatile ("jsr a6@(-30:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

/*
 * COLDSTART entry.  Return value is intentionally ignored by Exec.
 *
 * The device must already have initialized at its higher Resident priority
 * (scsi.device uses priority 21; this module uses 9).  We still check the live
 * DeviceList so a missing/failed device never causes ptable.library to scan a
 * nonexistent target.
 */
L2_USED static LONG l2boot_entry(register struct ExecBase *sysbase __asm("a6"))
{
    struct Node *device_node;
    struct Resident *ptable_resident;
    struct Library *ptable;

    if (!sysbase)
        sysbase = *(struct ExecBase **)4;
    if (!sysbase)
        return 0;

    l2boot_Forbid(sysbase);
    device_node = l2boot_FindName(sysbase,
                                  &sysbase->DeviceList,
                                  l2boot_device_name);
    l2boot_Permit(sysbase);

    if (!device_node)
        return 0;

    ptable = l2boot_OpenLibrary(sysbase,
                                l2boot_ptable_name,
                                PTABLE_MINVER);
    if (!ptable) {
        /* LoadModule may have preserved ptable.library as a Resident without
         * having initialized it yet.  Initialize that Resident explicitly. */
        ptable_resident = l2boot_FindResident(sysbase,
                                              l2boot_ptable_name);
        if (!ptable_resident)
            return 0;

        (void)l2boot_InitResident(sysbase, ptable_resident);
        ptable = l2boot_OpenLibrary(sysbase,
                                    l2boot_ptable_name,
                                    PTABLE_MINVER);
        if (!ptable)
            return 0;
    }

    /* Keep the library resident across CloseLibrary, matching the already
     * hardware-proven embedded bootstrap behavior. */
    ptable->lib_OpenCnt++;

    (void)l2boot_BootScanRDB(ptable, l2boot_device_name, 0);

    l2boot_CloseLibrary(sysbase, ptable);
    return 0;
}

extern struct Resident L2BootROMTag;

L2_USED struct Resident L2BootROMTag = {
    RTC_MATCHWORD,
    &L2BootROMTag,
    (APTR)(&L2BootROMTag + 1),
    RTF_COLDSTART,
    L2BOOT_VERSION,
    NT_UNKNOWN,
    L2BOOT_PRIORITY,
    l2boot_name,
    l2boot_verstring,
    (APTR)l2boot_entry
};
