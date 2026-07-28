/*
 * v43.21 WRITE1024:
 * - WRITE requests use two contiguous 512-byte blocks per WRITE_REQ whenever
 *   at least two blocks remain in the current caller request
 * - an odd final block is sent with the proven WRITE512 path
 * - retry, ACK matching, bounds checks, io_Actual accounting, TD_FORMAT,
 *   flush handling and read-cache invalidation are otherwise unchanged
 * - rebuilt from the hardware-verified v43.19 + TD_FORMAT baseline
 */

/*
 * v43.14 diagnostic repair:
 * - persistent WRITE TX frame in device-base RAM, not worker stack
 * - one 512-byte block per WRITE_REQ (no DATA1024 WRITE yet)
 */

/*
 * scsi.device v43.0 - L2 remote unit 0 / ptable stage 1
 *
 * Direct, minimal conversion of the certified l2scsi.device 42.56 baseline.
 * Exposes the remote Pi HDF as l2scsi.device unit 0. Units 1+ fail to open.
 * Read-only; transport and timing are otherwise unchanged from 42.56.
 * Intended first test: ptable.library / compactflash.boot scan of an RDB HDF.
 *
 * Source baseline: l2scsi-device-v42r56-rtt-fast-rx-postack-poll128-data1024.c
 */

/*
 * l2scsi.device v42r29-entry-stage
 *
 * Refactor target: A4091-style Exec device discipline.
 * BeginIO is now a strict enqueue-only path: no backend OpenDevice,
 * no IDENT, no READ_REQ, no polling/retry. The worker task owns all
 * potentially blocking transport operations and replies when complete.
 *
 * Derived from previous v40 resident backend/vncpump test source.
 *
 * Original embedded diagnostic header follows.
 *
 * l2boot-3c589-cardres-probe-v33-hdf-sink.c
 *
 * Probe EtherLink III PCMCIA through Amiga card.resource only.
 * No pccard.library developer headers required.
 *
 * This is NOT the final boot driver. It is a diagnostic to find the real
 * configured PCMCIA I/O base and verify that the 3C589 registers respond.
 *
 * Compile:
 *   m68k-amigaos-gcc -I/root/backend/include -I/opt/amiga/m68k-amigaos/ndk-include \
 *     -noixemul -Os -s -o l2boot-3c589-cardres-probe-v33-hdf-sink l2boot-3c589-cardres-probe-v33-hdf-sink.c -lamiga
 *
 * Usage:
 *   l2boot-3c589-cardres-probe-v33-hdf-sink rx [seconds] [yield_ticks]
 *
 *   seconds: omitted or 0 = listens indefinitely until Ctrl-C.
 *   yield_ticks: omitted = 1; 0 = brutal no-Delay mode; 1 = v29 behavior.
 *
 * Pi tcpdump for TX test:
 *   sudo tcpdump -i eth0 -e -XX ether proto 0x88b8
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/interrupts.h>
#include <dos/dos.h>
#include <resources/card.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* -------------------------------------------------------------------------
 * ROM/Remus attempt r3: do not pull generic C-library string/stdlib helpers.
 * A resident device should not need libc state.  Some Amiga cross-libc builds
 * can introduce a tiny HUNK_BSS even when the device source itself is clean.
 * These local helpers are deliberately small and BSS-free.
 * ------------------------------------------------------------------------- */
static void *l2_rom_memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

static void *l2_rom_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static unsigned long l2_rom_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

static char *l2_rom_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) { }
    return dst;
}

static int l2_rom_strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static unsigned long l2_rom_strtoul(const char *s, char **endp, int base)
{
    unsigned long v = 0;
    int any = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int d;
        char ch = *s;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'Z') d = ch - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        s++;
        any = 1;
    }
    if (endp) *endp = (char *)(any ? s : s);
    return v;
}

static int l2_rom_atoi(const char *s)
{
    int neg = 0;
    unsigned long v;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    v = l2_rom_strtoul(s, 0, 10);
    return neg ? -(int)v : (int)v;
}

#define memset  l2_rom_memset
#define memcpy  l2_rom_memcpy
#define strlen  l2_rom_strlen
#define strcpy  l2_rom_strcpy
#define strcmp  l2_rom_strcmp
#define strtoul l2_rom_strtoul
#define atoi    l2_rom_atoi


#include <exec/resident.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#ifndef IOERR_OPENFAIL
#define IOERR_OPENFAIL    (-1)
#endif
#ifndef IOERR_ABORTED
#define IOERR_ABORTED     (-2)
#endif
#ifndef IOERR_NOCMD
#define IOERR_NOCMD       (-3)
#endif
#ifndef IOERR_BADLENGTH
#define IOERR_BADLENGTH   (-4)
#endif
#ifndef IOERR_BADADDRESS
#define IOERR_BADADDRESS  (-5)
#endif
#ifndef IOERR_UNITBUSY
#define IOERR_UNITBUSY    (-6)
#endif
#ifndef IOERR_SELFTEST
#define IOERR_SELFTEST    (-7)
#endif
#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY 0x4000
#endif
#ifndef NSCMD_TD_READ64
#define NSCMD_TD_READ64   0xc000
#endif
#ifndef NSCMD_TD_WRITE64
#define NSCMD_TD_WRITE64  0xc001
#endif
#ifndef HD_SCSICMD
#define HD_SCSICMD        0x1c
#endif
#ifndef HFERR_BadStatus
#define HFERR_BadStatus 45
#endif
#ifndef DG_DIRECT_ACCESS
#define DG_DIRECT_ACCESS 0
#endif
#ifndef DGF_REMOVABLE
#define DGF_REMOVABLE 1
#endif
#ifndef __saveds
#define __saveds
#endif



#ifndef L2_USED
#define L2_USED __attribute__((used))
#endif

#ifndef CARDRESNAME
#define CARDRESNAME "card.resource"
#endif

/* Tuple codes: enough for diagnostics. */
#define CISTPL_MANFID       0x20
#define CISTPL_CONFIG       0x1a
#define CISTPL_CFTABLE      0x1b

/* Config register offsets in attribute memory. */
#ifndef PCCARD_REG_COR
#define PCCARD_REG_COR      0x00
#endif
#ifndef PCCARD_REG_CCSR
#define PCCARD_REG_CCSR     0x02
#endif
#ifndef PCCARD_REG_CCSRF_AUDIOENABLE
#define PCCARD_REG_CCSRF_AUDIOENABLE 0x08
#endif

/* 3Com regs/commands, from direct 3C589 backend source. */
#define EL3REG_DATA0        0
#define EL3REG_RXSTATUS     8
#define EL3REG_TXSTATUS     11
#define EL3REG_TXSPACE      12
#define EL3REG_COMMAND      14
#define EL3REG_STATUS       14
#define EL3REG_CONFIG       4
#define EL3REG_ADDRCONFIG   6
#define EL3REG_RESCONFIG    8
#define EL3REG_EEPROMCMD    10
#define EL3REG_EEPROMDATA   12
#define EL3REG_MEDIA        10

#define EL3CMD_SELECTWINDOW (1<<11)
#define EL3CMD_RXDISABLE    (3<<11)
#define EL3CMD_RXENABLE     (4<<11)
#define EL3CMD_RXRESET      (5<<11)
#define EL3CMD_RXDISCARD    (8<<11)
#define EL3CMD_TXENABLE     (9<<11)
#define EL3CMD_TXDISABLE    (10<<11)
#define EL3CMD_TXRESET      (11<<11)
#define EL3CMD_ACKINT       (13<<11)
#define EL3CMD_SETINTMASK   (14<<11)
#define EL3CMD_SETZEROMASK  (15<<11)
#define EL3CMD_SETRXFILTER  (16<<11)

#define EL3INTF_ANY         0x0001
#define EL3INTF_FAILURE     0x0002
#define EL3INTF_TXCOMPLETE  0x0004
#define EL3INTF_TXAVAIL     0x0008
#define EL3INTF_RXCOMPLETE  0x0010
#define INT_MASK (EL3INTF_RXCOMPLETE|EL3INTF_TXAVAIL|EL3INTF_TXCOMPLETE)

#define EL3REG_STATUSF_CMDINPROGRESS 0x1000
#define EL3REG_RXSTATUSF_INCOMPLETE  0x8000
#define EL3REG_RXSTATUSF_ERROR       0x4000
#define EL3REG_RXSTATUSF_LENMASK     0x07ff
#define EL3REG_CONFIGF_HASTP         (1<<9)
#define EL3REG_ADDRCONFIG_XCVRMASK   0xc000
#define EL3REG_ADDRCONFIG_XCVRTP     (0<<14)
#define EL3REG_ADDRCONFIG_XCVRAUI    (1<<14)
#define EL3REG_ADDRCONFIG_XCVRCOAX   (3<<14)
#define EL3REG_CONFIGF_HASTP         (1<<9)
#define EL3REG_CONFIGF_HASAUI        (1<<13)
#define EL3REG_CONFIGF_HASCOAX       (1<<12)
#define EL3REG_MEDIAF_BEAT           0x0800
#define EL3REG_MEDIAF_BEATENABLE     0x0080
#define EL3REG_MEDIAF_JABBERENABLE   0x0040

#define ETHERTYPE_L2BOOT 0x88B8
#define L2HD_MAGIC          0x4c324844UL
#define L2HD_VERSION        1
#define L2HD_TYPE_DATA      4
#define L2HD_TYPE_DATA_ACK  5
#define L2HD_TYPE_READ_REQ  6
#define L2HD_TYPE_WRITE_REQ 10
#define L2HD_TYPE_WRITE_ACK 11
#define L2HD_TYPE_FLUSH_REQ 12
#define L2HD_TYPE_FLUSH_ACK 13
#define L2HD_TYPE_CLICK     9  /* private pump frame: ignored by Pi server */
#define L2HD_DATA_HDR_LEN   20
#define L2HD_WIRE_HDR_LEN   (14 + L2HD_DATA_HDR_LEN)

/* v42r26: no global SysBase symbol. Exec is reached only via *(ExecBase **)4 wrappers. */
struct Library *CardResource = (struct Library *)1; /* ROM/Remus: avoid BSS hunk; diagnostics overwrite */

/*
 * Manual card.resource LVO wrappers.
 * This avoids <proto/cardres.h>, which on some cross-NDK installs
 * includes missing <clib/card_protos.h>.
 *
 * LVOs from card.resource vector table:
 *   OwnCard        -6
 *   ReleaseCard   -12
 *   GetCardMap    -18
 *   CardMiscControl -48
 *   CopyTuple     -72
 */
