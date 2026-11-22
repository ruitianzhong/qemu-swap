/*
 * Save guest-dirty pages to a file at VM pause and drop them with
 * MADV_DONTNEED; restore them at VM resume.
 *
 * Dirty pages are detected with the kernel soft-dirty mechanism: a page's
 * soft-dirty bit (bit 55) in /proc/<pid>/pagemap is set by any host write to
 * that page (i.e. a COW page the guest diverged from a file-backed MAP_PRIVATE
 * mapping).  Writing "4" to /proc/<pid>/clear_refs re-arms the tracking by
 * clearing every soft-dirty bit.
 */
#include "qemu/osdep.h"

#include <sys/uio.h>

#include "sysemu/save_dirty.h"
#include "sysemu/runstate.h"
#include "exec/ram_addr.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qemu/option.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"

static bool dirty_save_enabled;
static char *dirty_save_dir;
static char *dirty_save_file;       /* NULL = <dir>/dirty_snapshot.bin */
static uint64_t dirty_save_quota;   /* 0 = unlimited */
static bool dirty_save_direct;
static size_t dirty_save_batch = 4 << 20;

typedef enum {
    DIRTY_SAVE_DIRECT_AUTO,
    DIRTY_SAVE_DIRECT_ON,
    DIRTY_SAVE_DIRECT_OFF,
} DirtySaveDirectMode;

static DirtySaveDirectMode dirty_save_direct_mode = DIRTY_SAVE_DIRECT_AUTO;

static int pagemap_fd = -1;
static size_t host_page_size;

#define PAGEMAP_SOFT_DIRTY_BIT   (1ULL << 55)
#define PAGEMAP_PRESENT_BIT      (1ULL << 63)
#define PAGEMAP_ENTRY_SIZE       8

#define SNAP_MAGIC               0x53445151  /* magic; no special meaning */
#define SNAP_VERSION             2
#define SNAP_ALIGN               4096
#define SNAP_MAX_IOV             256
#define SNAP_DEFAULT_BATCH       (4 << 20)
#define SNAP_FLAG_DIRECT         (1u << 0)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t align;
    uint32_t block_count;
    uint64_t quota;
    uint64_t total_data;
    uint64_t flags;
    uint8_t  reserved[SNAP_ALIGN - 40];
} SnapHeader;

QEMU_BUILD_BUG_ON(sizeof(SnapHeader) != SNAP_ALIGN);

typedef struct {
    uint32_t idl;
    uint32_t map_bytes;
    uint64_t used;
    uint64_t psz;
    uint64_t nsave;
    uint64_t data_bytes;
    uint64_t reserved;
} BlockMeta;

typedef struct {
    uint8_t *start;
    size_t   len;
} RamDirtyRange;

static uint8_t *block_pagemap(RAMBlock *rb);
static bool slot_is_dirty(const uint8_t *buf, uint64_t slot, size_t page_size);
static void count_dirty_before_baseline(void);

