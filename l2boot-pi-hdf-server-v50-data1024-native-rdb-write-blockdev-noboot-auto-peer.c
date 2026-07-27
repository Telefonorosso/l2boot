/*
 * l2boot-pi-hdf-server-v45-data1024-auto-peer-unicast-timestamped.c
 *
 * Minimal C Raspberry Pi HDF server for L2BootHDF / l2scsi.device tests.
 * Purpose: remove Python/scapy/select jitter from the Pi side and behave more
 * like the proven L2 VNC C server path.
 *
 * Wire EtherType 0x88B8, payload after Ethernet header:
 *   magic[4]="L2HD", version u8, type u8, payload_len be16,
 *   seq be32, blockno be32, offset be32, payload...
 *
 * v45: console log lines are prefixed with local wall-clock timestamps
 * in the form [YYYY-MM-DD HH:MM:SS.mmm]. Protocol, packet timing, retry
 * behavior and data path are unchanged from v44.
 *
 * v46: the Amiga MAC is learned automatically from the first valid IDENT_REQ.
 * If the server process is restarted while the Amiga is already online, the
 * first structurally valid READ_REQ may also relearn the peer from its Ethernet
 * source MAC, allowing the existing mounted disk session to resume without a
 * fresh IDENT exchange.
 * DATA replies are always unicast to the latched peer; no Amiga MAC argument
 * is required on the command line. A later valid IDENT_REQ received while idle
 * may replace the latched peer, allowing a rebooted/replaced client to attach.
 * The former --unicast-data option and broadcast DATA path have been removed.
 * IDENT_REPLY remains broadcast so the Amiga can learn the Pi MAC before switching its RX filter.
 *
 * v47: direct-filesystem HDF images beginning with DOS\0..DOS\7 are exposed
 * on the fly as a virtual one-partition RDB disk.  RDSK and PART blocks are
 * synthesized in RAM and reads inside the virtual partition are translated
 * to the original file without copying or modifying it.  Native RDB images
 * keep the exact v46 data path.  Use --raw to disable DOS-image wrapping.
 *
 * v48: adds WRITE for native RDB images. Native RDB files are writable by
 * default and may be forced read-only with --protect. DOS/x virtual-RDB and
 * other raw images remain read-only. WRITE_REQ carries 512 or 1024 bytes;
 * WRITE_ACK is sent only after a complete pwrite(). FLUSH_REQ uses fdatasync().
 *
 * v48 block-device extension: regular files retain the original st_size path;
 * Linux block devices obtain their byte capacity with BLKGETSIZE64. This lets
 * the same native-RDB READ/WRITE512/FLUSH path serve devices such as /dev/sda
 * and /dev/loop0 without changing the wire protocol.
 *
 *
 * v49: adds explicit --allow-raw-write for both regular files and Linux
 * block devices. Without it, non-RDB sources remain read-only. --protect
 * always wins. DOS/x virtual-RDB images remain read-only unless exposed as
 * raw with --raw, preventing translated writes from corrupting the source.

 * v50: adds --noboot. On every read, any valid-looking RDB PART block is
 * copied to the reply buffer with de_BootPri overridden to -10 and its RDB
 * checksum recomputed. The source HDF or block device is never modified.
 * This applies both to native RDB partition chains and to synthesized virtual
 * RDB images; normal operation remains at BootPri 10 for virtual RDB.
 * Types:
 *   4 DATA
 *   5 DATA_ACK
 *   6 READ_REQ
 *   7 IDENT_REQ
 *   8 IDENT_REPLY
 *
 * Build on Pi:
 *   gcc -O2 -Wall -Wextra -o l2boot-pi-hdf-server-v48-data1024-native-rdb-write-blockdev-auto-peer l2boot-pi-hdf-server-v48-data1024-native-rdb-write-blockdev-auto-peer.c
 *
 * Run:
 *   sudo ./l2boot-pi-hdf-server-v48-data1024-native-rdb-write-blockdev-auto-peer eth0 ./disk.hdf
 *   sudo ./l2boot-pi-hdf-server-v48-data1024-native-rdb-write-blockdev-auto-peer eth0 /dev/sda
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/fs.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ETHERTYPE_L2HD 0x88B8
#define L2HD_VERSION 1
#define TYPE_DATA 4
#define TYPE_DATA_ACK 5
#define TYPE_READ_REQ 6
#define TYPE_IDENT_REQ 7
#define TYPE_IDENT_REPLY 8
#define TYPE_WRITE_REQ 10
#define TYPE_WRITE_ACK 11
#define TYPE_FLUSH_REQ 12
#define TYPE_FLUSH_ACK 13
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#define BLOCK_SIZE 512
#define MIN_ETH_FRAME 60

static const uint8_t BCAST[6] = {0xff,0xff,0xff,0xff,0xff,0xff};


#define NO_BLOCK 0xFFFFFFFFu
#define RDB_BLOCK 0u
#define PART_BLOCK 1u
#define RDB_CYLINDERS 2u
#define BOOT_PRIORITY 10

struct virtual_disk {
    int fd;
    int virtual_rdb;
    int readonly;
    int native_rdb;
    int allow_raw_write;
    int noboot;
    int boot_priority;
    uint64_t source_blocks;
    uint64_t virtual_blocks;
    uint32_t heads;
    uint32_t sectors;
    uint32_t partition_start_block;
    uint8_t dos_type[4];
    uint8_t rdb_block[BLOCK_SIZE];
    uint8_t part_block[BLOCK_SIZE];
};

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}


static void put_long(uint8_t block[BLOCK_SIZE], uint32_t index, uint32_t value) {
    put_be32(block + index * 4u, value);
}

static void put_bstr(uint8_t block[BLOCK_SIZE], uint32_t index,
                     uint32_t max_chars, const char *text) {
    size_t len = strlen(text);
    if (len > max_chars) len = max_chars;
    block[index * 4u] = (uint8_t)len;
    memcpy(block + index * 4u + 1u, text, len);
}

static void put_cstr(uint8_t block[BLOCK_SIZE], uint32_t index,
                     uint32_t byte_len, const char *text) {
    size_t len = strlen(text);
    if (byte_len == 0) return;
    if (len >= byte_len) len = byte_len - 1u;
    memset(block + index * 4u, 0, byte_len);
    memcpy(block + index * 4u, text, len);
}

static void finalize_rdb_checksum(uint8_t block[BLOCK_SIZE], uint32_t size_longs) {
    uint32_t sum = 0;
    uint32_t i;
    put_long(block, 2, 0);
    for (i = 0; i < size_longs; i++)
        sum += get_be32(block + i * 4u);
    put_long(block, 2, 0u - sum);
}

/* Override de_BootPri in a returned PART block only.  RDB block long 1 is
 * the checksum span in longs; de_BootPri is environment vector long 15,
 * i.e. absolute PART long 47. */