static LONG CR_OwnCard(struct CardHandle *h)
{
    register LONG d0 __asm("d0");
    register struct CardHandle *a1 __asm("a1") = h;
    register struct Library *a6 __asm("a6") = CardResource;
    __asm__ volatile ("jsr a6@(-6:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static LONG CR_ReleaseCard(struct CardHandle *h, ULONG flags)
{
    register ULONG d0 __asm("d0") = flags;
    register struct CardHandle *a1 __asm("a1") = h;
    register struct Library *a6 __asm("a6") = CardResource;
    __asm__ volatile ("jsr a6@(-12:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return (LONG)d0;
}

static struct CardMemoryMap *CR_GetCardMap(void)
{
    register struct CardMemoryMap *d0 __asm("d0");
    register struct Library *a6 __asm("a6") = CardResource;
    __asm__ volatile ("jsr a6@(-18:W)"
        : "=r"(d0)
        : "r"(a6)
        : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

static UBYTE CR_CardMiscControl(struct CardHandle *h, UBYTE bits)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1") = bits;
    register struct CardHandle *a1 __asm("a1") = h;
    register struct Library *a6 __asm("a6") = CardResource;
    __asm__ volatile ("jsr a6@(-48:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1), "r"(d1)
        : "a0", "cc", "memory");
    return (UBYTE)d0;
}


static LONG CR_CardResetCard(struct CardHandle *h)
{
    register LONG d0 __asm("d0");
    register struct CardHandle *a1 __asm("a1") = h;
    register struct Library *a6 __asm("a6") = CardResource;
    /* card.resource LVO inferred from the classic card.resource vector order:
     * OwnCard -6, ReleaseCard -12, GetCardMap -18, ... CardResetCard -42,
     * CardMiscControl -48, CopyTuple -72. This is volatile card reset only.
     */
    __asm__ volatile ("jsr a6@(-42:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static LONG CR_CopyTuple(struct CardHandle *h, UBYTE *buf, ULONG tuplecode, ULONG size)
{
    register ULONG d0 __asm("d0") = size;
    register ULONG d1 __asm("d1") = tuplecode;
    register UBYTE *a0 __asm("a0") = buf;
    register struct CardHandle *a1 __asm("a1") = h;
    register struct Library *a6 __asm("a6") = CardResource;
    __asm__ volatile ("jsr a6@(-72:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a1), "r"(a0), "r"(d1)
        : "cc", "memory");
    return (LONG)d0;
}

static volatile UBYTE *g_io = (volatile UBYTE *)1; /* old direct-cardres diagnostic state */
static int g_did_tx = -1; /* old diagnostic state; avoid BSS */
static UBYTE g_station_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff}; /* old diagnostic state; avoid BSS */

static UBYTE rb(volatile UBYTE *p, ULONG off) { return *((volatile UBYTE *)(p + off)); }
static void wb(volatile UBYTE *p, ULONG off, UBYTE v) { *((volatile UBYTE *)(p + off)) = v; }

static UWORD swap16(UWORD x) { return (UWORD)((x << 8) | (x >> 8)); }
static ULONG swap32(ULONG x) { return ((x & 0xffUL) << 24) | ((x & 0xff00UL) << 8) | ((x >> 8) & 0xff00UL) | ((x >> 24) & 0xffUL); }

/* direct 3C589 backend style: command/status regs are little-endian, frame bytes are raw big-endian CPU writes. */
static UWORD lew_in(volatile UBYTE *p, ULONG off) { return swap16(*((volatile UWORD *)(p + off))); }
static void lew_out(volatile UBYTE *p, ULONG off, UWORD v) { *((volatile UWORD *)(p + off)) = swap16(v); }
static UWORD raww_in(volatile UBYTE *p, ULONG off) { return *((volatile UWORD *)(p + off)); }
static void raww_out(volatile UBYTE *p, ULONG off, UWORD v) { *((volatile UWORD *)(p + off)) = v; }
static void rawl_out(volatile UBYTE *p, ULONG off, ULONG v) { *((volatile ULONG *)(p + off)) = v; }
static void lel_out(volatile UBYTE *p, ULONG off, ULONG v) { *((volatile ULONG *)(p + off)) = swap32(v); }

static void delay_small(void) { volatile ULONG i; for (i=0; i<30000; i++) { __asm__ volatile("nop"); } }
static void delay_big(void) { volatile ULONG i; for (i=0; i<900000; i++) { __asm__ volatile("nop"); } }

/* ROM build: legacy direct-cardres/CLI v35 diagnostic removed.
 * Keep only endian helpers still used by the backend device path.
 */
static void put_be16(UBYTE *p, UWORD v) { p[0]=(UBYTE)(v>>8); p[1]=(UBYTE)v; }
static void put_be32(UBYTE *p, ULONG v) { p[0]=(UBYTE)(v>>24); p[1]=(UBYTE)(v>>16); p[2]=(UBYTE)(v>>8); p[3]=(UBYTE)v; }
static UWORD be16_at(const UBYTE *p) { return (UWORD)(((UWORD)p[0] << 8) | p[1]); }
static ULONG be32_at(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | (ULONG)p[3]; }

/* ========================================================================
 * l2scsi.device v42r18-romsafe-rom3c589 attempt r1
 * ROM carrier pass:
 *  - open the backend backend by device name only: direct 3C589 backend
 *  - this lets Exec resolve a resident ROM module before any DEVS: load path
 *  - keep all v42r17/r8b transport behavior unchanged
 *
 * Derived from v42r17-romsafe-bssless-attempt-r8b.
 * ======================================================================== */

/* ========================================================================
 * l2scsi.device v42r17-romsafe-bssless attempt r7
 * ROM/Remus compatibility pass:
 *  - move mutable worker/backend buffers from file-scope BSS into device base RAM
 *  - avoid static zero block by clearing caller buffer directly
 *  - avoid global device-base dependency for worker task via tc_UserData
 *  - keep transport logic identical to v42r17 as much as possible
 *
 * NOTE: compile with -nostartfiles and inspect hunk output.  This is meant as
 * a ROM-safety source variant, not a protocol rewrite.
 * ======================================================================== */

/* ========================================================================
 * l2scsi.device v38c4-rxreset-rxstatus-zero diagnostic
 * Adds a real Exec device wrapper around the v35 3C589 transport and the
 * FS-UAE-style SCSI core. Read-only. Unit 0 only. Block size 512.
 * ======================================================================== */

#define L2SCSI_NAME       "l2scsi.device"
#define L2SCSI_VERSION    43
#define L2SCSI_REVISION   23
#define L2SCSI_VERSTRING   "$VER: l2scsi.device 43.23 (29.07.2026) LoadModule standalone coexistence with native scsi.device WRITE1024 TD_FORMAT native-RDB"
#define L2SCSI_BLOCK_SIZE 512UL
#define L2SCSI_MAX_BLOCKS_PER_IO 256UL

#define TYPE_IDENT_REQ    7
#define TYPE_IDENT_REPLY  8

#define SCSI_STATUS_GOOD             0x00
#define SCSI_STATUS_CHECK_CONDITION  0x02
#define SENSE_NO_SENSE        0x00
#define SENSE_NOT_READY       0x02
#define SENSE_MEDIUM_ERROR    0x03
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION  0x06
#define SENSE_DATA_PROTECT    0x07
#define ASC_LBA_OUT_OF_RANGE  0x21
#define ASC_INVALID_COMMAND   0x20
#define ASC_WRITE_PROTECTED   0x27
#define ASC_UNRECOVERED_READ  0x11

struct L2Disk {
    ULONG block_size;
    ULONG total_blocks_lo;
    ULONG readonly;
    ULONG online;
    ULONG max_blocks_per_io;
    UBYTE pi_mac[6];
    UBYTE sense_key, asc, ascq, unit_attention;
};

#define L2backend_RX_COUNT 2
#define L2backend_DEVICE_NAME "direct 3C589 backend"  /* ROM resident lookup; no DEVS:Networks path */
#define L2backend_DEVICE_UNIT 0
#define L2backend_RX_BUFSIZE 1600
#define L2backend_TX_BUFSIZE 1500
#define L2backend_MIN_ETH_PAYLOAD 46

struct ProbeDiag {
    ULONG magic;
    ULONG size;
    ULONG stages;
    ULONG result;
    ULONG io_base;
    ULONG attr_base;
    ULONG config_offset;
    ULONG io_offset;
    ULONG io_length;
    ULONG command_timeouts;
    ULONG tx_polls_1;
    ULONG tx_polls_2;
    ULONG link_polls;
    UWORD maker;
    UWORD product;
    UWORD transceiver;
    UWORD status_before_init;
    UWORD status_after_init;
    UWORD config_before;
    UWORD config_after;
    UWORD addrconfig_before;
    UWORD addrconfig_after;
    UWORD media_before;
    UWORD media_after_enable;
    UWORD media_after_wait;
    UWORD txspace_1;
    UWORD txspace_after_1;
    UWORD txspace_2;
    UWORD txspace_after_2;
    UBYTE config_index;
    UBYTE cor_before;
    UBYTE cor_after;
    UBYTE txstatus_before;
    UBYTE txstatus_1;
    UBYTE txstatus_2;
    UBYTE attempts;
    UBYTE reset_result;
    UBYTE link_seen;
    UBYTE mac[6];
    UBYTE pi_mac[6];
    UWORD last_rxstatus;
    UWORD ident_payload_len;
    UWORD data_payload_len;
    UWORD last_rx_length;
    ULONG ident_seq;
    ULONG data_seq;
    ULONG data_block;
    ULONG data_offset;
    ULONG total_blocks_hi;
    ULONG total_blocks_lo;
    ULONG block_size;
    ULONG readonly;
    ULONG max_count;
    ULONG window;
    ULONG rx_polls_ident;
    ULONG rx_polls_data;
    ULONG rx_bad;
    ULONG rx_frames;
    ULONG ack_sent;
    ULONG payload_valid;
    ULONG payload_bytes;
    ULONG payload_sum;
    ULONG payload_xor;
    ULONG blocks_saved;
    ULONG requests_ok;
    ULONG packets_ok;
    ULONG total_payload_bytes;
    ULONG last_request_block;
    UBYTE block0_first[32];
    UBYTE block1_first[32];
    UBYTE block6_first[32];
    UBYTE block7_first[32];
    UBYTE data_header[34];
    UBYTE reserved[4];
};

struct L2Direct3C {
    struct Device device;
    struct ExecBase *sys_base;
    struct Library *card_resource;
    struct CardHandle card_handle;
    struct Interrupt card_status_int;
    UBYTE tuple[256];
    UBYTE mac[6];
    volatile UBYTE *io_base;
    volatile UBYTE *config_base;
    ULONG result;
    UWORD maker;
    UWORD product;
    UWORD transceiver;
    UBYTE config_index;
    UBYTE reserved;
    ULONG io_offset;
    ULONG io_length;
    struct Task tx_task;
    UBYTE tx_stack[4096];
    ULONG tx_task_started;
    ULONG tx_task_finished;
    UBYTE rx_header[128];
    UBYTE pi_mac[6];
    ULONG ident_seq;
    ULONG read_seq;
    UBYTE data0_7[4096];
    /* v43.14: WRITE frame storage moved out of the 4 KiB worker stack. */
    UBYTE write_tx_frame[14 + 20 + 1024 + 4];
    struct ProbeDiag diag;
};


#define L2_ASYNC_STACK_SIZE 4096UL

/* v43.8: CFD-style isolated worker allocation.
 * The worker stack, message port and Task no longer live inside DeviceBase.
 * One MEMF_PUBLIC|MEMF_CLEAR block contains all three, preventing any stack
 * damage from reaching the 3c589 state, read cache or bootstrap diagnostics.
 */
struct L2AsyncBlock {
    UBYTE stack[L2_ASYNC_STACK_SIZE];
    struct MsgPort port;
    struct Task task;
};

struct L2ScsiBase {
    struct Device dev;
    struct ExecBase *sysbase; /* ROM-safe: mutable ExecBase pointer lives in device base RAM */
    struct L2Disk disk;
    ULONG open_count;
    struct CardHandle *ch;
    UBYTE *tuplebuf;
    struct MsgPort *async_port;
    struct Task *async_task;
    ULONG async_started;
    ULONG seq_counter; /* ROM-safe replacement for former static local l2_spin_seq state */

    struct L2Direct3C direct3c;
    ULONG direct_online;
    ULONG offline_latched; /* v43.11: bootstrap failed; fast-fail until reboot */

    /* ROM/Remus-safe mutable storage: this lives in the device base allocated
     * by Exec autoinit, not in a loader BSS hunk or in ROM data.
     */
    volatile ULONG async_ready;
    struct L2AsyncBlock *async_block;
    BYTE async_sig;
    /* v42r41 DATA1024 read-ahead cache.  This is deliberately inside
     * the device base RAM, not file-scope BSS/ROM data.
     */
    ULONG readcache_valid;
    ULONG readcache_lba;
    UBYTE readcache_data[L2SCSI_BLOCK_SIZE];

    /* v42.51 diagnostic state. */
    ULONG diagnostic_read_used;
    UBYTE diagnostic_scratch[1024];

    /* v42r29 entry/stage diagnostics: readable via TD_GETGEOMETRY.
     * This tells us whether Exec actually calls DevOpen/DevBeginIO and
     * where the backend bootstrap stops, without serial and without traffic.
     */
    volatile ULONG debug_stage;
    volatile ULONG debug_last_cmd;
    volatile LONG  debug_last_err;
    volatile ULONG debug_last_actual;

    /* v43.3 dual-resident ptable bootstrap diagnostics/state. */
    volatile ULONG boot_done;
    volatile ULONG boot_stage;
    volatile LONG  boot_scan_result;
};

#define L2_OFFSETOF(type, field) ((ULONG)&(((type *)0)->field))
#define L2_BASE_FROM_DISK(dptr) ((struct L2ScsiBase *)((UBYTE *)(dptr) - L2_OFFSETOF(struct L2ScsiBase, disk)))

static void l2_dbg_stage(struct L2ScsiBase *b, ULONG stage)
{
    if (b) b->debug_stage = stage;
}

static void l2_dbg_io(struct L2ScsiBase *b, UWORD cmd, LONG err, ULONG actual)
{
    if (!b) return;
    b->debug_last_cmd = (ULONG)cmd;
    b->debug_last_err = err;
    b->debug_last_actual = actual;
}


#ifndef PA_SIGNAL
#define PA_SIGNAL 0
#endif

#ifndef S2_DMACopyFromBuff32
#define S2_DMACopyFromBuff32 (S2_Dummy + 9)
#endif


L2_USED LONG l2_direct_read_blocks(struct L2ScsiBase *b, ULONG lba, ULONG blocks, UBYTE *dst);
L2_USED LONG l2_direct_write_blocks(struct L2ScsiBase *b, ULONG lba, ULONG blocks, const UBYTE *src);
L2_USED LONG l2_direct_flush(struct L2ScsiBase *b);

static void scsi_put_be16(UBYTE *p, UWORD v) { p[0]=(UBYTE)(v>>8); p[1]=(UBYTE)v; }
static void scsi_put_be24(UBYTE *p, ULONG v) { p[0]=(UBYTE)(v>>16); p[1]=(UBYTE)(v>>8); p[2]=(UBYTE)v; }
static void scsi_put_be32(UBYTE *p, ULONG v) { p[0]=(UBYTE)(v>>24); p[1]=(UBYTE)(v>>16); p[2]=(UBYTE)(v>>8); p[3]=(UBYTE)v; }
static ULONG scsi_be32(const UBYTE *p) { return ((ULONG)p[0]<<24)|((ULONG)p[1]<<16)|((ULONG)p[2]<<8)|p[3]; }
static UWORD scsi_be16(const UBYTE *p) { return (UWORD)(((UWORD)p[0]<<8)|p[1]); }

static void l2_set_sense2(struct L2Disk *d, UBYTE key, UBYTE asc, UBYTE ascq) { d->sense_key=key; d->asc=asc; d->ascq=ascq; }
static void l2_clear_sense2(struct L2Disk *d) { d->sense_key=SENSE_NO_SENSE; d->asc=0; d->ascq=0; }

/* ROM/-nostdlib: avoid GCC 32-bit arithmetic helper calls on plain 68000/68k.
 * The disk block size is fixed at 512 in this device, so byte counts are shifts.
 * Geometry only needs an approximate cylinder count for TD_GETGEOMETRY; compute
 * total_blocks / 1008 with a tiny subtract loop instead of pulling __udivsi3.
 */
static ULONG l2_blocks_to_bytes_512(ULONG blocks)
{
    return blocks << 9;
}

static ULONG l2_cylinders_1008(ULONG total_blocks)
{
    ULONG cyl = 0;
    while (total_blocks >= 1008UL) {
        total_blocks -= 1008UL;
        cyl++;
    }
    return cyl ? cyl : 1UL;
}

/* ROM build: unused direct-3C589 ident/read diagnostic removed; backend path below is authoritative. */

static LONG l2_read_blocks_real(struct L2Disk *d, ULONG lba, ULONG blocks, UBYTE *dst)
{
    if (!d->online) return -1;
    if (!dst) return -2;
    if (blocks == 0) return 0;
    if (blocks > d->max_blocks_per_io) return -3;
    if (lba >= d->total_blocks_lo || blocks > d->total_blocks_lo - lba) return -4;

    /* v39h: first real readonly HDF path over backend.  Keep the proven
     * v38c4/v39d resident+worker surface, but replace zero-fill reads with
     * conservative one-block READ_REQ sessions.  This is intentionally slow
     * and simple: one AmigaDOS block request may become N one-block L2HD
     * requests so we avoid out-of-order maps in the first real mount test.
     */
    return l2_direct_read_blocks(L2_BASE_FROM_DISK(d), lba, blocks, dst);
}

static LONG l2_write_blocks_real(struct L2Disk *d, ULONG lba, ULONG blocks, const UBYTE *src)
{
    if (!d->online) return -1;
    if (d->readonly) return -5;
    if (!src) return -2;
    if (blocks == 0) return 0;
    if (blocks > d->max_blocks_per_io) return -3;
    if (lba >= d->total_blocks_lo || blocks > d->total_blocks_lo - lba) return -4;
    return l2_direct_write_blocks(L2_BASE_FROM_DISK(d), lba, blocks, src);
}

static ULONG l2_request_sense(struct L2Disk *d, UBYTE *data, ULONG alloc_len)
{
    ULONG n = alloc_len < 18 ? alloc_len : 18; memset(data,0,n);
    if (n >= 14) { data[0]=0x70; data[2]=d->sense_key & 0x0f; data[7]=10; data[12]=d->asc; data[13]=d->ascq; }
    l2_clear_sense2(d); return n;
}

static LONG l2scsi_read_lba2(struct L2Disk *d, ULONG lba, ULONG blocks, UBYTE *data, ULONG data_cap, ULONG *actual)
{
    ULONG len; *actual=0;
    if (!d->online) { l2_set_sense2(d,SENSE_NOT_READY,0x04,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (blocks == 0) return SCSI_STATUS_GOOD;
    if (lba >= d->total_blocks_lo || blocks > d->total_blocks_lo - lba) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,ASC_LBA_OUT_OF_RANGE,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (blocks > d->max_blocks_per_io) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,0x24,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (d->block_size != L2SCSI_BLOCK_SIZE) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,0x24,0); return SCSI_STATUS_CHECK_CONDITION; }
    len = l2_blocks_to_bytes_512(blocks); if (len > data_cap) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,0x24,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (l2_read_blocks_real(d,lba,blocks,data) != 0) { l2_set_sense2(d,SENSE_MEDIUM_ERROR,ASC_UNRECOVERED_READ,0); return SCSI_STATUS_CHECK_CONDITION; }
    *actual=len; l2_clear_sense2(d); return SCSI_STATUS_GOOD;
}

static LONG l2scsi_write_lba2(struct L2Disk *d, ULONG lba, ULONG blocks, const UBYTE *data, ULONG data_cap, ULONG *actual)
{
    ULONG len; *actual=0;
    if (!d->online) { l2_set_sense2(d,SENSE_NOT_READY,0x04,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (d->readonly) { l2_set_sense2(d,SENSE_DATA_PROTECT,ASC_WRITE_PROTECTED,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (blocks == 0) return SCSI_STATUS_GOOD;
    if (lba >= d->total_blocks_lo || blocks > d->total_blocks_lo-lba) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,ASC_LBA_OUT_OF_RANGE,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (blocks > d->max_blocks_per_io) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,0x24,0); return SCSI_STATUS_CHECK_CONDITION; }
    len=l2_blocks_to_bytes_512(blocks);
    if (!data || len>data_cap) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,0x24,0); return SCSI_STATUS_CHECK_CONDITION; }
    if (l2_write_blocks_real(d,lba,blocks,data)!=0) { l2_set_sense2(d,SENSE_MEDIUM_ERROR,0x0c,0); return SCSI_STATUS_CHECK_CONDITION; }
    *actual=len; l2_clear_sense2(d); return SCSI_STATUS_GOOD;
}

L2_USED LONG l2scsi_emulate2(struct L2Disk *d, const UBYTE *cmd, ULONG cmd_len, UBYTE *data, ULONG data_cap, ULONG *actual)
{
    UBYTE op; ULONG lba, blocks, n, pos; (void)cmd_len; *actual=0;
    if (!cmd) { l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,ASC_INVALID_COMMAND,0); return SCSI_STATUS_CHECK_CONDITION; }
    op=cmd[0];
    switch(op) {
    case 0x00: if (!d->online) { l2_set_sense2(d,SENSE_NOT_READY,0x04,0); return SCSI_STATUS_CHECK_CONDITION; } if (d->unit_attention) { d->unit_attention=0; l2_set_sense2(d,SENSE_UNIT_ATTENTION,0x29,0); return SCSI_STATUS_CHECK_CONDITION; } return SCSI_STATUS_GOOD;
    case 0x03: *actual = l2_request_sense(d,data,cmd[4] < data_cap ? cmd[4] : data_cap); return SCSI_STATUS_GOOD;
    case 0x12: n=cmd[4]; if (n>data_cap) n=data_cap; if (n>36) n=36; memset(data,0,n); if(n>0)data[0]=0; if(n>2)data[2]=2; if(n>3)data[3]=2; if(n>4)data[4]=31; if(n>15)memcpy(data+8,"L2BOOT  ",8); if(n>31)memcpy(data+16,"PI-HDF          ",16); if(n>35)memcpy(data+32,"0037",4); *actual=n; return SCSI_STATUS_GOOD;
    case 0x25: if (!d->online) { l2_set_sense2(d,SENSE_NOT_READY,0x04,0); return SCSI_STATUS_CHECK_CONDITION; } if (data_cap<8) return SCSI_STATUS_GOOD; scsi_put_be32(data,d->total_blocks_lo-1); scsi_put_be32(data+4,d->block_size); *actual=8; return SCSI_STATUS_GOOD;
    case 0x1a: n=cmd[4]; if(n>data_cap)n=data_cap; memset(data,0,n); if(n>=4){data[2]=d->readonly?0x80:0; pos=4; if(n>=12){data[3]=8; scsi_put_be24(data+pos+1,d->total_blocks_lo>0xffffff?0xffffff:d->total_blocks_lo); scsi_put_be24(data+pos+5,d->block_size); pos+=8;} data[0]=(UBYTE)(pos-1); *actual=pos<n?pos:n;} return SCSI_STATUS_GOOD;
    case 0x5a: n=scsi_be16(cmd+7); if(n>data_cap)n=data_cap; memset(data,0,n); if(n>=8){data[3]=d->readonly?0x80:0; scsi_put_be16(data, 6); *actual=8;} return SCSI_STATUS_GOOD;
    case 0x08: lba=((ULONG)(cmd[1]&0x1f)<<16)|((ULONG)cmd[2]<<8)|cmd[3]; blocks=cmd[4]?cmd[4]:256; return l2scsi_read_lba2(d,lba,blocks,data,data_cap,actual);
    case 0x28: lba=scsi_be32(cmd+2); blocks=scsi_be16(cmd+7); return l2scsi_read_lba2(d,lba,blocks,data,data_cap,actual);
    case 0x0a: lba=((ULONG)(cmd[1]&0x1f)<<16)|((ULONG)cmd[2]<<8)|cmd[3]; blocks=cmd[4]?cmd[4]:256; return l2scsi_write_lba2(d,lba,blocks,data,data_cap,actual);
    case 0x2a: lba=scsi_be32(cmd+2); blocks=scsi_be16(cmd+7); return l2scsi_write_lba2(d,lba,blocks,data,data_cap,actual);
    case 0x35: if (d->readonly) return SCSI_STATUS_GOOD; if (l2_direct_flush(L2_BASE_FROM_DISK(d)) != 0) { l2_set_sense2(d,SENSE_MEDIUM_ERROR,0x0c,0); return SCSI_STATUS_CHECK_CONDITION; } return SCSI_STATUS_GOOD;
    case 0x1b: case 0x2f: return SCSI_STATUS_GOOD;
    default: l2_set_sense2(d,SENSE_ILLEGAL_REQUEST,ASC_INVALID_COMMAND,0); return SCSI_STATUS_CHECK_CONDITION;
    }
}

static void l2scsi_cmd_read(struct L2Disk *d, struct IOStdReq *io)
{
    ULONG off = io->io_Offset;
    ULONG len = io->io_Length;
    ULONG lba, blocks;
    ULONG done_blocks = 0;
    UBYTE *dst = (UBYTE *)io->io_Data;

    if ((off & 511UL) || (len & 511UL) || !dst) {
        io->io_Error = IOERR_BADLENGTH;
        io->io_Actual = 0;
        return;
    }
    if (!d->online) {
        io->io_Error = IOERR_OPENFAIL;
        io->io_Actual = 0;
        return;
    }

    lba = off >> 9;
    blocks = len >> 9;
    if (blocks == 0) {
        io->io_Error = 0;
        io->io_Actual = 0;
        return;
    }
    if (lba >= d->total_blocks_lo || blocks > d->total_blocks_lo - lba) {
        io->io_Error = TDERR_SeekError;
        io->io_Actual = 0;
        return;
    }

    /* v43.10: Exec/DOS may issue a single READ larger than the transport's
     * certified per-call limit (256 blocks / 128 KiB).  Keep that transport
     * limit unchanged and split the caller's request internally.  Workbench
     * drag-and-drop and Copy BUF=0 can otherwise fail with IOERR_BADLENGTH.
     */
    while (done_blocks < blocks) {
        ULONG chunk = blocks - done_blocks;
        ULONG chunk_bytes;

        if (chunk > d->max_blocks_per_io)
            chunk = d->max_blocks_per_io;
        if (chunk == 0) {
            io->io_Error = IOERR_BADLENGTH;
            io->io_Actual = l2_blocks_to_bytes_512(done_blocks);
            return;
        }

        chunk_bytes = l2_blocks_to_bytes_512(chunk);
        if (l2_read_blocks_real(d, lba + done_blocks, chunk,
                                dst + l2_blocks_to_bytes_512(done_blocks)) != 0) {
            io->io_Error = TDERR_NotSpecified;
            io->io_Actual = l2_blocks_to_bytes_512(done_blocks);
            return;
        }

        done_blocks += chunk;
        io->io_Actual += chunk_bytes;
    }

    io->io_Error = 0;
}


static void l2scsi_cmd_write(struct L2Disk *d, struct IOStdReq *io)
{
    ULONG off=io->io_Offset, len=io->io_Length, lba, blocks, done=0;
    const UBYTE *src=(const UBYTE *)io->io_Data;
    if ((off&511UL)||(len&511UL)||!src) { io->io_Error=IOERR_BADLENGTH; return; }
    if (!d->online) { io->io_Error=IOERR_OPENFAIL; return; }
    if (d->readonly) { io->io_Error=TDERR_WriteProt; return; }
    lba=off>>9; blocks=len>>9;
    if (!blocks) { io->io_Error=0; return; }
    if (lba>=d->total_blocks_lo || blocks>d->total_blocks_lo-lba) { io->io_Error=TDERR_SeekError; return; }
    while (done<blocks) {
        ULONG chunk=blocks-done;
        if (chunk>d->max_blocks_per_io) chunk=d->max_blocks_per_io;
        if (!chunk || l2_write_blocks_real(d,lba+done,chunk,src+(done<<9))!=0) {
            io->io_Error=TDERR_NotSpecified; io->io_Actual=done<<9; return;
        }
        done+=chunk; io->io_Actual=done<<9;
    }
    L2_BASE_FROM_DISK(d)->readcache_valid=0;
    io->io_Error=0;
}

static void l2scsi_geometry(struct L2Disk *d, struct IOStdReq *io)
{
    struct DriveGeometry *dg=(struct DriveGeometry*)io->io_Data;
    struct L2ScsiBase *b = L2_BASE_FROM_DISK(d);
    if (!dg || io->io_Length < sizeof(*dg)) { io->io_Error=IOERR_BADLENGTH; return; }
    /* Do not overwrite debug_stage here: TD_GETGEOMETRY is the readout path. */
    memset(dg,0,sizeof(*dg));
    dg->dg_SectorSize=512;
    dg->dg_TotalSectors=d->total_blocks_lo;
    dg->dg_CylSectors=1008;
    dg->dg_Heads=16;
    dg->dg_TrackSectors=63;
    /* Diagnostic encoding: cylinders is not used by our transport.
     * l2stat prints this as the current debug stage.  Keep TotalSectors
     * sane so Mount/HDToolBox are not confused by impossible capacity.
     */
    dg->dg_Cylinders=b ? (b->boot_stage ? b->boot_stage : b->debug_stage)
                       : l2_cylinders_1008(d->total_blocks_lo);
    dg->dg_BufMemType=MEMF_PUBLIC;
    dg->dg_DeviceType=DG_DIRECT_ACCESS;
    dg->dg_Flags=d->readonly?DGF_REMOVABLE:0;
    io->io_Actual=sizeof(*dg);
    io->io_Error=0;
}

static void l2scsi_scsi_cmd(struct L2Disk *d, struct IOStdReq *io)
{
    struct SCSICmd *sc=(struct SCSICmd*)io->io_Data; ULONG actual=0; LONG st;
    if (!sc) { io->io_Error=IOERR_BADADDRESS; return; }
    st=l2scsi_emulate2(d,(const UBYTE*)sc->scsi_Command,sc->scsi_CmdLength,(UBYTE*)sc->scsi_Data,sc->scsi_Length,&actual);
    sc->scsi_Actual=actual; sc->scsi_Status=(UBYTE)st;
    if (st==SCSI_STATUS_CHECK_CONDITION && sc->scsi_SenseData && sc->scsi_SenseLength) sc->scsi_SenseActual=l2_request_sense(d,(UBYTE*)sc->scsi_SenseData,sc->scsi_SenseLength); else sc->scsi_SenseActual=0;
    io->io_Actual=sizeof(struct SCSICmd); io->io_Error=(st==SCSI_STATUS_GOOD)?0:HFERR_BadStatus;
}

static void l2scsi_begin_core2(struct L2Disk *d, struct IOStdReq *io)
{
    io->io_Error=0; io->io_Actual=0;
    switch(io->io_Command) {
    case CMD_READ: case NSCMD_TD_READ64: l2scsi_cmd_read(d,io); break;
    case CMD_WRITE: case TD_FORMAT: case NSCMD_TD_WRITE64: l2scsi_cmd_write(d,io); break;
    case CMD_UPDATE: case CMD_FLUSH:
        if (!d->readonly && l2_direct_flush(L2_BASE_FROM_DISK(d)) != 0) io->io_Error=TDERR_NotSpecified;
        break;
    case CMD_CLEAR: io->io_Error=0; break;
    case TD_GETGEOMETRY: l2scsi_geometry(d,io); break;
    case TD_PROTSTATUS: io->io_Actual=d->readonly?-1:0; break;
    case TD_CHANGESTATE: io->io_Actual=0; break;
    case TD_CHANGENUM: io->io_Actual=1; break;
    case HD_SCSICMD: l2scsi_scsi_cmd(d,io); break;
    default: io->io_Error=IOERR_NOCMD; break;
    }
}


/* ========================================================================
 * v42r26 ROM diagnostic: absolute ExecBase wrappers.
 *
 * The previous v42r23 null/nosysglobal stub proved the resident skeleton can
 * live in ROM.  v42r24 reintroduced the real backend/3c589 path and can Guru
 * before any wire traffic.  This build removes dependency on the global
 * SysBase symbol from the pre-traffic Exec/backend path by calling Exec through
 * the absolute ExecBase pointer at address 4, or through explicit LVO wrappers.
 *
 * This is still a localization build: no protocol rewrite, no 3c589 changes.
 * ======================================================================== */
static struct ExecBase *l2_abs_execbase(void)
{
    return *((struct ExecBase **)4);
}

static void l2_exec_new_list_local(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static APTR l2_exec_AllocMem(ULONG size, ULONG flags)
{
    register APTR d0 __asm("d0");
    register ULONG rd0 __asm("d0") = size;
    register ULONG rd1 __asm("d1") = flags;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-198:W)"
        : "=r"(d0)
        : "r"(a6), "r"(rd0), "r"(rd1)
        : "a0", "a1", "cc", "memory");
    return d0;
}

static void l2_exec_FreeMem(APTR mem, ULONG size)
{
    register APTR a1 __asm("a1") = mem;
    register ULONG d0 __asm("d0") = size;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-210:W)"
        :
        : "r"(a6), "r"(a1), "r"(d0)
        : "d1", "a0", "cc", "memory");
}

static void l2_exec_CopyMem(APTR src, APTR dst, ULONG len)
{
    register APTR a0 __asm("a0") = src;
    register APTR a1 __asm("a1") = dst;
    register ULONG d0 __asm("d0") = len;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-624:W)"
        :
        : "r"(a6), "r"(a0), "r"(a1), "r"(d0)
        : "d1", "cc", "memory");
}