void save_dirty_parse_options(const char *optstr, Error **errp)
{
    QDict *qdict = qdict_new();
    bool help = false;
    const char *dir, *file, *quota, *direct, *batch;
    uint64_t batch_size = SNAP_DEFAULT_BATCH;

    if (!keyval_parse_into(qdict, optstr, NULL, &help, errp)) {
        error_setg(errp, "-dirty-save: invalid option specification");
        qobject_unref(qdict);
        return;
    }
    if (help || *errp) {
        qobject_unref(qdict);
        return;
    }

    dir = qdict_get_try_str(qdict, "dir");
    file = qdict_get_try_str(qdict, "file");
    quota = qdict_get_try_str(qdict, "quota");
    direct = qdict_get_try_str(qdict, "direct");
    batch = qdict_get_try_str(qdict, "batch");
    if (!dir) {
        error_setg(errp, "-dirty-save requires dir=<path>");
        qobject_unref(qdict);
        return;
    }
    if (file && !*file) {
        error_setg(errp, "-dirty-save file: path must not be empty");
        qobject_unref(qdict);
        return;
    }

    if (direct) {
        if (!strcmp(direct, "auto")) {
            dirty_save_direct_mode = DIRTY_SAVE_DIRECT_AUTO;
        } else if (!strcmp(direct, "on")) {
            dirty_save_direct_mode = DIRTY_SAVE_DIRECT_ON;
        } else if (!strcmp(direct, "off")) {
            dirty_save_direct_mode = DIRTY_SAVE_DIRECT_OFF;
        } else {
            error_setg(errp, "-dirty-save direct: expected auto, on or off");
            qobject_unref(qdict);
            return;
        }
    } else {
        dirty_save_direct_mode = DIRTY_SAVE_DIRECT_AUTO;
    }

    if (batch) {
        if (qemu_strtosz(batch, NULL, &batch_size) < 0 ||
            batch_size == 0 || batch_size % SNAP_ALIGN != 0) {
            error_setg(errp, "-dirty-save batch: expected a positive "
                       "multiple of %d", SNAP_ALIGN);
            qobject_unref(qdict);
            return;
        }
    }

    g_free(dirty_save_dir);
    dirty_save_dir = g_strdup(dir);
    g_free(dirty_save_file);
    dirty_save_file = file ? g_strdup(file) : NULL;
    dirty_save_enabled = true;
    dirty_save_quota = 0;
    dirty_save_batch = batch_size;
    dirty_save_direct = false;
    if (quota) {
        if (qemu_strtosz(quota, NULL, &dirty_save_quota) < 0) {
            error_setg(errp, "-dirty-save quota: invalid size '%s'", quota);
        }
    }
    qobject_unref(qdict);
}

/* Snapshot file path: the explicit file= path when given, otherwise
 * <dir>/dirty_snapshot.bin.  Caller owns the returned string. */
static char *dirty_save_path(void)
{
    if (dirty_save_file) {
        return g_strdup(dirty_save_file);
    }
    return g_strdup_printf("%s/dirty_snapshot.bin", dirty_save_dir);
}

/* Write "4" to /proc/<pid>/clear_refs to clear all soft-dirty bits, i.e.
 * re-arm the tracking.  Returns 0 on success or a negative errno on failure.
 * The file is opened and closed per call rather than held open. */
static int clear_soft_dirty(void)
{
    char *path = g_strdup_printf("/proc/%d/clear_refs", getpid());
    int fd = open(path, O_WRONLY);
    int ret;

    g_free(path);
    if (fd < 0) {
        return -errno;
    }
    if (write(fd, "4", 1) != 1) {
        ret = -errno;
    } else {
        ret = 0;
    }
    close(fd);
    return ret;
}

/* Probe whether the snapshot directory supports O_DIRECT with aligned 4K
 * writes.  Returns 0 on success or a negative errno. */
static int probe_odirect(void)
{
    char *path = g_strdup_printf("%s/.dirty_save_probe.%d",
                                 dirty_save_dir, getpid());
    void *buf = qemu_memalign(SNAP_ALIGN, SNAP_ALIGN);
    int fd, ret = 0;

    memset(buf, 0, SNAP_ALIGN);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    if (fd < 0) {
        ret = -errno;
    } else {
        ssize_t w = pwrite(fd, buf, SNAP_ALIGN, 0);

        if (w != SNAP_ALIGN) {
            ret = w < 0 ? -errno : -EIO;
        }
        close(fd);
    }
    unlink(path);
    qemu_vfree(buf);
    g_free(path);
    return ret;
}