static int patch_part_boot_priority(uint8_t block[BLOCK_SIZE], int boot_priority) {
    uint32_t size_longs;
    if (memcmp(block, "PART", 4) != 0) return 0;
    size_longs = get_be32(block + 4);
    if (size_longs < 48u || size_longs > BLOCK_SIZE / 4u) return 0;
    put_long(block, 47, (uint32_t)(int32_t)boot_priority);
    finalize_rdb_checksum(block, size_longs);
    return 1;
}

static int choose_geometry(uint64_t fs_blocks, uint32_t *heads, uint32_t *sectors) {
    static const uint16_t geometries[][2] = {
        {16,63}, {8,63}, {4,63}, {2,63}, {1,63},
        {1,32}, {1,16}, {1,8}, {1,4}, {1,2}, {1,1}
    };
    size_t i;
    for (i = 0; i < sizeof(geometries)/sizeof(geometries[0]); i++) {
        uint32_t cyl_blocks = (uint32_t)geometries[i][0] * geometries[i][1];
        if ((fs_blocks % cyl_blocks) == 0) {
            *heads = geometries[i][0];
            *sectors = geometries[i][1];
            return 0;
        }
    }
    return -1;
}

static int build_virtual_rdb(struct virtual_disk *disk) {
    uint64_t partition_cylinders;
    uint64_t total_cylinders;
    uint64_t high_cyl;
    uint32_t cyl_blocks;
    uint32_t reserved_blocks;

    if (choose_geometry(disk->source_blocks, &disk->heads, &disk->sectors) != 0)
        return -1;

    cyl_blocks = disk->heads * disk->sectors;
    partition_cylinders = disk->source_blocks / cyl_blocks;
    high_cyl = RDB_CYLINDERS + partition_cylinders - 1u;
    total_cylinders = high_cyl + 1u;
    reserved_blocks = RDB_CYLINDERS * cyl_blocks;

    if (partition_cylinders == 0 || total_cylinders > 0xFFFFFFFFu ||
        high_cyl > 0xFFFFFFFFu)
        return -1;

    disk->partition_start_block = reserved_blocks;
    disk->virtual_blocks = disk->source_blocks + reserved_blocks;
    memset(disk->rdb_block, 0, BLOCK_SIZE);
    memset(disk->part_block, 0, BLOCK_SIZE);

    memcpy(disk->rdb_block, "RDSK", 4);
    put_long(disk->rdb_block, 1, 64);
    put_long(disk->rdb_block, 3, 7);
    put_long(disk->rdb_block, 4, BLOCK_SIZE);
    put_long(disk->rdb_block, 5, 0x17);
    put_long(disk->rdb_block, 6, NO_BLOCK);
    put_long(disk->rdb_block, 7, PART_BLOCK);
    put_long(disk->rdb_block, 8, NO_BLOCK);
    put_long(disk->rdb_block, 9, NO_BLOCK);
    put_long(disk->rdb_block, 16, (uint32_t)total_cylinders);
    put_long(disk->rdb_block, 17, disk->sectors);
    put_long(disk->rdb_block, 18, disk->heads);
    put_long(disk->rdb_block, 19, 1);
    put_long(disk->rdb_block, 20, (uint32_t)total_cylinders);
    put_long(disk->rdb_block, 24, (uint32_t)total_cylinders);
    put_long(disk->rdb_block, 25, (uint32_t)total_cylinders);
    put_long(disk->rdb_block, 26, 3);
    put_long(disk->rdb_block, 32, 0);
    put_long(disk->rdb_block, 33, reserved_blocks - 1u);
    put_long(disk->rdb_block, 34, RDB_CYLINDERS);
    put_long(disk->rdb_block, 35, (uint32_t)high_cyl);
    put_long(disk->rdb_block, 36, cyl_blocks);
    put_long(disk->rdb_block, 37, 0);
    put_long(disk->rdb_block, 38, PART_BLOCK);
    put_cstr(disk->rdb_block, 40, 8, "L2BOOT");
    put_cstr(disk->rdb_block, 42, 16, "Virtual HDF");
    put_cstr(disk->rdb_block, 46, 4, "1.0");
    put_cstr(disk->rdb_block, 47, 8, "L2BOOT");
    put_cstr(disk->rdb_block, 49, 16, "VIRTUAL RDB");
    put_cstr(disk->rdb_block, 53, 4, "1.0");
    finalize_rdb_checksum(disk->rdb_block, 64);

    memcpy(disk->part_block, "PART", 4);
    put_long(disk->part_block, 1, 64);
    put_long(disk->part_block, 3, 7);
    put_long(disk->part_block, 4, NO_BLOCK);
    put_long(disk->part_block, 5, 1);
    put_long(disk->part_block, 8, 0);
    put_bstr(disk->part_block, 9, 31, "DH0");
    put_long(disk->part_block, 32, 16);
    put_long(disk->part_block, 33, 128);
    put_long(disk->part_block, 34, 0);
    put_long(disk->part_block, 35, disk->heads);
    put_long(disk->part_block, 36, 1);
    put_long(disk->part_block, 37, disk->sectors);
    put_long(disk->part_block, 38, 2);
    put_long(disk->part_block, 39, 0);
    put_long(disk->part_block, 40, 0);
    put_long(disk->part_block, 41, RDB_CYLINDERS);
    put_long(disk->part_block, 42, (uint32_t)high_cyl);
    put_long(disk->part_block, 43, 30);
    put_long(disk->part_block, 44, 0);
    put_long(disk->part_block, 45, 0x00FFFFFFu);
    put_long(disk->part_block, 46, 0x7FFFFFFEu);
    put_long(disk->part_block, 47, (uint32_t)(int32_t)disk->boot_priority);
    put_long(disk->part_block, 48, get_be32(disk->dos_type));
    finalize_rdb_checksum(disk->part_block, 64);
    return 0;
}