static struct Task *l2_exec_FindTask(STRPTR name)
{
    register struct Task *d0 __asm("d0");
    register STRPTR a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-294:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static BYTE l2_exec_AllocSignal(LONG sig)
{
    register LONG d0 __asm("d0") = sig;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-330:W)"
        : "+r"(d0)
        : "r"(a6)
        : "d1", "a0", "a1", "cc", "memory");
    return (BYTE)d0;
}

static void l2_exec_FreeSignal(LONG sig)
{
    register LONG d0 __asm("d0") = sig;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-336:W)"
        :
        : "r"(a6), "r"(d0)
        : "d1", "a0", "a1", "cc", "memory");
}

static ULONG l2_exec_Wait(ULONG sigmask)
{
    register ULONG d0 __asm("d0") = sigmask;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-318:W)"
        : "+r"(d0)
        : "r"(a6)
        : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

static void l2_exec_AddTask(struct Task *task, APTR initpc, APTR finalpc)
{
    register struct Task *a1 __asm("a1") = task;
    register APTR a2 __asm("a2") = initpc;
    register APTR a3 __asm("a3") = finalpc;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-282:W)"
        :
        : "r"(a6), "r"(a1), "r"(a2), "r"(a3)
        : "d0", "d1", "a0", "cc", "memory");
}

static struct Message *l2_exec_GetMsg(struct MsgPort *port)
{
    register struct Message *d0 __asm("d0");
    register struct MsgPort *a0 __asm("a0") = port;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-372:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a0)
        : "d1", "a1", "cc", "memory");
    return d0;
}

static LONG l2_exec_OpenDevice(STRPTR name, ULONG unit, struct IORequest *io, ULONG flags)
{
    register LONG d0 __asm("d0") = unit;
    register STRPTR a0 __asm("a0") = name;
    register struct IORequest *a1 __asm("a1") = io;
    register ULONG d1 __asm("d1") = flags;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-444:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a0), "r"(a1), "r"(d1)
        : "cc", "memory");
    return d0;
}

static void l2_exec_CloseDevice(struct IORequest *io)
{
    register struct IORequest *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-450:W)"
        :
        : "r"(a6), "r"(a1)
        : "d0", "d1", "a0", "cc", "memory");
}

static LONG l2_exec_DoIO(struct IORequest *io)
{
    register LONG d0 __asm("d0");
    register struct IORequest *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-456:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static void l2_exec_SendIO(struct IORequest *io)
{
    register struct IORequest *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-462:W)"
        :
        : "r"(a6), "r"(a1)
        : "d0", "d1", "a0", "cc", "memory");
}