void save_dirty_init(Error **errp)
{
    char *pagemap_path, *snapshot_path, *snapshot_dir;
    struct stat st;
    int ret;

    if (!dirty_save_enabled) {
        return;
    }

    if (mkdir(dirty_save_dir, 0700) < 0 && errno != EEXIST) {
        error_setg_errno(errp, errno,
                         "-dirty-save: cannot create dir %s", dirty_save_dir);
        return;
    }

    snapshot_path = dirty_save_path();
    snapshot_dir = g_path_get_dirname(snapshot_path);
    if (stat(snapshot_dir, &st) < 0) {
        error_setg_errno(errp, errno,
                         "-dirty-save: snapshot directory %s is not accessible",
                         snapshot_dir);
        goto out;
    }
    if (!S_ISDIR(st.st_mode)) {
        error_setg(errp, "-dirty-save: %s is not a directory", snapshot_dir);
        goto out;
    }
    if (stat(snapshot_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        error_setg(errp, "-dirty-save: snapshot path %s is a directory",
                   snapshot_path);
        goto out;
    }

    /* This process has not paused yet, so any snapshot left in the file can
     * only be from a previous QEMU process, whose pages do not correspond to
     * this VM's RAM (e.g. -incoming finishes straight into PAUSED, then
     * `cont` would restore foreign pages).  Remove it; failure is fatal
     * because the hazard would otherwise persist. */
    if (unlink(snapshot_path) < 0 && errno != ENOENT) {
        error_setg_errno(errp, errno, "-dirty-save: cannot remove stale "
                         "snapshot %s", snapshot_path);
        goto out;
    }
    g_free(snapshot_dir);
    snapshot_dir = NULL;
    g_free(snapshot_path);
    snapshot_path = NULL;

    pagemap_path = g_strdup_printf("/proc/%d/pagemap", getpid());
    pagemap_fd = open(pagemap_path, O_RDONLY);
    g_free(pagemap_path);
    if (pagemap_fd < 0) {
        error_setg_errno(errp, errno, "-dirty-save: cannot open "
                         "/proc/<pid>/pagemap (need permission to read own pagemap)");
        return;
    }

    host_page_size = (size_t)qemu_real_host_page_size;

    /* Diagnostic: how many dirty pages exist in the qualifying regions before
     * the baseline clear below discards the soft-dirty state. */
    count_dirty_before_baseline();

    /* Probe support and establish the baseline with one clear_refs write. */
    ret = clear_soft_dirty();
    if (ret < 0) {
        if (ret == -EINVAL) {
            error_setg(errp, "-dirty-save: kernel lacks CONFIG_MEM_SOFT_DIRTY "
                       "support (clear_refs returned EINVAL)");
        } else {
            error_setg_errno(errp, -ret, "-dirty-save: clear_refs failed");
        }
        return;
    }

    dirty_save_direct = false;
    if (dirty_save_direct_mode != DIRTY_SAVE_DIRECT_OFF) {
        ret = probe_odirect();
        if (ret == 0) {
            dirty_save_direct = true;
            error_report("dirty-save: O_DIRECT enabled (dir=%s, align=%d, "
                         "batch=%zu)", dirty_save_dir, SNAP_ALIGN,
                         dirty_save_batch);
        } else if (dirty_save_direct_mode == DIRTY_SAVE_DIRECT_ON) {
            error_report("dirty-save: O_DIRECT not supported on %s (%s)",
                         dirty_save_dir, strerror(-ret));
            error_setg(errp, "-dirty-save: O_DIRECT requested but not "
                       "supported on %s", dirty_save_dir);
            return;
        } else {
            error_report("dirty-save: O_DIRECT not supported on %s (%s), "
                         "using buffered I/O", dirty_save_dir, strerror(-ret));
        }
    }

out:
    g_free(snapshot_dir);
    g_free(snapshot_path);
}

/* Read one RAMBlock's soft-dirty bitmap from /proc/<pid>/pagemap.  One 8-byte
 * entry per host page of virtual address space; block-internal host-page slot
 * "k" lives at buf[k].  Returns NULL on error. */
static uint8_t *block_pagemap(RAMBlock *rb)
{
    uint64_t n = QEMU_ALIGN_UP(rb->used_length, host_page_size) / host_page_size;
    size_t bytes = (size_t)n * PAGEMAP_ENTRY_SIZE;
    uint8_t *buf = g_malloc0(bytes);
    off_t off = ((uintptr_t)rb->host / host_page_size) * PAGEMAP_ENTRY_SIZE;

    if (pread(pagemap_fd, buf, bytes, off) != (ssize_t)bytes) {
        error_report("dirty-save: pread pagemap for %s: %s",
                     rb->idstr, strerror(errno));
        g_free(buf);
        return NULL;
    }
    return buf;
}

/* Is the save-unit "slot" (a host page, qemu_ram_pagesize(rb) bytes) dirty,
 * i.e. does any of its per-host-page entries carry the soft-dirty bit? */
static bool slot_is_dirty(const uint8_t *buf, uint64_t slot, size_t page_size)
{
    uint64_t per = page_size / host_page_size;
    const uint64_t *e = (const uint64_t *)buf;

    for (uint64_t k = slot * per; k < (slot + 1) * per; k++) {
        if (e[k] & PAGEMAP_SOFT_DIRTY_BIT) {
            return true;
        }
    }
    return false;
}

/* Count dirty pages in the same blocks save_dirty_pause() targets (file-backed
 * MAP_PRIVATE RAM), before the init-time baseline clear discards the current
 * soft-dirty state.  Diagnostic only. */
static void count_dirty_before_baseline(void)
{
    RAMBlock *rb;
    uint64_t total = 0, blocks = 0;

    if (pagemap_fd < 0) {
        return;
    }
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb) {
        uint64_t ps, slots, i, n = 0;
        uint8_t *buf;

        if (rb->host == NULL || rb->used_length == 0) {
            continue;
        }
        if (rb->fd < 0 || (rb->flags & RAM_SHARED)) {
            continue;
        }
        ps = qemu_ram_pagesize(rb);
        slots = rb->used_length / ps;
        buf = block_pagemap(rb);
        if (!buf) {
            continue;
        }
        for (i = 0; i < slots; i++) {
            if (slot_is_dirty(buf, i, ps)) {
                n++;
            }
        }
        g_free(buf);
        fprintf(stderr, "dirty-save: pre-init dirty block %s: start=%p len=%" PRIu64
                " dirty=%" PRIu64 "\n",
                rb->idstr, (void *)rb->host, (uint64_t)rb->used_length, n);
        total += n;
        blocks++;
    }
    fprintf(stderr, "dirty-save: pre-init dirty total: %" PRIu64
            " blocks, %" PRIu64 " dirty pages\n", blocks, total);
}