static int virtual_disk_init(struct virtual_disk *disk, int fd, off_t file_size,
                             int force_raw, int noboot) {
    uint8_t first[4];
    ssize_t n;
    memset(disk, 0, sizeof(*disk));
    disk->fd = fd;
    disk->noboot = noboot;
    disk->boot_priority = noboot ? -10 : BOOT_PRIORITY;
    disk->source_blocks = (uint64_t)file_size / BLOCK_SIZE;
    disk->virtual_blocks = disk->source_blocks;

    n = pread(fd, first, sizeof(first), 0);
    if (n != (ssize_t)sizeof(first)) return -1;
    memcpy(disk->dos_type, first, 4);
    disk->native_rdb = 0;
    {
        uint8_t probe[BLOCK_SIZE];
        uint32_t i;
        uint64_t limit = disk->source_blocks < 16u ? disk->source_blocks : 16u;
        for (i = 0; i < limit; i++) {
            if (pread(fd, probe, sizeof(probe), (off_t)i * BLOCK_SIZE) != BLOCK_SIZE)
                return -1;
            if (memcmp(probe, "RDSK", 4) == 0) {
                disk->native_rdb = 1;
                break;
            }
        }
    }

    if (!force_raw && first[0] == 'D' && first[1] == 'O' && first[2] == 'S' &&
        first[3] <= 7) {
        disk->virtual_rdb = 1;
        return build_virtual_rdb(disk);
    }
    return 0;
}

static int virtual_disk_read(const struct virtual_disk *disk, uint64_t blockno,
                             uint32_t count, uint8_t *dst) {
    uint32_t i;
    if (count == 0 || blockno >= disk->virtual_blocks ||
        count > disk->virtual_blocks - blockno) {
        errno = EINVAL;
        return -1;
    }

    if (!disk->virtual_rdb) {
        size_t bytes = (size_t)count * BLOCK_SIZE;
        off_t off = (off_t)(blockno * BLOCK_SIZE);
        ssize_t n = pread(disk->fd, dst, bytes, off);
        if (n != (ssize_t)bytes) return -1;
        if (disk->noboot) {
            for (i = 0; i < count; i++)
                patch_part_boot_priority(dst + (size_t)i * BLOCK_SIZE,
                                         disk->boot_priority);
        }
        return 0;
    }

    for (i = 0; i < count; i++) {
        uint64_t vblock = blockno + i;
        uint8_t *out = dst + (size_t)i * BLOCK_SIZE;
        if (vblock == RDB_BLOCK) {
            memcpy(out, disk->rdb_block, BLOCK_SIZE);
        } else if (vblock == PART_BLOCK) {
            memcpy(out, disk->part_block, BLOCK_SIZE);
        } else if (vblock < disk->partition_start_block) {
            memset(out, 0, BLOCK_SIZE);
        } else {
            uint64_t source_block = vblock - disk->partition_start_block;
            off_t off = (off_t)(source_block * BLOCK_SIZE);
            ssize_t n = pread(disk->fd, out, BLOCK_SIZE, off);
            if (n != BLOCK_SIZE) return -1;
        }
    }
    return 0;
}