static LONG l2_exec_WaitIO(struct IORequest *io)
{
    register LONG d0 __asm("d0");
    register struct IORequest *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-474:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static LONG l2_exec_AbortIO(struct IORequest *io)
{
    register LONG d0 __asm("d0");
    register struct IORequest *a1 __asm("a1") = io;
    register struct ExecBase *a6 __asm("a6") = l2_abs_execbase();
    __asm__ volatile ("jsr a6@(-480:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static struct MsgPort *l2_exec_CreateMsgPort(void)
{
    struct MsgPort *p;
    BYTE sig;

    p = (struct MsgPort *)l2_exec_AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC|MEMF_CLEAR);
    if (!p) return NULL;

    sig = l2_exec_AllocSignal(-1);
    if (sig < 0) {
        l2_exec_FreeMem(p, sizeof(struct MsgPort));
        return NULL;
    }

    p->mp_Node.ln_Type = NT_MSGPORT;
    p->mp_Flags = PA_SIGNAL;
    p->mp_SigBit = sig;
    p->mp_SigTask = l2_exec_FindTask(NULL);
    l2_exec_new_list_local(&p->mp_MsgList);
    return p;
}

static void l2_exec_DeleteMsgPort(struct MsgPort *p)
{
    if (!p) return;
    if (p->mp_SigBit >= 0) l2_exec_FreeSignal(p->mp_SigBit);
    l2_exec_FreeMem(p, sizeof(struct MsgPort));
}

static struct IORequest *l2_exec_CreateIORequest(struct MsgPort *reply_port, ULONG size)
{
    struct IORequest *io;
    if (!reply_port || size < sizeof(struct IORequest)) return NULL;
    io = (struct IORequest *)l2_exec_AllocMem(size, MEMF_PUBLIC|MEMF_CLEAR);
    if (!io) return NULL;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    io->io_Message.mn_ReplyPort = reply_port;
    io->io_Message.mn_Length = (UWORD)size;
    return io;
}

static void l2_exec_DeleteIORequest(struct IORequest *io)
{
    ULONG size;
    if (!io) return;
    size = io->io_Message.mn_Length;
    if (size < sizeof(struct IORequest)) size = sizeof(struct IORequest);
    l2_exec_FreeMem(io, size);
}

#ifdef AllocMem
#undef AllocMem
#endif
#ifdef FreeMem
#undef FreeMem
#endif
#ifdef CopyMem
#undef CopyMem
#endif
#ifdef FindTask
#undef FindTask
#endif
#ifdef AllocSignal
#undef AllocSignal
#endif
#ifdef FreeSignal
#undef FreeSignal
#endif
#ifdef Wait
#undef Wait
#endif
#ifdef AddTask
#undef AddTask
#endif
#ifdef GetMsg
#undef GetMsg
#endif
#ifdef OpenDevice
#undef OpenDevice
#endif
#ifdef CloseDevice
#undef CloseDevice
#endif
#ifdef DoIO
#undef DoIO
#endif
#ifdef SendIO
#undef SendIO
#endif
#ifdef WaitIO
#undef WaitIO
#endif
#ifdef AbortIO
#undef AbortIO
#endif
#ifdef CreateMsgPort
#undef CreateMsgPort
#endif
#ifdef DeleteMsgPort
#undef DeleteMsgPort
#endif
#ifdef CreateIORequest
#undef CreateIORequest
#endif
#ifdef DeleteIORequest
#undef DeleteIORequest
#endif

#define AllocMem(size, flags)           l2_exec_AllocMem((ULONG)(size), (ULONG)(flags))
#define FreeMem(mem, size)              l2_exec_FreeMem((APTR)(mem), (ULONG)(size))
#define CopyMem(src, dst, len)          l2_exec_CopyMem((APTR)(src), (APTR)(dst), (ULONG)(len))
#define FindTask(name)                  l2_exec_FindTask((STRPTR)(name))
#define AllocSignal(sig)                l2_exec_AllocSignal((LONG)(sig))
#define FreeSignal(sig)                 l2_exec_FreeSignal((LONG)(sig))
#define Wait(sigmask)                   l2_exec_Wait((ULONG)(sigmask))
#define AddTask(task, initpc, finalpc)  l2_exec_AddTask((struct Task *)(task), (APTR)(initpc), (APTR)(finalpc))
#define GetMsg(port)                    l2_exec_GetMsg((struct MsgPort *)(port))
#define OpenDevice(name, unit, io, flags) l2_exec_OpenDevice((STRPTR)(name), (ULONG)(unit), (struct IORequest *)(io), (ULONG)(flags))
#define CloseDevice(io)                 l2_exec_CloseDevice((struct IORequest *)(io))
#define DoIO(io)                        l2_exec_DoIO((struct IORequest *)(io))
#define SendIO(io)                      l2_exec_SendIO((struct IORequest *)(io))
#define WaitIO(io)                      l2_exec_WaitIO((struct IORequest *)(io))
#define AbortIO(io)                     l2_exec_AbortIO((struct IORequest *)(io))
#define CreateMsgPort()                 l2_exec_CreateMsgPort()
#define DeleteMsgPort(port)             l2_exec_DeleteMsgPort((struct MsgPort *)(port))
#define CreateIORequest(port, size)     l2_exec_CreateIORequest((struct MsgPort *)(port), (ULONG)(size))
#define DeleteIORequest(io)             l2_exec_DeleteIORequest((struct IORequest *)(io))


/* Direct 3C589 backend: mechanically transplanted from certified probe.device v3.3. */
#ifndef CARDF_IFAVAILABLE
#define CARDF_IFAVAILABLE 0x01
#endif
#ifndef CARD_ENABLEF_DIGAUDIO
#define CARD_ENABLEF_DIGAUDIO 0x02
#endif
#ifndef CARD_DISABLEF_WP
#define CARD_DISABLEF_WP 0x04
#endif
#define EL3_STATUS_CMD_IN_PROGRESS EL3REG_STATUSF_CMDINPROGRESS
#define EL3_RX_INCOMPLETE EL3REG_RXSTATUSF_INCOMPLETE
#define EL3_RX_ERROR EL3REG_RXSTATUSF_ERROR
#define EL3_RX_LENMASK EL3REG_RXSTATUSF_LENMASK
#define EL3_CONFIG_HAS_TP EL3REG_CONFIGF_HASTP
#define EL3_CONFIG_HAS_COAX EL3REG_CONFIGF_HASCOAX
#define EL3_CONFIG_HAS_AUI EL3REG_CONFIGF_HASAUI
#define EL3_ADDR_XCVR_MASK EL3REG_ADDRCONFIG_XCVRMASK
#define EL3_ADDR_XCVR_TP EL3REG_ADDRCONFIG_XCVRTP
#define EL3_ADDR_XCVR_AUI EL3REG_ADDRCONFIG_XCVRAUI
#define EL3_ADDR_XCVR_COAX EL3REG_ADDRCONFIG_XCVRCOAX
#define EL3_MEDIA_BEAT EL3REG_MEDIAF_BEAT
#define EL3_MEDIA_BEAT_EN EL3REG_MEDIAF_BEATENABLE
#define EL3_MEDIA_JABBER_EN EL3REG_MEDIAF_JABBERENABLE
#define EL3_EEPROM_BUSY 0x8000
#define EL3ECMD_READ (0x8U << 4)
#define EL3EEPROM_ADDRESS0 0
#define EL3EEPROM_ADDRCONFIG 8
#define EL3EEPROM_ALTADDRESS0 10
#define PR_NOT_RUN 0
#define PR_NO_CARD_RESOURCE 1
#define PR_OWNCARD_FAILED 2
#define PR_BAD_MANFID 3
#define PR_NO_CONFIG_TUPLE 4
#define PR_NO_CFTABLE 5
#define PR_NO_CARD_MAP 6
#define PR_BAD_EEPROM 7
#define PR_INIT_COMMAND_TIMEOUT 8
#define PR_NO_TX_SPACE 9
#define PR_TX_ERROR 10
#define PR_TX_TIMEOUT 11
#define PR_SENT 12
#define STAGE_OPEN_RESOURCE (1UL<<0)
#define STAGE_OWN_CARD (1UL<<1)
#define STAGE_MANFID (1UL<<2)
#define STAGE_CONFIG (1UL<<3)
#define STAGE_MAP (1UL<<4)
#define STAGE_COR_WRITTEN (1UL<<5)
#define STAGE_MAC_READ (1UL<<6)
#define STAGE_INITIALISED (1UL<<7)
#define STAGE_TX1_QUEUED (1UL<<8)
#define STAGE_TX2_QUEUED (1UL << 9)
#define STAGE_LINK_WAIT (1UL<<11)
#define STAGE_LINK_SEEN (1UL<<12)
#define STAGE_IDENT_TX (1UL<<13)
#define STAGE_IDENT_RX (1UL<<14)
#define STAGE_READ0_TX (1UL<<15)
#define STAGE_DATA0_RX (1UL<<16)
#define STAGE_ACK_TX (1UL<<17)
#define STAGE_DATA1024_SAVED (1UL<<18)
#define L2HD_TYPE_IDENT_REQ TYPE_IDENT_REQ
#define L2HD_TYPE_IDENT_REPLY TYPE_IDENT_REPLY
static char d3_name[] = L2SCSI_NAME;
static char d3_card_resource_name[] = CARDRESNAME;
static UWORD d3_bswap16(UWORD v)
{
    return (UWORD)((v << 8) | (v >> 8));
}

static ULONG d3_bswap32(ULONG v)
{
    return ((v & 0x000000ffUL) << 24) |
           ((v & 0x0000ff00UL) << 8)  |
           ((v & 0x00ff0000UL) >> 8)  |
           ((v & 0xff000000UL) >> 24);
}

static UWORD d3_le16_in(volatile UBYTE *base, ULONG off)
{
    return d3_bswap16(*(volatile UWORD *)(base + off));
}

static void d3_le16_out(volatile UBYTE *base, ULONG off, UWORD value)
{
    *(volatile UWORD *)(base + off) = d3_bswap16(value);
}

static void d3_le32_out(volatile UBYTE *base, ULONG off, ULONG value)
{
    *(volatile ULONG *)(base + off) = d3_bswap32(value);
}

static void d3_raw32_out(volatile UBYTE *base, ULONG off, ULONG value)
{
    *(volatile ULONG *)(base + off) = value;
}

static UBYTE d3_raw8_in(volatile UBYTE *base, ULONG off)
{
    return *(volatile UBYTE *)(base + off);
}

static void d3_raw8_out(volatile UBYTE *base, ULONG off, UBYTE value)
{
    *(volatile UBYTE *)(base + off) = value;
}

static void d3_spin(ULONG count)
{
    while (count-- != 0)
        __asm__ volatile ("nop");
}

/* Exec OpenResource(), LVO -498. */
static struct Library *d3_exec_open_resource(struct ExecBase *sys_base,
                                          const char *name)
{
    register struct Library *d0 __asm("d0");
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys_base;

    __asm__ volatile ("jsr a6@(-498:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}


/* Exec AddTask(), LVO -282.  The task and its stack live in the autoinit
 * device base, so no allocation and no BSS are required. */
static LONG d3_exec_add_task(struct ExecBase *sys_base, struct Task *task,
                          APTR initial_pc, APTR final_pc)
{
    register LONG d0 __asm("d0");
    register struct Task *a1 __asm("a1") = task;
    register APTR a2 __asm("a2") = initial_pc;
    register APTR a3 __asm("a3") = final_pc;
    register struct ExecBase *a6 __asm("a6") = sys_base;

    __asm__ volatile ("jsr a6@(-282:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1), "r"(a2), "r"(a3)
        : "d1", "a0", "cc", "memory");
    return d0;
}


/* Exec RemTask(), LVO -288. */
static void d3_exec_rem_task(struct ExecBase *sys_base, struct Task *task)
{
    register struct Task *a1 __asm("a1") = task;
    register struct ExecBase *a6 __asm("a6") = sys_base;

    __asm__ volatile ("jsr a6@(-288:W)"
        :
        : "r"(a6), "r"(a1)
        : "d0", "d1", "a0", "cc", "memory");
}

/* Exec FindTask(), LVO -294. */
static struct Task *d3_exec_find_task(struct ExecBase *sys_base, const char *name)
{
    register struct Task *d0 __asm("d0");
    register const char *a1 __asm("a1") = name;
    register struct ExecBase *a6 __asm("a6") = sys_base;

    __asm__ volatile ("jsr a6@(-294:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

/* card.resource vectors from the documented classic vector order. */
static LONG d3_cr_own_card(struct Library *card_base, struct CardHandle *handle)
{
    register LONG d0 __asm("d0");
    register struct CardHandle *a1 __asm("a1") = handle;
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-6:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}


/* card.resource ReleaseCard(), LVO -12. */
static LONG d3_cr_release_card(struct Library *card_base,
                            struct CardHandle *handle, ULONG flags)
{
    register ULONG d0 __asm("d0") = flags;
    register struct CardHandle *a1 __asm("a1") = handle;
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-12:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return (LONG)d0;
}

static struct CardMemoryMap *d3_cr_get_card_map(struct Library *card_base)
{
    register struct CardMemoryMap *d0 __asm("d0");
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-18:W)"
        : "=r"(d0)
        : "r"(a6)
        : "d1", "a0", "a1", "cc", "memory");
    return d0;
}

static LONG d3_cr_reset_card(struct Library *card_base,
                          struct CardHandle *handle)
{
    register LONG d0 __asm("d0");
    register struct CardHandle *a1 __asm("a1") = handle;
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-42:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1)
        : "d1", "a0", "cc", "memory");
    return d0;
}

static UBYTE d3_cr_misc_control(struct Library *card_base,
                             struct CardHandle *handle, UBYTE bits)
{
    register ULONG d0 __asm("d0");
    register ULONG d1 __asm("d1") = bits;
    register struct CardHandle *a1 __asm("a1") = handle;
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-48:W)"
        : "=r"(d0)
        : "r"(a6), "r"(a1), "r"(d1)
        : "a0", "cc", "memory");
    return (UBYTE)d0;
}

static LONG d3_cr_copy_tuple(struct Library *card_base,
                          struct CardHandle *handle, UBYTE *buffer,
                          ULONG tuple_code, ULONG buffer_size)
{
    register ULONG d0 __asm("d0") = buffer_size;
    register ULONG d1 __asm("d1") = tuple_code;
    register UBYTE *a0 __asm("a0") = buffer;
    register struct CardHandle *a1 __asm("a1") = handle;
    register struct Library *a6 __asm("a6") = card_base;

    __asm__ volatile ("jsr a6@(-72:W)"
        : "+r"(d0)
        : "r"(a6), "r"(a1), "r"(a0), "r"(d1)
        : "cc", "memory");
    return (LONG)d0;
}

static UBYTE *d3_tuple_payload(UBYTE *tuple, UBYTE expected_code, ULONG *length)
{
    if (tuple[0] == expected_code) {
        *length = tuple[1];
        return tuple + 2;
    }

    /* Defensive fallback for card.resource variants returning payload only. */
    *length = 254;
    return tuple;
}

static int d3_parse_manfid(struct L2Direct3C *base)
{
    ULONG len;
    UBYTE *p;

    if (!d3_cr_copy_tuple(base->card_resource, &base->card_handle,
                       base->tuple, CISTPL_MANFID, sizeof(base->tuple)))
        return 0;

    p = d3_tuple_payload(base->tuple, CISTPL_MANFID, &len);
    if (len < 4)
        return 0;

    base->maker = (UWORD)(p[0] | ((UWORD)p[1] << 8));
    base->product = (UWORD)(p[2] | ((UWORD)p[3] << 8));

    if (base->maker != 0x0101)
        return 0;

    switch (base->product) {
    case 0x0035:
    case 0x003d:
    case 0x0556:
    case 0x0562:
    case 0x0574:
    case 0x0589:
        return 1;
    default:
        return 0;
    }
}