static bool read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len) {
        ssize_t r = read(fd, p, len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_report("dirty-save: read: %s", strerror(errno));
            return false;
        }
        if (r == 0) {
            error_report("dirty-save: unexpected EOF");
            return false;
        }
        p += r;
        len -= r;
    }
    return true;
}

/* Record the first failure of a save-dirty pause.  stats may be NULL. */
static void stats_set_error(SaveDirtyPauseStats *stats,
                            const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void stats_set_error(SaveDirtyPauseStats *stats,
                            const char *fmt, ...)
{
    va_list ap;

    if (!stats || stats->error) {
        return;
    }
    va_start(ap, fmt);
    stats->error = g_strdup_vprintf(fmt, ap);
    va_end(ap);
}

/* Write a 4K-aligned buffer at an aligned file offset. */
static bool pwrite_all(int fd, const void *buf, size_t len, off_t off,
                       bool direct)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t r = pwrite(fd, p, len, off);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_report("dirty-save: pwrite: %s", strerror(errno));
            return false;
        }
        if (r == 0) {
            error_report("dirty-save: pwrite: unexpected zero write");
            errno = EIO;
            return false;
        }
        if (direct && (r % SNAP_ALIGN) != 0) {
            error_report("dirty-save: pwrite: misaligned short write");
            errno = EIO;
            return false;
        }
        p += r;
        len -= r;
        off += r;
    }
    return true;
}

/* Write an iovec array with pwritev, advancing over short writes. */
static bool pwritev_all(int fd, struct iovec *iov, int iovcnt, off_t off,
                        size_t total, bool direct)
{
    size_t done = 0;

    while (done < total) {
        ssize_t r = pwritev(fd, iov, iovcnt, off + done);
        size_t skip;

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_report("dirty-save: pwritev: %s", strerror(errno));
            return false;
        }
        if (r == 0) {
            error_report("dirty-save: pwritev: unexpected zero write");
            errno = EIO;
            return false;
        }
        if (direct && (r % SNAP_ALIGN) != 0) {
            error_report("dirty-save: pwritev: misaligned short write");
            errno = EIO;
            return false;
        }
        done += r;
        skip = r;
        while (skip > 0) {
            if (skip >= iov->iov_len) {
                skip -= iov->iov_len;
                iov++;
                iovcnt--;
            } else {
                iov->iov_base = (uint8_t *)iov->iov_base + skip;
                iov->iov_len -= skip;
                skip = 0;
            }
        }
    }
    return true;
}