static int virtual_disk_write(const struct virtual_disk *disk, uint64_t blockno,
                              uint32_t count, const uint8_t *src) {
    size_t bytes;
    off_t off;
    ssize_t n;

    if (disk->readonly || disk->virtual_rdb ||
        (!disk->native_rdb && !disk->allow_raw_write)) {
        errno = EROFS;
        return -1;
    }
    if (!src || count == 0 || count > 2 || blockno >= disk->virtual_blocks ||
        count > disk->virtual_blocks - blockno) {
        errno = EINVAL;
        return -1;
    }
    bytes = (size_t)count * BLOCK_SIZE;
    off = (off_t)(blockno * BLOCK_SIZE);
    n = pwrite(disk->fd, src, bytes, off);
    return n == (ssize_t)bytes ? 0 : -1;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void log_vfprintf(FILE *stream, const char *fmt, va_list ap) {
    struct timespec ts;
    struct tm tmv;
    char stamp[32];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stream, "[%s.%03ld] ", stamp, ts.tv_nsec / 1000000L);
    vfprintf(stream, fmt, ap);
}

static void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vfprintf(stdout, fmt, ap);
    va_end(ap);
}

static void log_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vfprintf(stream, fmt, ap);
    va_end(ap);
}

static void log_perror(const char *what) {
    int saved_errno = errno;
    log_fprintf(stderr, "%s: %s\n", what, strerror(saved_errno));
}

static void mac_str(const uint8_t m[6], char *out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0],m[1],m[2],m[3],m[4],m[5]);
}

static const char *diag_name(uint32_t code) {
    switch (code & 0x0FFFFFFFu) {
        case 0x101u: return "D101 READ_REQ_TX_DONE_LEGACY";
        case 0x102u: return "D102 RX_COMPLETION_SEEN_LEGACY";
        case 0x103u: return "D103 DATA_HEADER_SEEN";
        case 0x104u: return "D104 DATA_VALID_BEFORE_REAL_ACK";
        case 0x105u: return "D105 REAL_ACK_SEND_RETURNED";
        case 0x110u: return "D110 POST_READ_BEFORE";
        case 0x111u: return "D111 POST_READ_AFTER";
        case 0x113u: return "D113 READ_REQ_TX_BEFORE";
        case 0x114u: return "D114 READ_REQ_TX_DONE";
        case 0x120u: return "D120 RX_POLL_ENTER";
        case 0x121u: return "D121 RX_POLL_SPIN";
        case 0x122u: return "D122 GETMSG_RETURNED";
        default: return "D??? UNKNOWN";
    }
}