static int d3_parse_config_base(struct L2Direct3C *base, ULONG *config_offset)
{
    ULONG len;
    UBYTE *p;
    ULONG value;
    ULONG bytes;
    ULONG i;

    if (!d3_cr_copy_tuple(base->card_resource, &base->card_handle,
                       base->tuple, CISTPL_CONFIG, sizeof(base->tuple)))
        return 0;

    p = d3_tuple_payload(base->tuple, CISTPL_CONFIG, &len);
    if (len < 3)
        return 0;

    bytes = (ULONG)(p[0] & 0x03) + 1;
    if (2 + bytes > len || bytes > 4)
        return 0;

    value = 0;
    for (i = 0; i < bytes; ++i)
        value |= (ULONG)p[2 + i] << (i * 8);

    *config_offset = value;
    return 1;
}

static ULONG d3_tuple_read_le(const UBYTE *p, ULONG bytes)
{
    ULONG value = 0;
    ULONG i;
    for (i = 0; i < bytes; ++i)
        value |= (ULONG)p[i] << (8 * i);
    return value;
}

static int d3_tuple_skip_ext_value(const UBYTE *p, ULONG len, ULONG *pos)
{
    UBYTE b;
    do {
        if (*pos >= len) return 0;
        b = p[(*pos)++];
    } while (b & 0x80);
    return 1;
}

static int d3_parse_config_index(struct L2Direct3C *base)
{
    ULONG len, pos, i, count, addr_bytes, len_bytes;
    UBYTE *p;
    UBYTE index, features, desc, ranges;
    ULONG selected_base = 0;
    ULONG selected_length = 0;

    if (!d3_cr_copy_tuple(base->card_resource, &base->card_handle,
                       base->tuple, CISTPL_CFTABLE, sizeof(base->tuple)))
        return 0;

    p = d3_tuple_payload(base->tuple, CISTPL_CFTABLE, &len);
    if (len < 2) return 0;

    index = (UBYTE)(p[0] & 0x3f);
    pos = 1;

    /* TPCE_INDX bit 7: interface description byte follows. */
    if (p[0] & 0x80) {
        if (pos >= len) return 0;
        ++pos;
    }

    if (pos >= len) return 0;
    features = p[pos++];

    /* Power descriptors: low two feature bits give descriptor count. */
    count = features & 0x03;
    while (count-- != 0) {
        UBYTE params;
        if (pos >= len) return 0;
        params = p[pos++];
        for (i = 0; i < 7; ++i)
            if ((params & (1U << i)) && !d3_tuple_skip_ext_value(p, len, &pos))
                return 0;
    }

    /* Timing descriptor. Each non-zero scale field has one extended value. */
    if (features & 0x04) {
        UBYTE timing;
        if (pos >= len) return 0;
        timing = p[pos++];
        if ((timing & 0x03) != 0x03 && !d3_tuple_skip_ext_value(p, len, &pos)) return 0;
        if (((timing >> 2) & 0x07) != 0x07 && !d3_tuple_skip_ext_value(p, len, &pos)) return 0;
        if (((timing >> 5) & 0x07) != 0x07 && !d3_tuple_skip_ext_value(p, len, &pos)) return 0;
    }

    /* I/O descriptor: this is the information the old probe discarded. */
    if (!(features & 0x08) || pos >= len) return 0;
    desc = p[pos++];

    if (!(desc & 0x80)) {
        ULONG lines = desc & 0x1f;
        selected_base = 0;
        selected_length = lines < 31 ? (1UL << lines) : 0;
    } else {
        if (pos >= len) return 0;
        ranges = p[pos++];
        count = (ranges & 0x0f) + 1;
        addr_bytes = ((ranges >> 4) & 0x03) + 1;
        len_bytes = ((ranges >> 6) & 0x03) + 1;

        for (i = 0; i < count; ++i) {
            ULONG io_base, io_len;
            if (pos + addr_bytes + len_bytes > len) return 0;
            io_base = d3_tuple_read_le(p + pos, addr_bytes);
            pos += addr_bytes;
            io_len = d3_tuple_read_le(p + pos, len_bytes) + 1;
            pos += len_bytes;

            /* Same policy as direct 3C589 backend 1.5: choose a 16-byte window. */
            if (selected_length == 0 && io_len == 16) {
                selected_base = io_base;
                selected_length = io_len;
            }
        }
    }

    if (base->product == 0x0562 && index == 9) index = 7;
    if (index == 0 || selected_length != 16) return 0;

    base->config_index = index;
    base->io_offset = selected_base;
    base->io_length = selected_length;
    return 1;
}

static int d3_el3_wait_command(struct L2Direct3C *base, volatile UBYTE *io)
{
    ULONG i;
    for (i = 0; i < 50000UL; ++i) {
        if (!(d3_le16_in(io, EL3REG_STATUS) & EL3_STATUS_CMD_IN_PROGRESS))
            return 1;
    }
    base->diag.command_timeouts++;
    return 0;
}

static int d3_el3_command(struct L2Direct3C *base, volatile UBYTE *io, UWORD command)
{
    d3_le16_out(io, EL3REG_COMMAND, command);
    return d3_el3_wait_command(base, io);
}

static int d3_el3_window(struct L2Direct3C *base, volatile UBYTE *io, UWORD window)
{
    return d3_el3_command(base, io, (UWORD)(EL3CMD_SELECTWINDOW | (window & 7)));
}

static UWORD d3_el3_read_eeprom(struct L2Direct3C *base, volatile UBYTE *io, UWORD index, int *ok)
{
    ULONG timeout;

    d3_le16_out(io, EL3REG_EEPROMCMD, (UWORD)(EL3ECMD_READ | index));
    for (timeout = 0; timeout < 100000UL; ++timeout) {
        if ((d3_le16_in(io, EL3REG_EEPROMCMD) & EL3_EEPROM_BUSY) == 0) {
            *ok = 1;
            return d3_le16_in(io, EL3REG_EEPROMDATA);
        }
    }

    *ok = 0;
    return 0xffff;
}

static int d3_mac_is_valid(const UBYTE *mac)
{
    UBYTE all_zero = 1;
    UBYTE all_ff = 1;
    UWORD i;

    for (i = 0; i < 6; ++i) {
        if (mac[i] != 0x00) all_zero = 0;
        if (mac[i] != 0xff) all_ff = 0;
    }

    if (all_zero || all_ff)
        return 0;
    if (mac[0] & 1)
        return 0;
    return 1;
}

static int d3_el3_read_mac(struct L2Direct3C *base, volatile UBYTE *io, UBYTE *mac)
{
    UWORD i;
    UWORD word;
    int ok;

    if (!d3_el3_window(base, io, 0))
        return 0;

    for (i = 0; i < 3; ++i) {
        word = d3_el3_read_eeprom(base, io, (UWORD)(EL3EEPROM_ALTADDRESS0 + i), &ok);
        if (!ok) return 0;
        mac[i * 2] = (UBYTE)(word >> 8);
        mac[i * 2 + 1] = (UBYTE)word;
    }

    if (d3_mac_is_valid(mac))
        return 1;

    for (i = 0; i < 3; ++i) {
        word = d3_el3_read_eeprom(base, io, (UWORD)(EL3EEPROM_ADDRESS0 + i), &ok);
        if (!ok) return 0;
        mac[i * 2] = (UBYTE)(word >> 8);
        mac[i * 2 + 1] = (UBYTE)word;
    }

    return d3_mac_is_valid(mac);
}

static UWORD d3_el3_choose_transceiver(struct L2Direct3C *base, volatile UBYTE *io)
{
    UWORD media;
    UWORD media_status;
    UWORD active;
    UWORD eeprom_choice;

    d3_el3_window(base, io, 0);
    d3_le16_out(io, EL3REG_RESCONFIG, 0x3f00);
    media = d3_le16_in(io, EL3REG_CONFIG);

    d3_el3_window(base, io, 4);
    media_status = d3_le16_in(io, EL3REG_MEDIA);
    d3_el3_window(base, io, 0);

    active = 0;
    if (media_status & EL3_MEDIA_BEAT)
        active |= EL3_CONFIG_HAS_TP;

    if (media & active)
        media &= active;
    else {
        int ok;
        eeprom_choice = (UWORD)(d3_el3_read_eeprom(base, io, EL3EEPROM_ADDRCONFIG, &ok) &
                         EL3_ADDR_XCVR_MASK);
        if (ok) {
            if (eeprom_choice == EL3_ADDR_XCVR_TP)
                active = EL3_CONFIG_HAS_TP;
            else if (eeprom_choice == EL3_ADDR_XCVR_AUI)
                active = EL3_CONFIG_HAS_AUI;
            else if (eeprom_choice == EL3_ADDR_XCVR_COAX)
                active = EL3_CONFIG_HAS_COAX;
            else
                active = EL3_CONFIG_HAS_TP | EL3_CONFIG_HAS_COAX |
                         EL3_CONFIG_HAS_AUI;

            if (media & active)
                media &= active;
        }
    }

    if (media & EL3_CONFIG_HAS_TP)
        return EL3_ADDR_XCVR_TP;
    if (media & EL3_CONFIG_HAS_COAX)
        return EL3_ADDR_XCVR_COAX;
    return EL3_ADDR_XCVR_AUI;
}

static int d3_el3_wait_tp_link(struct L2Direct3C *base, volatile UBYTE *io)
{
    ULONG i;
    UWORD media = 0;

    base->diag.stages |= STAGE_LINK_WAIT;
    base->diag.link_seen = 0;
    base->diag.link_polls = 0;

    if (!d3_el3_window(base, io, 4))
        return 0;

    /* Long but finite ROM-safe settle.  No timer.device or DOS dependency.
     * Poll MEDIA beat and add a small spacing loop so we do not hammer the bus. */
    for (i = 0; i < 400000UL; ++i) {
        media = d3_le16_in(io, EL3REG_MEDIA);
        if (media & EL3_MEDIA_BEAT) {
            base->diag.link_seen = 1;
            base->diag.stages |= STAGE_LINK_SEEN;
            base->diag.link_polls = i + 1;
            base->diag.media_after_wait = media;
            return 1;
        }
        d3_spin(32UL);
    }

    base->diag.link_polls = 400000UL;
    base->diag.media_after_wait = media;
    return 1; /* absence of beat is diagnostic, not a fatal init error */
}

static UBYTE __attribute__((used)) d3_card_status_isr(
    register UBYTE mask __asm("d0"),
    register struct L2Direct3C *base __asm("a1"))
{
    volatile UBYTE *io;
    UWORD ints;
    UBYTE txs;
    ULONG n;

    if (base != 0 && base->io_base != 0 && (mask & CARD_STATUSF_IRQ) != 0) {
        io = base->io_base;
        ints = d3_le16_in(io, EL3REG_STATUS);

        if ((ints & EL3INTF_ANY) != 0) {
            /* Minimal probe form of the original CardStatusInt().
             * No Cause(): there are no RX/TX software queues in this module. */
            if ((ints & EL3INTF_RXCOMPLETE) != 0) {
                d3_le16_out(io, EL3REG_COMMAND,
                    EL3CMD_SETINTMASK | (INT_MASK & ~EL3INTF_RXCOMPLETE));
                d3_le16_out(io, EL3REG_COMMAND,
                    EL3CMD_ACKINT | EL3INTF_RXCOMPLETE);
            }

            if ((ints & EL3INTF_TXAVAIL) != 0)
                d3_le16_out(io, EL3REG_COMMAND,
                    EL3CMD_ACKINT | EL3INTF_TXAVAIL);

            if ((ints & EL3INTF_TXCOMPLETE) != 0) {
                /* TxError() drains the completion stack. Keep it bounded. */
                for (n = 0; n < 8; ++n) {
                    txs = d3_raw8_in(io, EL3REG_TXSTATUS);
                    if ((txs & 0x80) == 0)
                        break;
                    d3_raw8_out(io, EL3REG_TXSTATUS, 0);
                }
                d3_le16_out(io, EL3REG_COMMAND,
                    EL3CMD_ACKINT | EL3INTF_TXCOMPLETE);
            }

            d3_le16_out(io, EL3REG_COMMAND, EL3CMD_ACKINT | EL3INTF_ANY);
        }

        /* Exact Gayle IRQ workaround used by direct 3C589 backend 1.5. */
        *((volatile UBYTE *)0x00da9000UL) = (UBYTE)((mask ^ 0x2cU) | 0xc0U);
        mask = 0;
    }

    return mask;
}

static int d3_el3_initialise_tx_only(struct L2Direct3C *base)
{
    volatile UBYTE *io = base->io_base;
    UWORD i;

    /* InitialiseAdapter() from direct 3C589 backend 1.5. */
    if (!d3_el3_window(base, io, 0)) return 0;
    d3_le16_out(io, EL3REG_RESCONFIG, 0x3f00);
    base->diag.config_before = d3_le16_in(io, EL3REG_CONFIG);
    base->diag.addrconfig_before = d3_le16_in(io, EL3REG_ADDRCONFIG);

    if (!d3_el3_window(base, io, 4)) return 0;
    base->diag.media_before = d3_le16_in(io, EL3REG_MEDIA);
    base->transceiver = d3_el3_choose_transceiver(base, io);

    /* ConfigureAdapter(): station address then chosen transceiver. */
    if (!d3_el3_window(base, io, 2)) return 0;
    for (i = 0; i < 6; ++i) d3_raw8_out(io, i, base->mac[i]);

    if (!d3_el3_window(base, io, 0)) return 0;
    d3_le16_out(io, EL3REG_ADDRCONFIG, base->transceiver);
    base->diag.addrconfig_after = d3_le16_in(io, EL3REG_ADDRCONFIG);
    base->diag.config_after = d3_le16_in(io, EL3REG_CONFIG);
    if (!d3_el3_window(base, io, 1)) return 0;

    /* GoOnline() from direct 3C589 backend 1.5: real card.resource IRQ callback
     * is installed before OwnCard(), so retain the original interrupt mask. */
    d3_le16_out(io, EL3REG_COMMAND, EL3CMD_SETINTMASK | INT_MASK);
    d3_le16_out(io, EL3REG_COMMAND, EL3CMD_SETZEROMASK | 0xff);

    if (base->transceiver == EL3_ADDR_XCVR_TP) {
        d3_le16_out(io, EL3REG_COMMAND, EL3CMD_SELECTWINDOW | 4);
        d3_le16_out(io, EL3REG_MEDIA, EL3_MEDIA_BEAT_EN | EL3_MEDIA_JABBER_EN);
        base->diag.media_after_enable = d3_le16_in(io, EL3REG_MEDIA);
        base->diag.media_after_wait = base->diag.media_after_enable;
        base->diag.link_seen = (base->diag.media_after_enable & EL3_MEDIA_BEAT) ? 1 : 0;
        base->diag.link_polls = 1;
        d3_le16_out(io, EL3REG_COMMAND, EL3CMD_SELECTWINDOW | 1);
    } else {
        base->diag.media_after_enable = base->diag.media_before;
        base->diag.media_after_wait = base->diag.media_before;
    }

    d3_le16_out(io, EL3REG_COMMAND, EL3CMD_SETRXFILTER | 0x0005);
    d3_le16_out(io, EL3REG_COMMAND, EL3CMD_RXENABLE);
    d3_le16_out(io, EL3REG_COMMAND, EL3CMD_TXENABLE);
    return 1;
}

static void d3_put_be16(UBYTE *p, UWORD value)
{
    p[0] = (UBYTE)(value >> 8);
    p[1] = (UBYTE)value;
}

static void d3_put_be32(UBYTE *p, ULONG value)
{
    p[0] = (UBYTE)(value >> 24);
    p[1] = (UBYTE)(value >> 16);
    p[2] = (UBYTE)(value >> 8);
    p[3] = (UBYTE)value;
}

static UWORD d3_get_be16(const UBYTE *p)
{
    return (UWORD)(((UWORD)p[0] << 8) | p[1]);
}