static bool write_pages_batch(int fd, struct iovec *iov, int iovcnt,
                              size_t bytes, off_t *off, bool direct,
                              const char *path, SaveDirtyPauseStats *stats)
{
    bool ok;

    if (direct) {
        bool aligned = true;
        int i;

        for (i = 0; i < iovcnt; i++) {
            if (((uintptr_t)iov[i].iov_base % SNAP_ALIGN) != 0 ||
                (iov[i].iov_len % SNAP_ALIGN) != 0) {
                aligned = false;
                break;
            }
        }
        if (!aligned) {
            uint8_t *tmp = qemu_memalign(SNAP_ALIGN, bytes);
            size_t pos = 0;

            for (i = 0; i < iovcnt; i++) {
                memcpy(tmp + pos, iov[i].iov_base, iov[i].iov_len);
                pos += iov[i].iov_len;
            }
            ok = pwrite_all(fd, tmp, bytes, *off, true);
            qemu_vfree(tmp);
        } else {
            ok = pwritev_all(fd, iov, iovcnt, *off, bytes, true);
        }
    } else {
        ok = pwritev_all(fd, iov, iovcnt, *off, bytes, false);
    }
    if (!ok) {
        stats_set_error(stats, "write %s: %s", path, strerror(errno));
        return false;
    }
    *off += bytes;
    return true;
}

static bool write_snap_header(int fd, const SnapHeader *h, bool direct)
{
    void *buf = qemu_memalign(SNAP_ALIGN, SNAP_ALIGN);
    bool ok;

    memcpy(buf, h, sizeof(*h));
    ok = pwrite_all(fd, buf, SNAP_ALIGN, 0, direct);
    qemu_vfree(buf);
    return ok;
}

static bool write_block_meta(int fd, const char *idstr, uint64_t used,
                             uint64_t psz, uint64_t nsave,
                             const uint8_t *slot_map, size_t map_bytes,
                             off_t off, bool direct)
{
    size_t idl = strlen(idstr);
    size_t meta_len = sizeof(BlockMeta) + idl + map_bytes;
    size_t alloc = QEMU_ALIGN_UP(meta_len, SNAP_ALIGN);
    BlockMeta m;
    uint8_t *buf;
    bool ok;

    if (idl > UINT32_MAX || map_bytes > UINT32_MAX) {
        error_report("dirty-save: block metadata too large");
        return false;
    }
    buf = qemu_memalign(SNAP_ALIGN, alloc);
    memset(buf, 0, alloc);
    m.idl = idl;
    m.map_bytes = map_bytes;
    m.used = used;
    m.psz = psz;
    m.nsave = nsave;
    m.data_bytes = nsave * psz;
    m.reserved = 0;
    memcpy(buf, &m, sizeof(m));
    memcpy(buf + sizeof(m), idstr, idl);
    memcpy(buf + sizeof(m) + idl, slot_map, map_bytes);
    ok = pwrite_all(fd, buf, alloc, off, direct);
    qemu_vfree(buf);
    return ok;
}

static void madvise_ranges(GArray *ranges)
{
    for (guint j = 0; j < ranges->len; j++) {
        RamDirtyRange r = g_array_index(ranges, RamDirtyRange, j);
        uint8_t *rs = r.start;
        uint8_t *re = r.start + r.len;

        /* Coalesce adjacent ranges into a single MADV_DONTNEED call. */
        while (j + 1 < ranges->len) {
            RamDirtyRange n = g_array_index(ranges, RamDirtyRange, j + 1);
            if (n.start == re) {
                re = n.start + n.len;
                j++;
            } else {
                break;
            }
        }
        qemu_madvise(rs, re - rs, QEMU_MADV_DONTNEED);
    }
}