static int get_iface_info(int fd, const char *ifname, int *ifindex, uint8_t mac[6]) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return -1;
    *ifindex = ifr.ifr_ifindex;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static ssize_t send_frame(int fd, int ifindex, const uint8_t *frame, size_t len) {
    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_ifindex = ifindex;
    sa.sll_halen = ETH_ALEN;
    memcpy(sa.sll_addr, frame, 6);
    if (len < MIN_ETH_FRAME) len = MIN_ETH_FRAME;
    return sendto(fd, frame, len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static size_t build_l2hd(uint8_t *frame, const uint8_t dst[6], const uint8_t src[6],
                         uint8_t typ, uint32_t seq, uint32_t blockno, uint32_t offset,
                         const uint8_t *payload, uint16_t plen) {
    memcpy(frame+0, dst, 6);
    memcpy(frame+6, src, 6);
    put_be16(frame+12, ETHERTYPE_L2HD);
    uint8_t *p = frame + 14;
    p[0]='L'; p[1]='2'; p[2]='H'; p[3]='D';
    p[4]=L2HD_VERSION; p[5]=typ;
    put_be16(p+6, plen);
    put_be32(p+8, seq);
    put_be32(p+12, blockno);
    put_be32(p+16, offset);
    if (plen && payload) memcpy(p+20, payload, plen);
    size_t len = 14 + 20 + plen;
    while (len < MIN_ETH_FRAME) frame[len++] = 0;
    return len;
}

struct parsed {
    uint8_t dst[6], src[6];
    uint8_t typ;
    uint16_t plen;
    uint32_t seq, blockno, offset;
    const uint8_t *payload;
};

static int parse_l2hd(const uint8_t *frame, ssize_t n, struct parsed *out) {
    if (n < 34) return 0;
    if (get_be16(frame+12) != ETHERTYPE_L2HD) return 0;
    const uint8_t *p = frame + 14;
    if (p[0]!='L'||p[1]!='2'||p[2]!='H'||p[3]!='D') return 0;
    if (p[4] != L2HD_VERSION) return 0;
    uint16_t plen = get_be16(p+6);
    if (n < (ssize_t)(14 + 20 + plen)) return 0;
    memcpy(out->dst, frame+0, 6);
    memcpy(out->src, frame+6, 6);
    out->typ = p[5];
    out->plen = plen;
    out->seq = get_be32(p+8);
    out->blockno = get_be32(p+12);
    out->offset = get_be32(p+16);
    out->payload = p + 20;
    return 1;
}

static int send_ident_reply(int fd, int ifindex, const uint8_t pi_mac[6],
                            const uint8_t dst[6], uint32_t seq,
                            const struct virtual_disk *disk,
                            int max_count, int window) {
    uint8_t payload[48];
    uint8_t frame[128];
    uint64_t total_blocks = disk->virtual_blocks;
    uint32_t disk_id = (uint32_t)disk->source_blocks ^ 0x4C324844u;
    if (disk->virtual_rdb)
        disk_id ^= disk->partition_start_block ^ get_be32(disk->dos_type) ^ 0x56343700u;

    memset(payload, 0, sizeof(payload));
    put_be32(payload+0, (uint32_t)(total_blocks >> 32));
    put_be32(payload+4, (uint32_t)total_blocks);
    put_be32(payload+8, BLOCK_SIZE);
    put_be32(payload+12, disk->readonly ? 1u : 0u);
    put_be32(payload+16, (uint32_t)max_count);
    put_be32(payload+20, (uint32_t)window);
    put_be32(payload+24, 0u);
    put_be32(payload+28, disk_id);
    memcpy(payload+32, "L2BOOT PI-HDF\0\0\0", 16);

    size_t len = build_l2hd(frame, dst, pi_mac, TYPE_IDENT_REPLY, seq, 0, 0, payload, sizeof(payload));
    return send_frame(fd, ifindex, frame, len) < 0 ? -1 : 0;
}

static int send_data_multi(int fd, int ifindex, const struct virtual_disk *disk,
                           const uint8_t pi_mac[6], const uint8_t dst[6],
                           uint32_t seq, uint32_t blockno, uint32_t count) {
    uint8_t payload[BLOCK_SIZE * 2];
    uint8_t frame[14 + 20 + (BLOCK_SIZE * 2) + 8];
    uint32_t off = blockno * BLOCK_SIZE;
    size_t bytes;

    if (count < 1) count = 1;
    if (count > 2) count = 2;
    bytes = (size_t)count * BLOCK_SIZE;

    if (virtual_disk_read(disk, blockno, count, payload) != 0) {
        log_fprintf(stderr, "virtual read block=%u count=%u failed errno=%d\n",
                    blockno, count, errno);
        return -1;
    }

    size_t len = build_l2hd(frame, dst, pi_mac, TYPE_DATA, seq, blockno,
                            off, payload, (uint16_t)bytes);
    return send_frame(fd, ifindex, frame, len) < 0 ? -1 : 0;
}


static int handle_read_req(int fd, int ifindex, const struct virtual_disk *disk, const uint8_t pi_mac[6],
                           const uint8_t amiga_mac[6], const struct parsed *req,
                           int timeout_ms, int max_retries, int delay_us,
                           int burst_count, int burst_gap_us) {
    (void)burst_gap_us; /* DATA1024 sends one frame per request session. */
    if (req->plen != 8) return 0;

    uint32_t start_block = req->blockno;
    uint32_t start_offset = req->offset;
    uint32_t count = get_be32(req->payload + 0);
    uint32_t block_size = get_be32(req->payload + 4);
    uint32_t session_count = count;

    char smac[32];
    mac_str(req->src, smac, sizeof(smac));

    (void)burst_count; /* DATA1024 honors the requested count directly: 1 or 2 blocks. */
    if (session_count < 1) session_count = 1;
    if (session_count > 2) session_count = 2;

    if (block_size != BLOCK_SIZE || count < 1 || count > 2 || start_offset != start_block * BLOCK_SIZE) {
        log_printf("reject READ_REQ src=%s start_block=%u count=%u block_size=%u offset=%u\n",
               smac, start_block, count, block_size, start_offset);
        fflush(stdout);
        return 0;
    }

    log_printf("READ_REQ from %s reqseq=%u start_block=%u count=%u data_blocks=%u\n",
           smac, req->seq, start_block, count, session_count);
    fflush(stdout);

    if (delay_us > 0) usleep((useconds_t)delay_us);

    uint32_t seq = req->seq; /* DATA token mirrors READ_REQ reqseq */
    int retries = 0;
    double t0 = now_sec();
    double last_send = 0.0;
    uint8_t rx[2048];

    for (;;) {
        double now = now_sec();
        if (last_send == 0.0 || ((now - last_send) * 1000.0) >= timeout_ms) {
            if (max_retries >= 0 && retries > max_retries) {
                log_printf("giveup seq=%u block=%u count=%u\n", seq, start_block, session_count);
                log_printf("summary: count=%u ack=0 missed=1 giveup=1 retries=%d total_time=%.3fs\n",
                       session_count, retries, now_sec() - t0);
                fflush(stdout);
                return 0;
            }
            log_printf("DATA_TX attempt=%d data_blocks=%u payload=%u seq=%u block=%u dst=%s elapsed_ms=%.3f\n",
                   retries + 1, session_count, session_count * BLOCK_SIZE, seq, start_block,
                   "unicast", (now_sec() - t0) * 1000.0);
            fflush(stdout);
            send_data_multi(fd, ifindex, disk, pi_mac, req->src, seq, start_block, session_count);
            last_send = now_sec();
            retries++;
            if (retries > 0 && (retries % 4) == 0) {
                log_printf("progress: ack=0/1 giveup=%s data_blocks=%u sent=%d\n", max_retries < 0 ? "disabled" : "0", session_count, retries);
                fflush(stdout);
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        int sr = select(fd+1, &rfds, NULL, NULL, &tv);
        if (sr <= 0) continue;

        for (;;) {
            ssize_t n = recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                log_perror("recv");
                break;
            }
            struct parsed p;
            if (!parse_l2hd(rx, n, &p)) continue;
            if (memcmp(p.src, amiga_mac, 6) != 0) continue;

            if (p.typ == TYPE_READ_REQ && p.plen == 8 &&
                p.blockno == start_block &&
                get_be32(p.payload + 0) == count &&
                get_be32(p.payload + 4) == BLOCK_SIZE &&
                p.offset == start_block * BLOCK_SIZE) {
                if (p.seq != seq) {
                    log_printf("REREQ in-session: oldseq=%u newseq=%u block=%u count=%u; rebasing session\n",
                           seq, p.seq, start_block, count);
                    fflush(stdout);
                    seq = p.seq;
                    retries = 0;
                    last_send = 0.0;
                    break;
                }
            }

            if (p.typ == TYPE_DATA_ACK) {
                char dmac[32], srcmac[32];
                mac_str(p.dst, dmac, sizeof(dmac));
                mac_str(p.src, srcmac, sizeof(srcmac));
                log_printf("ACKDEBUG in-session: src=%s dst=%s seq=%u block=%u plen=%u\n",
                       srcmac, dmac, p.seq, p.blockno, p.plen);
                if ((p.seq & 0xF0000000u) == 0xD0000000u) {
                    log_printf("DIAG_ACK %s rawseq=0x%08X value=%u elapsed_ms=%.3f\n",
                           diag_name(p.seq), p.seq, p.blockno, (now_sec() - t0) * 1000.0);
                }
                fflush(stdout);

                if (p.seq == seq && p.blockno == start_block && p.plen == 0) {
                    double rtt = (now_sec() - t0) * 1000.0;
                    log_printf("summary: count=%u ack=1 missed=0 giveup=0 retries=%d total_time=%.3fs rtt_ms=%.3f payload=%u\n",
                           session_count, retries > 0 ? retries-1 : 0, now_sec() - t0, rtt, session_count * BLOCK_SIZE);
                    log_printf("waiting for next L2HD request...\n");
                    fflush(stdout);
                    return 1;
                }
            }
        }
    }
}



static int send_empty_reply(int fd, int ifindex, const uint8_t pi_mac[6],
                            const uint8_t dst[6], uint8_t type,
                            uint32_t seq, uint32_t blockno) {
    uint8_t frame[64];
    size_t len = build_l2hd(frame, dst, pi_mac, type, seq, blockno, 0, NULL, 0);
    return send_frame(fd, ifindex, frame, len) < 0 ? -1 : 0;
}

static int handle_write_req(int fd, int ifindex, const struct virtual_disk *disk,
                            const uint8_t pi_mac[6], const struct parsed *req) {
    uint32_t count;
    char smac[32];
    mac_str(req->src, smac, sizeof(smac));

    if ((req->plen != BLOCK_SIZE && req->plen != BLOCK_SIZE * 2) ||
        req->offset != req->blockno * BLOCK_SIZE) {
        log_printf("reject WRITE_REQ src=%s seq=%u block=%u plen=%u offset=%u\n",
                   smac, req->seq, req->blockno, req->plen, req->offset);
        fflush(stdout);
        return 0;
    }
    count = req->plen / BLOCK_SIZE;
    if (virtual_disk_write(disk, req->blockno, count, req->payload) != 0) {
        log_printf("WRITE_FAIL src=%s seq=%u block=%u count=%u errno=%d (%s)\n",
                   smac, req->seq, req->blockno, count, errno, strerror(errno));
        fflush(stdout);
        return 0;
    }
    log_printf("WRITE_OK src=%s seq=%u block=%u count=%u payload=%u\n",
               smac, req->seq, req->blockno, count, req->plen);
    fflush(stdout);
    return send_empty_reply(fd, ifindex, pi_mac, req->src, TYPE_WRITE_ACK,
                            req->seq, req->blockno) == 0;
}

static int handle_flush_req(int fd, int ifindex, const struct virtual_disk *disk,
                            const uint8_t pi_mac[6], const struct parsed *req) {
    if (req->plen != 0) return 0;
    if (disk->readonly || fdatasync(disk->fd) != 0) {
        log_printf("FLUSH_FAIL seq=%u errno=%d (%s)\n",
                   req->seq, errno, strerror(errno));
        fflush(stdout);
        return 0;
    }
    log_printf("FLUSH_OK seq=%u\n", req->seq);
    fflush(stdout);
    return send_empty_reply(fd, ifindex, pi_mac, req->src, TYPE_FLUSH_ACK,
                            req->seq, 0) == 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        log_fprintf(stderr, "usage: sudo %s <iface> <image-or-block-device> [--delay-us N] [--timeout-ms N] [--max-retries N|-1] [--no-giveup] [--burst-count N] [--burst-gap-us N] [--raw] [--protect] [--allow-raw-write] [--noboot]\n", argv[0]);
        return 2;
    }

    const char *ifname = argv[1];
    const char *hdf_path = argv[2];
    uint8_t amiga_mac[6] = {0,0,0,0,0,0};
    int amiga_mac_valid = 0;
    int delay_us = 0;
    int timeout_ms = 250;
    int max_retries = -1; /* -1 = never give up: seek-like transport */
    int burst_count = 1;
    int burst_gap_us = 5000;
    int force_raw = 0;
    int force_protect = 0;
    int allow_raw_write = 0;
    int noboot = 0;

    for (int i=3; i<argc; i++) {
        if (strcmp(argv[i], "--delay-us") == 0 && i+1 < argc) delay_us = atoi(argv[++i]);
        else if (strcmp(argv[i], "--timeout-ms") == 0 && i+1 < argc) timeout_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-retries") == 0 && i+1 < argc) max_retries = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-giveup") == 0) max_retries = -1;
        else if (strcmp(argv[i], "--burst-count") == 0 && i+1 < argc) burst_count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--burst-gap-us") == 0 && i+1 < argc) burst_gap_us = atoi(argv[++i]);
        else if (strcmp(argv[i], "--raw") == 0) force_raw = 1;
        else if (strcmp(argv[i], "--protect") == 0) force_protect = 1;
        else if (strcmp(argv[i], "--allow-raw-write") == 0) allow_raw_write = 1;
        else if (strcmp(argv[i], "--noboot") == 0) noboot = 1;
        else {
            log_fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (burst_count < 1) burst_count = 1;
    if (burst_count > 32) burst_count = 32;
    if (burst_gap_us < 0) burst_gap_us = 0;

    int hdf_fd = open(hdf_path, force_protect ? O_RDONLY : O_RDWR);
    if (hdf_fd < 0) {
        log_perror("open hdf");
        return 1;
    }
    struct stat st;
    uint64_t source_size;
    int source_is_block_device;

    if (fstat(hdf_fd, &st) != 0) {
        log_perror("fstat source");
        return 1;
    }

    source_is_block_device = S_ISBLK(st.st_mode);
    if (source_is_block_device) {
        if (ioctl(hdf_fd, BLKGETSIZE64, &source_size) != 0) {
            log_perror("BLKGETSIZE64");
            return 1;
        }
    } else {
        if (st.st_size < 0) {
            log_fprintf(stderr, "bad HDF size\n");
            return 1;
        }
        source_size = (uint64_t)st.st_size;
    }

    if ((source_size % BLOCK_SIZE) != 0) {
        log_fprintf(stderr, "source size is not a multiple of 512 bytes\n");
        return 1;
    }
    if (source_size < BLOCK_SIZE * 4u) {
        log_fprintf(stderr, "source is too small\n");
        return 1;
    }
    if (source_size > (uint64_t)INT64_MAX) {
        log_fprintf(stderr, "source exceeds supported signed file offsets\n");
        return 1;
    }

    struct virtual_disk disk;
    if (virtual_disk_init(&disk, hdf_fd, (off_t)source_size, force_raw, noboot) != 0) {
        log_fprintf(stderr, "cannot initialize virtual disk wrapper\n");
        return 1;
    }
    disk.allow_raw_write = allow_raw_write;
    disk.readonly = force_protect || disk.virtual_rdb ||
                    (!disk.native_rdb && !allow_raw_write);

    if (disk.virtual_blocks > 0xFFFFFFFFu) {
        log_fprintf(stderr, "disk exceeds current 32-bit Amiga block addressing\n");
        return 1;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        log_perror("socket AF_PACKET");
        return 1;
    }

    int ifindex = 0;
    uint8_t pi_mac[6];
    if (get_iface_info(fd, ifname, &ifindex, pi_mac) != 0) {
        log_perror("get_iface_info");
        return 1;
    }

    struct sockaddr_ll bindaddr;
    memset(&bindaddr, 0, sizeof(bindaddr));
    bindaddr.sll_family = AF_PACKET;
    bindaddr.sll_protocol = htons(ETH_P_ALL);
    bindaddr.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
        log_perror("bind");
        return 1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char pm[32];
    mac_str(pi_mac, pm, sizeof(pm));
    log_printf("L2BootHDF C raw server v50-data1024 native-RDB-WRITE block-device noboot allow-raw-write virtual-RDB-readonly auto-peer\n");
    log_printf("iface=%s pi_mac=%s amiga_mac=auto hdf=%s source_size=%lld source_blocks=%llu virtual_blocks=%llu delay_us=%d timeout_ms=%d max_retries=%d%s burst_count=%d burst_gap_us=%d data_mode=auto-peer-readreq-relearn-unicast access=%s allow_raw_write=%s noboot=%s\n",
           ifname, pm, hdf_path, (long long)source_size,
           (unsigned long long)disk.source_blocks,
           (unsigned long long)disk.virtual_blocks,
           delay_us, timeout_ms, max_retries,
           max_retries < 0 ? " (no-giveup)" : "", burst_count, burst_gap_us,
           disk.readonly ? "read-only" : "read-write",
           allow_raw_write ? "enabled" : "disabled",
           noboot ? "enabled (BootPri=-10, source unchanged)" : "disabled");
    log_printf("source_type=%s size_method=%s\n",
               source_is_block_device ? "block-device" : "regular-file",
               source_is_block_device ? "BLKGETSIZE64" : "st_size");
    if (allow_raw_write && !disk.native_rdb && !disk.virtual_rdb) {
        log_printf("WARNING: raw non-RDB writes enabled for %s\n",
                   source_is_block_device ? "block device" : "regular file");
    }
    if (disk.virtual_rdb) {
        log_printf("image_format=DOS/%u virtual_rdb=enabled geometry=%ux%u partition_start_block=%u boot_priority=%d\n",
                   (unsigned)disk.dos_type[3], disk.heads, disk.sectors,
                   disk.partition_start_block, disk.boot_priority);
    } else {
        log_printf("image_format=%s virtual_rdb=disabled%s\n",
                   memcmp(disk.dos_type, "RDSK", 4) == 0 ? "native-RDB" : "raw",
                   force_raw ? " (--raw)" : "");
        if (disk.native_rdb && noboot)
            log_printf("boot_override=enabled boot_priority=-10 scope=all-returned-PART-blocks source_unchanged=yes\n");
        if (!disk.native_rdb)
            log_printf("write_policy=disabled (WRITE is supported only for native RDB images)\n");
    }
    log_printf("waiting for L2HD requests...\n");
    fflush(stdout);

    uint8_t rx[2048];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int sr = select(fd+1, &rfds, NULL, NULL, NULL);
        if (sr <= 0) continue;

        for (;;) {
            ssize_t n = recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                log_perror("recv");
                break;
            }

            struct parsed p;
            if (!parse_l2hd(rx, n, &p)) continue;

            char smac[32], dmac[32];
            mac_str(p.src, smac, sizeof(smac));
            mac_str(p.dst, dmac, sizeof(dmac));

            if (p.typ == TYPE_IDENT_REQ) {
                if (!amiga_mac_valid || memcmp(p.src, amiga_mac, 6) != 0) {
                    char oldmac[32];
                    if (amiga_mac_valid) {
                        mac_str(amiga_mac, oldmac, sizeof(oldmac));
                        log_printf("peer change: %s -> %s via IDENT_REQ\n", oldmac, smac);
                    } else {
                        log_printf("peer learned: %s via IDENT_REQ\n", smac);
                    }
                    memcpy(amiga_mac, p.src, 6);
                    amiga_mac_valid = 1;
                }
                log_printf("IDENT_REQ from %s seq=%u; replying capacity\n", smac, p.seq);
                send_ident_reply(fd, ifindex, pi_mac, BCAST, p.seq, &disk, 256, 48);
                fflush(stdout);
            } else if (p.typ == TYPE_READ_REQ) {
                /* v46: after a server-process restart, the Amiga may continue
                 * with READ_REQ and never emit a new IDENT_REQ.  Relearn only
                 * from a structurally valid request, so unrelated/malformed
                 * L2HD traffic cannot latch the peer.
                 */
                int valid_read_req =
                    p.plen == 8 &&
                    get_be32(p.payload + 0) >= 1 &&
                    get_be32(p.payload + 0) <= 2 &&
                    get_be32(p.payload + 4) == BLOCK_SIZE &&
                    p.offset == p.blockno * BLOCK_SIZE;

                if (!amiga_mac_valid) {
                    if (!valid_read_req) continue;
                    memcpy(amiga_mac, p.src, 6);
                    amiga_mac_valid = 1;
                    log_printf("peer learned: %s via READ_REQ after server restart\n", smac);
                } else if (memcmp(p.src, amiga_mac, 6) != 0) {
                    continue;
                }

                handle_read_req(fd, ifindex, &disk, pi_mac, amiga_mac, &p, timeout_ms, max_retries, delay_us, burst_count, burst_gap_us);
            } else if (p.typ == TYPE_WRITE_REQ) {
                int valid_write_req =
                    (p.plen == BLOCK_SIZE || p.plen == BLOCK_SIZE * 2) &&
                    p.offset == p.blockno * BLOCK_SIZE;
                if (!amiga_mac_valid) {
                    if (!valid_write_req) continue;
                    memcpy(amiga_mac, p.src, 6);
                    amiga_mac_valid = 1;
                    log_printf("peer learned: %s via WRITE_REQ after server restart\n", smac);
                } else if (memcmp(p.src, amiga_mac, 6) != 0) {
                    continue;
                }
                handle_write_req(fd, ifindex, &disk, pi_mac, &p);
            } else if (p.typ == TYPE_FLUSH_REQ) {
                if (!amiga_mac_valid || memcmp(p.src, amiga_mac, 6) != 0) continue;
                handle_flush_req(fd, ifindex, &disk, pi_mac, &p);
            } else if (p.typ == TYPE_DATA_ACK) {
                log_printf("ACKDEBUG idle: src=%s dst=%s seq=%u block=%u plen=%u\n", smac, dmac, p.seq, p.blockno, p.plen);
                fflush(stdout);
            }
        }
    }

    return 0;
}