static ULONG d3_get_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static ULONG d3_build_l2hd_control_frame(struct L2Direct3C *base, UBYTE *frame,
                                      const UBYTE *dst, UBYTE type, ULONG seq,
                                      ULONG block, ULONG offset,
                                      ULONG count, ULONG block_size)
{
    ULONG pos = 0, i;
    UWORD plen = (type == L2HD_TYPE_READ_REQ) ? 8 : 0;
    for (i = 0; i < 6; ++i) frame[pos++] = dst[i];
    for (i = 0; i < 6; ++i) frame[pos++] = base->mac[i];
    d3_put_be16(frame + pos, ETHERTYPE_L2BOOT); pos += 2;
    d3_put_be32(frame + pos, L2HD_MAGIC); pos += 4;
    frame[pos++] = L2HD_VERSION;
    frame[pos++] = type;
    d3_put_be16(frame + pos, plen); pos += 2;
    d3_put_be32(frame + pos, seq); pos += 4;
    d3_put_be32(frame + pos, block); pos += 4;
    d3_put_be32(frame + pos, offset); pos += 4;
    if (type == L2HD_TYPE_READ_REQ) {
        d3_put_be32(frame + pos, count); pos += 4;
        d3_put_be32(frame + pos, block_size); pos += 4;
    }
    while (pos < 60) frame[pos++] = 0;
    while (pos & 3UL) frame[pos++] = 0;
    return pos;
}

/* Forward declarations for helpers implemented later in the source.
 * Keep these explicit: implicit declarations are not valid for the later
 * static definitions and broke the first v2.7 build.
 */
static int d3_el3_queue_frame(struct L2Direct3C *base, const UBYTE *frame,
                           ULONG length, UWORD *space_before,
                           UWORD *space_after);
static int d3_el3_poll_tx(struct L2Direct3C *base, ULONG limit,
                       ULONG *polls_out, UBYTE *txstatus_out);
static void d3_clear_bytes(UBYTE *p, ULONG size);

static int d3_queue_control(struct L2Direct3C *base, const UBYTE *dst, UBYTE type,
                         ULONG seq, ULONG block, ULONG offset,
                         ULONG count, ULONG block_size,
                         UWORD *space_before, UWORD *space_after)
{
    UBYTE frame[64];
    ULONG len = d3_build_l2hd_control_frame(base, frame, dst, type, seq,
                                         block, offset, count, block_size);
    return d3_el3_queue_frame(base, frame, len, space_before, space_after);
}

/* TXSTATUS is diagnostic only.  The proven v2.5 transmitter queued frames and
 * allowed the adapter time to send them; it did not make forward progress
 * depend on observing a completion-stack entry. */
static int d3_queue_payload(struct L2Direct3C *base, const UBYTE *dst, UBYTE type,
                            ULONG seq, ULONG block, ULONG offset,
                            const UBYTE *payload, UWORD plen,
                            UWORD *space_before, UWORD *space_after)
{
    UBYTE *frame;
    ULONG pos=0, i;
    if (!base || !payload || (plen != 512 && plen != 1024)) return -1;
    /* v43.14: never place a 1 KiB Ethernet frame on the isolated worker's
     * 4 KiB stack.  The buffer is mutable RAM inside L2Direct3C. */
    frame=base->write_tx_frame;
    for(i=0;i<6;i++) frame[pos++]=dst[i];
    for(i=0;i<6;i++) frame[pos++]=base->mac[i];
    d3_put_be16(frame+pos,ETHERTYPE_L2BOOT); pos+=2;
    d3_put_be32(frame+pos,L2HD_MAGIC); pos+=4;
    frame[pos++]=L2HD_VERSION; frame[pos++]=type;
    d3_put_be16(frame+pos,plen); pos+=2;
    d3_put_be32(frame+pos,seq); pos+=4;
    d3_put_be32(frame+pos,block); pos+=4;
    d3_put_be32(frame+pos,offset); pos+=4;
    for(i=0;i<(ULONG)plen;i++) frame[pos++]=payload[i];
    while(pos<60) frame[pos++]=0;
    while(pos&3UL) frame[pos++]=0;
    return d3_el3_queue_frame(base,frame,pos,space_before,space_after);
}

static void d3_sample_tx_status_nonfatal(struct L2Direct3C *base, ULONG limit,
                                      ULONG *polls_out, UBYTE *status_out)
{
    int rc = d3_el3_poll_tx(base, limit, polls_out, status_out);
    if (rc == PR_TX_ERROR) {
        d3_el3_command(base, base->io_base, EL3CMD_TXENABLE);
    }
}

static void d3_rx_discard(struct L2Direct3C *base)
{
    d3_el3_command(base, base->io_base, EL3CMD_RXDISCARD);
    d3_el3_command(base, base->io_base, EL3CMD_ACKINT | EL3INTF_RXCOMPLETE);
}

static int d3_rx_wait_complete(struct L2Direct3C *base, ULONG limit, ULONG *polls_out,
                            UWORD *rxst_out)
{
    ULONG i;
    UWORD rxst = 0;
    d3_el3_window(base, base->io_base, 1);
    for (i = 0; i < limit; ++i) {
        rxst = d3_le16_in(base->io_base, EL3REG_RXSTATUS);
        if ((rxst & EL3_RX_LENMASK) != 0 && !(rxst & EL3_RX_INCOMPLETE)) {
            *polls_out = i + 1;
            *rxst_out = rxst;
            return 1;
        }
        d3_spin(16UL);
    }
    *polls_out = limit;
    *rxst_out = rxst;
    return 0;
}

static int d3_rx_read_header(struct L2Direct3C *base, UWORD rxst, UBYTE *header,
                          ULONG header_cap, ULONG *frame_len_out)
{
    ULONG len = (ULONG)(rxst & EL3_RX_LENMASK);
    ULONG read_len, i;
    UWORD w;
    if ((rxst & EL3_RX_ERROR) || len < L2HD_WIRE_HDR_LEN) {
        base->diag.rx_bad++;
        d3_rx_discard(base);
        return 0;
    }
    read_len = L2HD_WIRE_HDR_LEN;
    if (read_len > header_cap) read_len = header_cap;
    d3_clear_bytes(header, header_cap);
    for (i = 0; i < read_len; i += 2) {
        w = *(volatile UWORD *)(base->io_base + EL3REG_DATA0);
        header[i] = (UBYTE)(w >> 8);
        if (i + 1 < read_len) header[i + 1] = (UBYTE)w;
    }
    *frame_len_out = len;
    return 1;
}

static int d3_header_is_l2hd(const UBYTE *h, UBYTE type, ULONG seq)
{
    return d3_get_be16(h + 12) == ETHERTYPE_L2BOOT &&
           h[14] == 'L' && h[15] == '2' && h[16] == 'H' && h[17] == 'D' &&
           h[18] == L2HD_VERSION && h[19] == type && d3_get_be32(h + 22) == seq;
}

static int d3_wait_ident_reply(struct L2Direct3C *base)
{
    ULONG attempts, frame_len = 0;
    UWORD rxst = 0;
    for (attempts = 0; attempts < 16UL; ++attempts) {
        if (!d3_rx_wait_complete(base, 120000UL, &base->diag.rx_polls_ident, &rxst))
            continue;
        base->diag.last_rxstatus = rxst;
        base->diag.last_rx_length = (UWORD)(rxst & EL3_RX_LENMASK);
        base->diag.rx_frames++;
        if (!d3_rx_read_header(base, rxst, base->rx_header, sizeof(base->rx_header), &frame_len))
            continue;
        if (d3_header_is_l2hd(base->rx_header, L2HD_TYPE_IDENT_REPLY, base->ident_seq)) {
            ULONG plen = d3_get_be16(base->rx_header + 20);
            ULONG remain = frame_len - L2HD_WIRE_HDR_LEN;
            ULONG i;
            base->diag.ident_payload_len = (UWORD)plen;
            for (i = 0; i < 6; ++i) {
                base->pi_mac[i] = base->rx_header[6+i];
                base->diag.pi_mac[i] = base->rx_header[6+i];
            }
            /* IDENT payload is 48 bytes; drain it into rx_header[34..81]. */
            if (remain > 48UL) remain = 48UL;
            for (i = 0; i < remain; i += 2) {
                UWORD w = *(volatile UWORD *)(base->io_base + EL3REG_DATA0);
                base->rx_header[34+i] = (UBYTE)(w >> 8);
                if (i + 1 < remain) base->rx_header[34+i+1] = (UBYTE)w;
            }
            d3_rx_discard(base);
            if (plen < 48UL) { base->diag.rx_bad++; continue; }
            base->diag.total_blocks_hi = d3_get_be32(base->rx_header + 34);
            base->diag.total_blocks_lo = d3_get_be32(base->rx_header + 38);
            base->diag.block_size = d3_get_be32(base->rx_header + 42);
            base->diag.readonly = d3_get_be32(base->rx_header + 46);
            base->diag.max_count = d3_get_be32(base->rx_header + 50);
            base->diag.window = d3_get_be32(base->rx_header + 54);
            return 1;
        }
        d3_rx_discard(base);
    }
    return 0;
}

static int d3_wait_data1024_payload(struct L2Direct3C *base,
                                      ULONG expected_seq,
                                      ULONG expected_block,
                                      ULONG dst_offset)
{
    ULONG attempts, frame_len = 0, i;
    UWORD rxst = 0;

    for (attempts = 0; attempts < 16UL; ++attempts) {
        ULONG plen;
        ULONG sum = 0;
        ULONG x = 0;

        if (!d3_rx_wait_complete(base, 120000UL,
                              &base->diag.rx_polls_data, &rxst))
            continue;

        base->diag.last_rxstatus = rxst;
        base->diag.last_rx_length = (UWORD)(rxst & EL3_RX_LENMASK);
        base->diag.rx_frames++;

        if (!d3_rx_read_header(base, rxst, base->rx_header,
                            sizeof(base->rx_header), &frame_len))
            continue;

        if (!d3_header_is_l2hd(base->rx_header, L2HD_TYPE_DATA,
                            expected_seq)) {
            d3_rx_discard(base);
            continue;
        }

        plen = (ULONG)d3_get_be16(base->rx_header + 20);
        base->diag.data_payload_len = (UWORD)plen;
        base->diag.data_seq = d3_get_be32(base->rx_header + 22);
        base->diag.data_block = d3_get_be32(base->rx_header + 26);
        base->diag.data_offset = d3_get_be32(base->rx_header + 30);

        for (i = 0; i < 34; ++i)
            base->diag.data_header[i] = base->rx_header[i];

        if (base->diag.data_block != expected_block ||
            base->diag.data_offset != (expected_block << 9) ||
            plen != 1024UL ||
            frame_len < L2HD_WIRE_HDR_LEN + plen ||
            dst_offset > (4096UL - 1024UL)) {
            base->diag.rx_bad++;
            d3_rx_discard(base);
            continue;
        }

        for (i = 0; i < plen; i += 2) {
            UWORD w = *(volatile UWORD *)(base->io_base + EL3REG_DATA0);
            UBYTE a = (UBYTE)(w >> 8);
            UBYTE b = (UBYTE)w;

            base->data0_7[dst_offset + i] = a;
            base->data0_7[dst_offset + i + 1] = b;
            sum += (ULONG)a + (ULONG)b;
            x ^= ((ULONG)a << ((i & 3UL) * 8UL));
            x ^= ((ULONG)b << (((i + 1UL) & 3UL) * 8UL));
        }

        d3_rx_discard(base);

        base->diag.payload_valid = 1;
        base->diag.payload_bytes = plen;
        base->diag.payload_sum += sum;
        base->diag.payload_xor ^= x;
        base->diag.blocks_saved += 2;
        base->diag.packets_ok++;
        base->diag.total_payload_bytes += plen;
        base->diag.last_request_block = expected_block;

        base->diag.stages |= STAGE_DATA1024_SAVED;
        return 1;
    }

    return 0;
}


static int d3_wait_empty_reply(struct L2Direct3C *base, UBYTE type,
                               ULONG expected_seq, ULONG expected_block)
{
    ULONG attempts, frame_len=0; UWORD rxst=0;
    for(attempts=0;attempts<16UL;attempts++) {
        if(!d3_rx_wait_complete(base,120000UL,&base->diag.rx_polls_data,&rxst)) continue;
        if(!d3_rx_read_header(base,rxst,base->rx_header,sizeof(base->rx_header),&frame_len)) continue;
        if(d3_header_is_l2hd(base->rx_header,type,expected_seq) &&
           d3_get_be16(base->rx_header+20)==0 &&
           d3_get_be32(base->rx_header+26)==expected_block) {
            d3_rx_discard(base); return 1;
        }
        d3_rx_discard(base);
    }
    return 0;
}

static void d3_el3_clear_tx_status(volatile UBYTE *io)
{
    ULONG n;
    UBYTE s;
    for (n = 0; n < 8; ++n) {
        s = d3_raw8_in(io, EL3REG_TXSTATUS);
        if (!(s & 0x80))
            break;
        d3_raw8_out(io, EL3REG_TXSTATUS, 0);
    }
}

static int d3_el3_queue_frame(struct L2Direct3C *base, const UBYTE *frame,
                           ULONG length, UWORD *space_before_out,
                           UWORD *space_after_out)
{
    volatile UBYTE *io = base->io_base;
    ULONG i;
    ULONG word;

    if (!d3_el3_window(base, io, 1))
        return PR_INIT_COMMAND_TIMEOUT;

    *space_before_out = d3_le16_in(io, EL3REG_TXSPACE);
    if (*space_before_out < (UWORD)(length + 4))
        return PR_NO_TX_SPACE;

    d3_le32_out(io, EL3REG_DATA0, length);
    for (i = 0; i < length; i += 4) {
        word = ((ULONG)frame[i] << 24) |
               ((ULONG)frame[i + 1] << 16) |
               ((ULONG)frame[i + 2] << 8) |
               (ULONG)frame[i + 3];
        d3_raw32_out(io, EL3REG_DATA0, word);
    }
    *space_after_out = d3_le16_in(io, EL3REG_TXSPACE);
    return 0;
}

static int d3_el3_poll_tx(struct L2Direct3C *base, ULONG limit,
                       ULONG *polls_out, UBYTE *status_out)
{
    ULONG i;
    UBYTE s = 0;
    for (i = 0; i < limit; ++i) {
        s = d3_raw8_in(base->io_base, EL3REG_TXSTATUS);
        if (s & 0x80) {
            *polls_out = i + 1;
            *status_out = s;
            d3_raw8_out(base->io_base, EL3REG_TXSTATUS, 0);
            if (s & (0x20 | 0x10 | 0x08 | 0x04))
                return PR_TX_ERROR;
            return PR_SENT;
        }
    }
    *polls_out = limit;
    *status_out = s;
    return PR_TX_TIMEOUT;
}



static void d3_clear_bytes(UBYTE *p, ULONG size)
{
    while (size-- != 0) *p++ = 0;
}

static ULONG l2_spin_seq(struct L2ScsiBase *b)
{
    if (!b) return 0x39060001UL;
    if (b->seq_counter == 0) b->seq_counter = 0x39060001UL;
    b->seq_counter += 0x00010001UL;
    return b->seq_counter;
}

static void l2_rom_worker_yield(void)
{
    volatile ULONG i;
    for (i = 0; i < 20000UL; ++i) __asm__ volatile ("nop");
}

/* Exact deferred cadence carried over from the certified probe v3.3.
 * It runs only inside the private l2scsi worker task, never in DevOpen,
 * DevInit or BeginIO. */
static void l2_direct_wait_500ms(void)
{
    ULONG outer;
    for (outer = 0; outer < 160UL; ++outer) {
        ULONG count = 50000UL;
        while (count-- != 0) __asm__ volatile ("nop");
    }
}