bool save_dirty_is_enabled(void)
{
    return dirty_save_enabled && pagemap_fd >= 0;
}

void save_dirty_pause(RunState state, bool enable_save_dirty,
                      bool has_quota, uint64_t quota,
                      SaveDirtyPauseStats *stats)
{
    RAMBlock *rb;
    uint64_t effective_quota = has_quota ? quota : dirty_save_quota;
    SnapHeader h = {
        .magic = SNAP_MAGIC,
        .version = SNAP_VERSION,
        .align = SNAP_ALIGN,
        .block_count = 0,
        .quota = effective_quota,
        .total_data = 0,
        .flags = dirty_save_direct ? SNAP_FLAG_DIRECT : 0,
    };
    GArray *ranges = NULL;
    uint64_t total_saved_pages = 0, total_skipped_pages = 0;
    uint64_t written_pages = 0, written_bytes = 0;
    unsigned int blocks_saved = 0;
    bool direct;
    gint64 t0;
    off_t off;
    char *path;
    int fd;

    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }

    if (!dirty_save_enabled || !enable_save_dirty ||
        state != RUN_STATE_PAUSED || pagemap_fd < 0) {
        return;
    }

    if (stats) {
        stats->attempted = true;
    }

    direct = dirty_save_direct;
    t0 = g_get_monotonic_time();

    path = dirty_save_path();
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC |
              (direct ? O_DIRECT : 0), 0600);
    if (fd < 0) {
        int err = errno;

        error_report("dirty-save: open %s: %s", path, strerror(err));
        stats_set_error(stats, "open %s: %s", path, strerror(err));
        g_free(path);
        return;
    }
    if (!write_snap_header(fd, &h, direct)) {
        stats_set_error(stats, "write header %s: %s", path, strerror(errno));
        goto out;
    }
    off = SNAP_ALIGN;

    ranges = g_array_new(false, false, sizeof(RamDirtyRange));

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb) {
        uint8_t *buf, *slot_map;
        GArray *idx;
        uint64_t slots, i, nsave, dirty_seen = 0;
        size_t ps, map_bytes, k, meta_bytes;
        struct iovec iov[SNAP_MAX_IOV];
        int cnt = 0;
        size_t batch = 0;
        gint64 tb;

        if (rb->host == NULL || rb->used_length == 0) {
            continue;
        }
        /* Only operate on file-backed MAP_PRIVATE RAM (e.g. a
         * memory-backend-file with share=off): those are the COW pages we can
         * drop and later refault.  Anonymous/ROM blocks (fd < 0) and shared
         * mappings have nothing to reclaim. */
        if (rb->fd < 0 || (rb->flags & RAM_SHARED)) {
            continue;
        }
        ps = qemu_ram_pagesize(rb);
        slots = rb->used_length / ps;
        tb = g_get_monotonic_time();

        buf = block_pagemap(rb);
        if (!buf) {
            stats_set_error(stats, "cannot read pagemap for block %s",
                            rb->idstr);
            continue;
        }
        map_bytes = (slots + 7) / 8;
        slot_map = g_malloc0(map_bytes);
        idx = g_array_new(false, false, sizeof(uint64_t));

        for (i = 0; i < slots; i++) {
            if (!slot_is_dirty(buf, i, ps)) {
                continue;
            }
            dirty_seen++;
            if (effective_quota && h.total_data + ps > effective_quota) {
                /* Over quota: leave pages resident, neither save nor drop. */
                continue;
            }
            uint64_t one = i;
            g_array_append_val(idx, one);
            slot_map[i / 8] |= (1u << (i % 8));
            h.total_data += ps;
        }
        nsave = idx->len;
        total_saved_pages += nsave;
        total_skipped_pages += dirty_seen - nsave;

        fprintf(stderr, "dirty-save: pause block %s: start=%p len=%" PRIu64
                " (host page %" PRIu64 "): dirty=%" PRIu64
                " saved=%" PRIu64 " skipped=%" PRIu64 " (%" PRIu64 " KB) in %"
                G_GINT64_FORMAT " us\n",
                rb->idstr, (void *)rb->host, rb->used_length,
                (uint64_t)ps, dirty_seen, nsave, dirty_seen - nsave,
                (nsave * ps) >> 10, g_get_monotonic_time() - tb);

        if (nsave == 0) {
            g_free(buf);
            g_free(slot_map);
            g_array_unref(idx);
            continue;
        }
        blocks_saved++;

        meta_bytes = QEMU_ALIGN_UP(sizeof(BlockMeta) + strlen(rb->idstr) +
                                   map_bytes, SNAP_ALIGN);
        if (!write_block_meta(fd, rb->idstr, rb->used_length, ps, nsave,
                              slot_map, map_bytes, off, direct)) {
            stats_set_error(stats, "write meta %s: %s", path, strerror(errno));
            g_free(buf);
            g_free(slot_map);
            g_array_unref(idx);
            goto out;
        }
        off += meta_bytes;

        for (k = 0; k < nsave; k++) {
            uint64_t slot = g_array_index(idx, uint64_t, k);
            uint8_t *base = rb->host + slot * ps;

            if (cnt > 0 && batch + ps <= dirty_save_batch &&
                (uint8_t *)iov[cnt - 1].iov_base + iov[cnt - 1].iov_len == base) {
                iov[cnt - 1].iov_len += ps;
            } else {
                if (cnt > 0 &&
                    (cnt == SNAP_MAX_IOV || batch + ps > dirty_save_batch)) {
                    if (!write_pages_batch(fd, iov, cnt, batch, &off, direct,
                                           path, stats)) {
                        g_free(buf);
                        g_free(slot_map);
                        g_array_unref(idx);
                        goto out;
                    }
                    cnt = 0;
                    batch = 0;
                }
                iov[cnt].iov_base = base;
                iov[cnt].iov_len = ps;
                cnt++;
            }
            batch += ps;
            written_pages++;
            written_bytes += ps;
            RamDirtyRange one = { .start = base, .len = ps };
            g_array_append_val(ranges, one);
        }
        if (cnt && !write_pages_batch(fd, iov, cnt, batch, &off, direct,
                                      path, stats)) {
            g_free(buf);
            g_free(slot_map);
            g_array_unref(idx);
            goto out;
        }

        h.block_count++;

        g_free(buf);
        g_free(slot_map);
        g_array_unref(idx);
    }

    /* Publish the final header before the only fsync.  The pages may be
     * dropped only after that fsync succeeds. */
    if (!write_snap_header(fd, &h, direct)) {
        stats_set_error(stats, "write header %s: %s", path, strerror(errno));
        goto out;
    }
    if (fsync(fd) < 0) {
        int err = errno;

        error_report("dirty-save: fsync: %s", strerror(err));
        stats_set_error(stats, "fsync %s: %s", path, strerror(err));
        goto out;
    }
    madvise_ranges(ranges);

    if (!direct && posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) < 0) {
        error_report("dirty-save: fadvise DONTNEED: %s", strerror(errno));
    }