static void l2_direct_wait_1s(void)
{
    l2_direct_wait_500ms();
    l2_direct_wait_500ms();
}

static LONG l2_direct_open(struct L2ScsiBase *b)
{
    struct L2Direct3C *d=&b->direct3c; struct CardMemoryMap *map; ULONG config_offset, i;
    if (b->direct_online) return 0;
    memset(d,0,sizeof(*d)); d->sys_base=b->sysbase;
    d->card_resource=d3_exec_open_resource(d->sys_base,d3_card_resource_name); if(!d->card_resource) return -1;
    d->card_handle.cah_CardNode.ln_Pri=120; d->card_handle.cah_CardNode.ln_Name=(STRPTR)d3_name; d->card_handle.cah_CardFlags=CARDF_IFAVAILABLE;
    d->card_status_int.is_Node.ln_Name=(STRPTR)d3_name; d->card_status_int.is_Code=(APTR)d3_card_status_isr; d->card_status_int.is_Data=d; d->card_handle.cah_CardStatus=&d->card_status_int;
    if(d3_cr_own_card(d->card_resource,&d->card_handle)!=0) {
        /* v43.12: ownership was not acquired, so do not call ReleaseCard.
         * Remove the callback and clear all partial open state instead. */
        d->card_handle.cah_CardStatus = 0;
        memset(d, 0, sizeof(*d));
        return -2;
    }
    d3_cr_misc_control(d->card_resource,&d->card_handle,CARD_ENABLEF_DIGAUDIO|CARD_DISABLEF_WP);
    if(!d3_parse_manfid(d) || !d3_parse_config_base(d,&config_offset) || !d3_parse_config_index(d)) goto fail;
    map=d3_cr_get_card_map(d->card_resource); if(!map) goto fail;
    d->config_base=map->cmm_AttributeMemory+config_offset; d->io_base=map->cmm_IOMemory+d->io_offset;
    d->config_base[PCCARD_REG_COR]=d->config_index; d->config_base[PCCARD_REG_CCSR]|=PCCARD_REG_CCSRF_AUDIOENABLE;
    if(!d3_el3_read_mac(d,d->io_base,d->mac) || !d3_el3_initialise_tx_only(d)) goto fail;
    for (i = 0; i < 6; i++)
        d->diag.mac[i] = d->mac[i];
    b->direct_online = 1;
    return 0;
fail:
    d->card_handle.cah_CardStatus=0; d3_cr_release_card(d->card_resource,&d->card_handle,0); memset(d,0,sizeof(*d)); return -3;
}

static LONG l2_direct_ident(struct L2ScsiBase *b)
{
    static const UBYTE bc[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    struct L2Direct3C *d = &b->direct3c;
    ULONG attempt;
    int rc;

    /* Exact probe.device v3.3 IDENT state machine.  The certified probe uses
     * the fixed I330 sequence for all ten autonomous attempts, treats
     * TXSTATUS as diagnostic/non-fatal, and spaces failed attempts by the
     * same approximately 500 ms busy wait. */
    d->ident_seq = 0x49333330UL; /* I330 */
    d->diag.ident_seq = d->ident_seq;

    for (attempt = 0; attempt < 10UL; ++attempt) {
        d3_el3_clear_tx_status(d->io_base);

        rc = d3_queue_control(d, bc, TYPE_IDENT_REQ,
                              d->ident_seq, 0, 0, 0, 0,
                              &d->diag.txspace_1,
                              &d->diag.txspace_after_1);
        if (rc == 0) {
            d->diag.stages |= STAGE_TX1_QUEUED | STAGE_IDENT_TX;
            d3_sample_tx_status_nonfatal(d, 20000UL,
                                         &d->diag.tx_polls_1,
                                         &d->diag.txstatus_1);

            if (d3_wait_ident_reply(d)) {
                d->diag.stages |= STAGE_IDENT_RX;
                CopyMem(d->pi_mac, b->disk.pi_mac, 6);
                b->disk.total_blocks_lo = d->diag.total_blocks_lo;
                b->disk.block_size = d->diag.block_size;
                b->disk.readonly = d->diag.readonly & 1UL;
                b->disk.max_blocks_per_io = d->diag.max_count;
                b->debug_last_actual = b->disk.total_blocks_lo;
                b->debug_last_err = (LONG)b->disk.block_size;
                if (!b->disk.max_blocks_per_io ||
                    b->disk.max_blocks_per_io > L2SCSI_MAX_BLOCKS_PER_IO)
                    b->disk.max_blocks_per_io = L2SCSI_MAX_BLOCKS_PER_IO;
                if (b->disk.block_size != L2SCSI_BLOCK_SIZE ||
                    !b->disk.total_blocks_lo) {
                    l2_dbg_stage(b, 0x80000054UL);
                    return -4;
                }
                /* v43.9: IDENT must remain broadcast-capable, but all normal
                 * DATA traffic is unicast in the milestone server setup.
                 * Once IDENT_REPLY has been fully validated and the Pi MAC
                 * saved, stop accepting unrelated LAN broadcasts into the
                 * 3C589 RX FIFO.  0x0001 = individual-address frames only.
                 * No RX disable/reset is required: SetRxFilter is an online
                 * command and leaves the proven transport timing unchanged.
                 */
                d3_le16_out(d->io_base, EL3REG_COMMAND,
                            EL3CMD_SETRXFILTER | 0x0001);

                b->offline_latched = 0;
                b->disk.online = 1;
                b->disk.unit_attention = 1;
                l2_clear_sense2(&b->disk);
                return 0;
            }
        }

        l2_direct_wait_500ms();
    }

    return -5;
}

static LONG l2_direct_read_pair(struct L2ScsiBase *b,ULONG lba,UBYTE *dst0,UBYTE *dst1)
{
    struct L2Direct3C *d=&b->direct3c; ULONG seq=l2_spin_seq(b)^lba,tries; int rc; UWORD ab=0,aa=0;
    for(tries=0;;tries++) { l2_dbg_stage(b, 0x00000070UL); d3_el3_clear_tx_status(d->io_base); rc=d3_queue_control(d,b->disk.pi_mac,L2HD_TYPE_READ_REQ,seq,lba,lba<<9,2,512,&d->diag.txspace_2,&d->diag.txspace_after_2);
        if(rc==0) l2_dbg_stage(b, 0x00000071UL);
        if(rc==0 && d3_wait_data1024_payload(d,seq,lba,0)) { l2_dbg_stage(b, 0x00000072UL); CopyMem(d->data0_7,dst0,512); CopyMem(d->data0_7+512,dst1,512); d3_el3_clear_tx_status(d->io_base); rc=d3_queue_control(d,b->disk.pi_mac,L2HD_TYPE_DATA_ACK,seq,lba,0,0,0,&ab,&aa); if(rc==0)return 0; }
        l2_rom_worker_yield();
    }
}

L2_USED LONG l2_direct_read_blocks(struct L2ScsiBase *b,ULONG lba,ULONG blocks,UBYTE *dst)
{
    struct L2Direct3C *d;
    ULONG done = 0;

    if (!b || !b->direct_online || !b->disk.online) return -1;
    if (!dst) return -2;
    if (blocks == 0) return 0;

    d = &b->direct3c;

    while (done < blocks) {
        ULONG cur = lba + done;
        UBYTE *out = dst + (done << 9);
        ULONG seq;
        ULONG attempt;
        int transaction_ok = 0;

        /* Preserve the stable v42.41 second-block cache contract. */
        if (b->readcache_valid && b->readcache_lba == cur) {
            CopyMem(b->readcache_data, out, L2SCSI_BLOCK_SIZE);
            b->readcache_valid = 0;
            done++;
            continue;
        }

        if (cur + 1 >= b->disk.total_blocks_lo)
            return -3;

        /* Exact probe v3.3 transaction cadence, now repeated per pair.
         * Keep the R330 base and derive a stable per-pair sequence.
         */
        seq = 0x52333330UL + (cur >> 1);

        for (attempt = 0; attempt < 10UL; ++attempt) {
            UWORD ack_before = 0;
            UWORD ack_after = 0;
            ULONG ack_polls = 0;
            UBYTE ack_status = 0;
            int rc;

            d3_el3_clear_tx_status(d->io_base);

            rc = d3_queue_control(d, d->pi_mac, L2HD_TYPE_READ_REQ,
                                  seq, cur, cur << 9, 2, 512,
                                  &d->diag.txspace_2,
                                  &d->diag.txspace_after_2);
            if (rc == 0) {
                d->diag.stages |= STAGE_TX2_QUEUED | STAGE_READ0_TX;
                /* RTT experiment: enter RX immediately after queueing READ_REQ.
                 * The milestone post-ACK TX poll and settle remain unchanged.
                 */
                d->diag.tx_polls_2 = 0;
                d->diag.txstatus_2 = 0;

                if (d3_wait_data1024_payload(d, seq, cur, 0)) {
                    d->diag.stages |= STAGE_DATA0_RX;

                    d3_el3_clear_tx_status(d->io_base);
                    rc = d3_queue_control(d, d->pi_mac,
                                          L2HD_TYPE_DATA_ACK,
                                          d->diag.data_seq,
                                          d->diag.data_block,
                                          0, 0, 0,
                                          &ack_before, &ack_after);
                    if (rc == 0) {
                        d3_sample_tx_status_nonfatal(d, 128UL,
                                                     &ack_polls,
                                                     &ack_status);
                        d->diag.ack_sent++;
                        d->diag.stages |= STAGE_ACK_TX;

                        /* RTT experiment v42.55: shorten only the certified
                         * post-ACK TX-status poll from 20000 to 512 iterations, and expose the received
                         * DATA immediately after it.  Failed attempts retain
                         * the original bounded settle below.
                         */
                        transaction_ok = 1;
                        break;
                    }
                }
            }

            l2_direct_wait_500ms();
        }

        if (!transaction_ok)
            return -4;

        /* Deliver the requested first block. */
        CopyMem(d->data0_7, out, L2SCSI_BLOCK_SIZE);

        if (done + 1 < blocks) {
            /* Caller asked for both blocks: deliver the second directly. */
            CopyMem(d->data0_7 + L2SCSI_BLOCK_SIZE,
                    out + L2SCSI_BLOCK_SIZE,
                    L2SCSI_BLOCK_SIZE);
            b->readcache_valid = 0;
            done += 2;
        } else {
            /* Caller asked for one block: retain the second exactly as in
             * the stable 42.41 DATA1024 cache design.
             */
            CopyMem(d->data0_7 + L2SCSI_BLOCK_SIZE,
                    b->readcache_data,
                    L2SCSI_BLOCK_SIZE);
            b->readcache_lba = cur + 1;
            b->readcache_valid = 1;
            done++;
        }
    }

    l2_dbg_stage(b, 0x000000A2UL);
    return 0;
}

L2_USED LONG l2_direct_write_blocks(struct L2ScsiBase *b,ULONG lba,ULONG blocks,const UBYTE *src)
{
    struct L2Direct3C *d; ULONG done=0;
    if(!b||!b->direct_online||!b->disk.online) return -1;
    if(b->disk.readonly) return -5;
    if(!src) return -2;
    d=&b->direct3c;
    while(done<blocks) {
        ULONG cur=lba+done; ULONG count=(blocks-done>=2UL)?2UL:1UL; /* WRITE1024, WRITE512 tail */
        UWORD plen=(UWORD)(count<<9); ULONG seq=(l2_spin_seq(b)^cur^0x57000000UL);
        ULONG attempt; int ok=0;
        for(attempt=0;attempt<10UL;attempt++) {
            UWORD sb=0,sa=0; int rc;
            d3_el3_clear_tx_status(d->io_base);
            rc=d3_queue_payload(d,b->disk.pi_mac,L2HD_TYPE_WRITE_REQ,seq,cur,cur<<9,src+(done<<9),plen,&sb,&sa);
            if(rc==0 && d3_wait_empty_reply(d,L2HD_TYPE_WRITE_ACK,seq,cur)) { ok=1; break; }
            l2_direct_wait_500ms();
        }
        if(!ok) return -4;
        done+=count;
    }
    b->readcache_valid=0;
    return 0;
}

L2_USED LONG l2_direct_flush(struct L2ScsiBase *b)
{
    struct L2Direct3C *d; ULONG seq,attempt;
    if(!b||!b->direct_online||!b->disk.online) return -1;
    if(b->disk.readonly) return 0;
    d=&b->direct3c; seq=l2_spin_seq(b)^0x46000000UL;
    for(attempt=0;attempt<10UL;attempt++) {
        UWORD sb=0,sa=0;
        d3_el3_clear_tx_status(d->io_base);
        if(d3_queue_control(d,b->disk.pi_mac,L2HD_TYPE_FLUSH_REQ,seq,0,0,0,0,&sb,&sa)==0 &&
           d3_wait_empty_reply(d,L2HD_TYPE_FLUSH_ACK,seq,0)) return 0;
        l2_direct_wait_500ms();
    }
    return -2;
}

static void l2_direct_close(struct L2ScsiBase *b)
{
    struct L2Direct3C *d=&b->direct3c; if(d->io_base){d3_el3_command(d,d->io_base,EL3CMD_SETINTMASK|0);d3_el3_command(d,d->io_base,EL3CMD_SETZEROMASK|0);d3_el3_command(d,d->io_base,EL3CMD_RXDISABLE);d3_el3_command(d,d->io_base,EL3CMD_TXDISABLE);} if(d->card_resource){d->card_handle.cah_CardStatus=0;d3_cr_release_card(d->card_resource,&d->card_handle,0);} memset(d,0,sizeof(*d)); b->direct_online=0;b->disk.online=0;b->disk.unit_attention=0;b->readcache_valid=0;l2_set_sense2(&b->disk,SENSE_NOT_READY,0x04,0);
}

static LONG l2scsi_hw_init(struct L2ScsiBase *b)
{
    LONG rc;
    if (!b) return -1;
    if (b->disk.online && b->direct_online) return 0;

    /* v43.11 fail-safe: after one complete bootstrap failure, do not reset
     * the card or repeat the IDENT busy waits during the same boot.  This
     * branch is never taken on the successful Pi-present path. */
    if (b->offline_latched) {
        l2_dbg_stage(b, 0x80000012UL);
        return -6;
    }

    l2_dbg_stage(b, 0x40);
    rc = l2_direct_open(b);
    if (rc) {
        /* v43.12: guarantee cleanup of every partial-open failure before
         * latching the remote unit offline.  l2_direct_open() already avoids
         * ReleaseCard when OwnCard itself failed; this close is therefore
         * safe for both pre-ownership and post-ownership failure paths. */
        l2_direct_close(b);
        b->offline_latched = 1;
        l2_dbg_stage(b, 0x80000041UL);
        return rc;
    }

    /* probe v3.3 performs IDENT later, from its low-priority task, after
     * allowing the freshly configured adapter/link to settle. */
    l2_dbg_stage(b, 0x48);
    l2_direct_wait_1s();

    l2_dbg_stage(b, 0x50);
    rc = l2_direct_ident(b);
    if (rc) {
        l2_direct_close(b);
        b->offline_latched = 1;
        l2_dbg_stage(b, 0x80000055UL);
        return rc;
    }
    b->offline_latched = 0;
    l2_dbg_stage(b, 0x60);
    return 0;
}

static void l2_new_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}


static void xReplyMsgBase(struct L2ScsiBase *base, struct Message *msg)
{
    register struct ExecBase *a6 __asm("a6") = base ? base->sysbase : (struct ExecBase *)0;
    register struct Message *a1 __asm("a1") = msg;
    __asm__ volatile ("jsr a6@(-378:W)" : : "r"(a6), "r"(a1) : "d0","d1","a0","cc","memory");
}

static void xPutMsgBase(struct L2ScsiBase *base, struct MsgPort *port, struct Message *msg)
{
    register struct ExecBase *a6 __asm("a6") = base ? base->sysbase : (struct ExecBase *)0;
    register struct MsgPort *a0 __asm("a0") = port;
    register struct Message *a1 __asm("a1") = msg;
    __asm__ volatile ("jsr a6@(-366:W)"
        :
        : "r"(a6), "r"(a0), "r"(a1)
        : "d0", "d1", "cc", "memory");
}

static int l2_need_online(UWORD cmd);
static int l2_ensure_online(struct L2ScsiBase *base);

static void l2_worker_process(struct L2ScsiBase *base, struct IOStdReq *io)
{
    /* v41r1 / A4091-style discipline:
     * All work that may touch backend, IDENT, READ_REQ/DATA/ACK, retry loops
     * or other slow paths happens here, inside the private device worker task.
     * DevBeginIO must remain enqueue-only.
     */
    if (!base || !io) return;

    if (l2_need_online(io->io_Command) && !base->disk.online) {
        if (l2_ensure_online(base) != 0) {
            io->io_Error = IOERR_OPENFAIL;
            io->io_Actual = 0;
            return;
        }
    }

    l2scsi_begin_core2(&base->disk, io);
}

L2_USED void l2_async_task_entry(void)
{
    struct L2ScsiBase *base;
    struct Task *me;
    struct IOStdReq *io;
    ULONG sigmask;

    me = FindTask(NULL);
    base = (struct L2ScsiBase *)me->tc_UserData;
    if (!base) return;

    base->async_sig = AllocSignal(-1);
    if (base->async_sig < 0) return;

    if (!base->async_block) return;
    memset(&base->async_block->port, 0, sizeof(base->async_block->port));
    base->async_block->port.mp_Node.ln_Type = NT_MSGPORT;
    base->async_block->port.mp_Node.ln_Name = (STRPTR)"L2SCSI_HandlerPort";
    base->async_block->port.mp_Flags = PA_SIGNAL;
    base->async_block->port.mp_SigBit = base->async_sig;
    base->async_block->port.mp_SigTask = me;
    l2_new_list(&base->async_block->port.mp_MsgList);

    base->async_port = &base->async_block->port;

    /* v42r47 COLDSTART scheduling fix: publish the worker port before any
     * slow hardware work, then perform the probe-v3.3 primer at low priority.
     * This reproduces probe.device's temporal model: DevInit starts a deferred
     * task and returns; it never waits for IDENT/DATA.  Any BootNode request
     * queued meanwhile remains on this port and is processed after the primer. */
    base->async_ready = 1;
    sigmask = 1UL << base->async_sig;

    if (!base->disk.online)
        (void)l2scsi_hw_init(base);

    for (;;) {
        Wait(sigmask);
        while ((io = (struct IOStdReq *)GetMsg(base->async_port)) != NULL) {
            l2_worker_process(base, io);
            xReplyMsgBase(base, &io->io_Message);
        }
    }
}

static int l2_start_async_handler(struct L2ScsiBase *base)
{
    ULONG i;
    if (!base) return 0;
    if (base->async_port && base->async_ready) return 1;
    if (base->async_started) return (base->async_port != NULL);

    base->async_started = 1;
    base->async_block = (struct L2AsyncBlock *)AllocMem(sizeof(struct L2AsyncBlock),
                                                        MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->async_block) {
        base->async_started = 0;
        return 0;
    }

    base->async_block->task.tc_Node.ln_Type = NT_TASK;
    base->async_block->task.tc_Node.ln_Pri = -50;
    base->async_block->task.tc_Node.ln_Name = (STRPTR)"L2SCSI_Handler";
    base->async_block->task.tc_SPLower = (APTR)&base->async_block->stack[0];
    base->async_block->task.tc_SPUpper = (APTR)&base->async_block->port;
    base->async_block->task.tc_SPReg = (APTR)&base->async_block->port;
    base->async_block->task.tc_UserData = (APTR)base;
    l2_new_list(&base->async_block->task.tc_MemEntry);

    base->async_task = &base->async_block->task;
    AddTask(base->async_task, l2_async_task_entry, NULL);

    /* Wait only until the task has published its message port.  Never wait
     * here for card initialisation, IDENT or disk traffic during COLDSTART. */
    for (i = 0; i < 200000UL; i++) {
        if (base->async_port && base->async_ready) return 1;
        __asm__ volatile("nop");
    }
    return (base->async_port != NULL && base->async_ready != 0);
}

static int l2_async_cmd(UWORD cmd)
{
    switch (cmd) {
    case CMD_READ:
    case NSCMD_TD_READ64:
    case CMD_WRITE:
    case TD_FORMAT:
    case NSCMD_TD_WRITE64:
    case CMD_UPDATE:
    case CMD_FLUSH:
    case HD_SCSICMD:
    case TD_GETGEOMETRY:
    case TD_PROTSTATUS:
    case TD_CHANGESTATE:
    case TD_CHANGENUM:
        /* v42r10: Mount must arm the transport, not only the later Dir.
         * These metadata/media-probe commands are often what DOS/FFS issues
         * during Mount before the first real directory read.  Keep BeginIO
         * enqueue-only, but route them through the worker so l2_ensure_online()
         * can perform the backend bootstrap/IDENT in the safe task context.
         */
        return 1;
    default:
        return 0;
    }
}

static void l2_fake_defaults(struct L2Disk *d)
{
    memset(d, 0, sizeof(*d));
    d->block_size = L2SCSI_BLOCK_SIZE;
    d->total_blocks_lo = 32768UL; /* 16 MiB until IDENT succeeds */
    d->readonly = 1;
    d->online = 0;
    d->max_blocks_per_io = L2SCSI_MAX_BLOCKS_PER_IO;
    l2_set_sense2(d, SENSE_NOT_READY, 0x04, 0x00);
}

static int l2_need_online(UWORD cmd)
{
    switch (cmd) {
    case CMD_READ:
    case NSCMD_TD_READ64:
    case CMD_WRITE:
    case TD_FORMAT:
    case NSCMD_TD_WRITE64:
    case CMD_UPDATE:
    case CMD_FLUSH:
    case HD_SCSICMD:
    case TD_GETGEOMETRY:
    case TD_PROTSTATUS:
    case TD_CHANGESTATE:
    case TD_CHANGENUM:
        /* v42r10: treat mount-time media/geometry probes as the primer.
         * This removes the manual mount/dismount/mount dance by making the
         * first Mount bring backend online and perform IDENT while still inside
         * the worker, never in BeginIO/OpenDevice.
         */
        return 1;
    default:
        return 0;
    }
}

static int l2_ensure_online(struct L2ScsiBase *base)
{
    LONG rc;
    l2_dbg_stage(base, 0x00000010UL); /* ensure_online entry */
    if (base->disk.online) { l2_dbg_stage(base, 0x00000011UL); return 0; }
    if (base->offline_latched) {
        l2_dbg_stage(base, 0x80000012UL);
        return -6;
    }
    rc = l2scsi_hw_init(base);
    if (rc != 0 && base && (base->debug_stage & 0x80000000UL) == 0) l2_dbg_stage(base, 0x80000010UL);
    return (int)rc;
}

L2_USED struct L2ScsiBase *DevInit(register struct L2ScsiBase *base __asm("d0"),
                                  register BPTR seglist __asm("a0"),
                                  register struct ExecBase *sysbase __asm("a6"))
{
    (void)seglist;
    base->sysbase = sysbase;
    base->dev.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->dev.dd_Library.lib_Node.ln_Name = (char *)L2SCSI_NAME;
    base->dev.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->dev.dd_Library.lib_Version = L2SCSI_VERSION;
    base->dev.dd_Library.lib_Revision = L2SCSI_REVISION;
    base->dev.dd_Library.lib_IdString = (APTR)L2SCSI_VERSTRING "\r\n";
    base->open_count = 0;
    base->ch = NULL;
    base->tuplebuf = NULL;
    base->async_port = NULL;
    base->async_task = NULL;
    base->async_started = 0;
    memset(&base->direct3c, 0, sizeof(base->direct3c));
    base->direct_online = 0;
    base->offline_latched = 0;
    base->async_ready = 0;
    base->async_block = NULL;
    base->async_sig = -1;
    base->readcache_valid = 0;
    base->diagnostic_read_used = 0;
    base->readcache_lba = 0;
    memset(base->readcache_data, 0, sizeof(base->readcache_data));
    base->debug_stage = 0x00000001UL;
    base->debug_last_cmd = 0;
    base->debug_last_err = 0;
    base->debug_last_actual = 0;
    base->boot_done = 0;
    base->boot_stage = 0;
    base->boot_scan_result = 0;
    l2_fake_defaults(&base->disk);

    /* v42r46 COLDSTART primer: reproduce probe.device v3.3's decisive
     * startup property.  The resident starts its private task from DevInit,
     * before any OpenDevice(), Mount or Dir.  The existing worker remains the
     * sole owner of all slow hardware operations: it acquires/configures the
     * 3C589, waits for link settle and performs IDENT.  DevInit itself does
     * not touch the card and does not wait for completion.
     */
    (void)l2_start_async_handler(base);
    return base;
}

L2_USED struct L2ScsiBase *DevOpen(register struct L2ScsiBase *base __asm("a6"),
                                  register struct IOStdReq *io __asm("a1"),
                                  register ULONG unit __asm("d0"),
                                  register ULONG flags __asm("d1"))
{
    (void)flags;
    if (unit != 0) {
        if (io) io->io_Error = IOERR_OPENFAIL;
        return NULL;
    }
    l2_dbg_stage(base, 0x00000002UL); /* DevOpen */
    base->open_count++;
    base->dev.dd_Library.lib_OpenCnt++;

    /* v42r39 open-primer: port only the v42r11 anti mount/dismount/mount
     * trick onto the v42r36 stable workerqueue transport.  The important
     * difference is not merely starting the worker: v42r11 also performed
     * l2_ensure_online() from DevOpen so the first Mount returns from
     * OpenDevice only after backend bootstrap/IDENT has been attempted.
     *
     * Keep normal I/O A4091-style: BeginIO still enqueues READ/SCSI/probe
     * commands and the worker remains the owner of backend during transfers.
     * This primer is deliberately non-fatal: if IDENT/bootstrap fails, Open
     * still succeeds and later worker I/O retries/reports the real error.
     */
    /* The first backend attempt is performed by l2_async_task_entry()
     * before it publishes async_ready.  Do not run the direct card engine
     * again in the caller's DevOpen context. */
    l2_start_async_handler(base);

    if (io) {
        io->io_Device = (struct Device *)base;
        io->io_Unit = (struct Unit *)&base->disk;
        io->io_Error = 0;
    }
    return base;
}

L2_USED BPTR DevClose(register struct L2ScsiBase *base __asm("a6"),
                     register struct IOStdReq *io __asm("a1"))
{
    (void)io;
    if (base->open_count) base->open_count--;
    if (base->dev.dd_Library.lib_OpenCnt) base->dev.dd_Library.lib_OpenCnt--;

    /* v43.1 persistent-open:
     * HDToolBox and ptable-style scanners repeatedly OpenDevice()/CloseDevice()
     * while probing the same physical unit.  The v43.0 carry-over from
     * l2scsi.device shut down, released and later reset/reinitialised the
     * 3C589 whenever the last opener closed.  The private worker task and
     * message port, however, remain alive for the lifetime of this ROM device.
     * Reopening therefore caused repeated IDENT/reset cycles and could freeze
     * the machine.  Keep the card/backend latched online across ordinary
     * closes.  A ROM resident device is not expunged in normal operation;
     * failed IDENT/read paths still call l2_direct_close() themselves.
     */
    return 0;
}

L2_USED BPTR DevExpunge(register struct L2ScsiBase *base __asm("a6"))
{
    (void)base;
    return 0;
}

L2_USED ULONG DevReserved(void)
{
    return 0;
}


static void l2_finish_io(struct L2ScsiBase *base, struct IOStdReq *io)
{
    if (!io) return;

    /* v42r35: if caller allowed quick completion, complete synchronously
     * and DO NOT ReplyMsg().  r32 replied even for quick synchronous I/O;
     * under long CLI copies this may expose fragile caller/filesystem timing
     * or reply-port lifetime assumptions.  If IOF_QUICK was not set, behave
     * like a normal asynchronous completion and reply.
     */
    if (io->io_Flags & IOF_QUICK) return;

    if (base) xReplyMsgBase(base, &io->io_Message);
}

L2_USED void DevBeginIO(register struct IOStdReq *io __asm("a1"))
{
    struct L2ScsiBase *base = (struct L2ScsiBase *)io->io_Device;

    /* v42r36 workerqueue-noinitfreeze:
     * Restore the A4091-style rule for every command that can touch media or
     * backend state: BeginIO only enqueues, the private worker owns
     * l2_ensure_online(), IDENT, READ_REQ/DATA/ACK and retry loops.
     * This keeps the first Mount primer behavior from v42r10, but prevents
     * 3c589/backend initialization and long block reads from running inside
     * BeginIO, which was the likely source of mouse freezes and heavy-load
     * reentrancy crashes in v42r35 quickcomplete.
     */
    io->io_Error = 0;
    io->io_Actual = 0;

    if (!base) {
        io->io_Error = IOERR_OPENFAIL;
        l2_finish_io(base, io);
        return;
    }

    /* TD_GETGEOMETRY remains the out-of-band debug readout once online, but it
     * is also a mount-time primer command when the disk is not online yet.  Do
     * not overwrite debug_stage for the readout command itself.
     */
    if (io->io_Command != TD_GETGEOMETRY)
        l2_dbg_stage(base, 0x00000003UL); /* DevBeginIO */
    l2_dbg_io(base, io->io_Command, 0, 0);

    if (l2_async_cmd(io->io_Command)) {
        if (!l2_start_async_handler(base) || !base->async_port) {
            io->io_Error = IOERR_OPENFAIL;
            io->io_Actual = 0;
            l2_dbg_io(base, io->io_Command, io->io_Error, io->io_Actual);
            l2_finish_io(base, io);
            return;
        }

        io->io_Flags &= ~IOF_QUICK;
        xPutMsgBase(base, base->async_port, &io->io_Message);
        return;
    }

    l2scsi_begin_core2(&base->disk, io);
    l2_dbg_io(base, io->io_Command, io->io_Error, io->io_Actual);
    l2_finish_io(base, io);
}

L2_USED LONG DevAbortIO(register struct IOStdReq *io __asm("a1"))
{
    io->io_Error = IOERR_ABORTED;
    return 0;
}

L2_USED APTR DevVectors[] = {
    (APTR)DevOpen,
    (APTR)DevClose,
    (APTR)DevExpunge,
    (APTR)DevReserved,
    (APTR)DevBeginIO,
    (APTR)DevAbortIO,
    (APTR)-1
};


/* -------------------------------------------------------------------------
 * LoadModule compatibility probe.
 *
 * The embedded l2remote.boot Resident is intentionally omitted.  This file
 * exports exactly one Resident, l2scsi.device.  Partition-table boot scanning
 * must be handled separately while this variant verifies that LoadModule can
 * register and initialize the replacement device.
 * ------------------------------------------------------------------------- */

L2_USED APTR DevInitTab[] = {
    (APTR)sizeof(struct L2ScsiBase),
    (APTR)DevVectors,
    0,
    (APTR)DevInit
};

L2_USED struct Resident ROMTag = {
    RTC_MATCHWORD,
    &ROMTag,
    /* Exact limit used by Remus after its "fixed rt_endskip" repair:
     * first byte immediately following this 26-byte Resident structure.
     */
    (APTR)(&ROMTag + 1),
    RTF_AUTOINIT | RTF_COLDSTART,
    L2SCSI_VERSION,
    NT_DEVICE,
    21,
    (char *)L2SCSI_NAME,
    (char *)L2SCSI_VERSTRING,
    DevInitTab
};