out:
    if (stats && stats->attempted) {
        stats->saved_pages = written_pages;
        stats->saved_bytes = written_bytes;
        stats->skipped_pages = total_skipped_pages;
    }

    close(fd);
    g_free(path);
    if (ranges) {
        g_array_unref(ranges);
    }

    fprintf(stderr, "dirty-save: pause: %u blocks, %" PRIu64
                " dirty pages saved (%" PRIu64 " KB), %" PRIu64
                " skipped (over quota), took %" G_GINT64_FORMAT " us"
                " (direct=%s)\n",
                blocks_saved, total_saved_pages, h.total_data >> 10,
                total_skipped_pages, g_get_monotonic_time() - t0,
                direct ? "on" : "off");
}

void save_dirty_resume(void)
{
    SnapHeader h;
    uint64_t restored_bytes = 0, restored_pages = 0;
    gint64 t0;
    char *path;
    int fd;

    if (!dirty_save_enabled || !runstate_check(RUN_STATE_PAUSED)) {
        return;
    }

    t0 = g_get_monotonic_time();

    path = dirty_save_path();
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* No snapshot from a prior pause: nothing to restore. */
        g_free(path);
        return;
    }

    memset(&h, 0, sizeof(h));
    if (!read_all(fd, &h, sizeof(h)) ||
        h.magic != SNAP_MAGIC || h.version != SNAP_VERSION ||
        h.align != SNAP_ALIGN) {
        error_report("dirty-save: invalid snapshot header in %s", path);
        goto fail;
    }

    for (uint32_t b = 0; b < h.block_count; b++) {
        BlockMeta m;
        char *idstr;
        RAMBlock *rb = NULL;
        uint8_t *slot_map = NULL;
        uint64_t slots, set_bits = 0;
        size_t map_bytes;
        off_t data_off;
        uint64_t slot;

        if (!read_all(fd, &m, sizeof(m))) {
            goto fail;
        }
        if (m.psz == 0 || m.used % m.psz != 0 ||
            m.idl > (1u << 20) || m.map_bytes > (1u << 20)) {
            error_report("dirty-save: invalid block metadata in %s", path);
            goto fail;
        }
        slots = m.used / m.psz;
        map_bytes = (slots + 7) / 8;
        if (map_bytes != m.map_bytes ||
            m.data_bytes != m.nsave * m.psz) {
            error_report("dirty-save: inconsistent block metadata in %s",
                         path);
            goto fail;
        }

        idstr = g_malloc(m.idl + 1);
        if (!read_all(fd, idstr, m.idl)) {
            g_free(idstr);
            goto fail;
        }
        idstr[m.idl] = '\0';
        slot_map = g_malloc(map_bytes);
        if (!read_all(fd, slot_map, map_bytes)) {
            g_free(slot_map);
            g_free(idstr);
            goto fail;
        }

        for (slot = 0; slot < slots; slot++) {
            if (slot_map[slot / 8] & (1u << (slot % 8))) {
                set_bits++;
            }
        }
        if (set_bits != m.nsave) {
            error_report("dirty-save: inconsistent slot map in %s", path);
            g_free(slot_map);
            g_free(idstr);
            goto fail;
        }

        {
            RAMBlock *it;
            RCU_READ_LOCK_GUARD();
            RAMBLOCK_FOREACH(it) {
                if (strcmp(it->idstr, idstr) == 0) {
                    rb = it;
                    break;
                }
            }
            if (rb && (rb->used_length != m.used ||
                       qemu_ram_pagesize(rb) != m.psz)) {
                rb = NULL;
            }
        }

        data_off = lseek(fd, 0, SEEK_CUR);
        if (data_off < 0) {
            g_free(slot_map);
            g_free(idstr);
            goto fail;
        }
        data_off = QEMU_ALIGN_UP(data_off, SNAP_ALIGN);
        if (lseek(fd, data_off, SEEK_SET) < 0) {
            g_free(slot_map);
            g_free(idstr);
            goto fail;
        }

        if (rb) {
            for (slot = 0; slot < slots; slot++) {
                if (!(slot_map[slot / 8] & (1u << (slot % 8)))) {
                    continue;
                }
                if (!read_all(fd, rb->host + slot * m.psz, m.psz)) {
                    g_free(slot_map);
                    g_free(idstr);
                    goto fail;
                }
                restored_pages++;
                restored_bytes += m.psz;
            }
        }

        g_free(slot_map);
        g_free(idstr);
        if (lseek(fd, data_off + (off_t)m.data_bytes, SEEK_SET) < 0) {
            goto fail;
        }
    }

    unlink(path);
    close(fd);
    g_free(path);

    fprintf(stderr, "dirty-save: resume: %u blocks, %" PRIu64
                " pages restored (%" PRIu64 " KB), took %" G_GINT64_FORMAT " us\n",
                h.block_count, restored_pages, restored_bytes >> 10,
                g_get_monotonic_time() - t0);
    return;

fail:
    error_report("dirty-save: cannot restore snapshot %s; aborting", path);
    close(fd);
    g_free(path);
    exit(1);
}
