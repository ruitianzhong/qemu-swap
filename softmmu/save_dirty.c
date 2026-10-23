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
static int pagemap_fd = -1;
static size_t host_page_size;

#define PAGEMAP_SOFT_DIRTY_BIT   (1ULL << 55)
#define PAGEMAP_PRESENT_BIT      (1ULL << 63)
#define PAGEMAP_ENTRY_SIZE       8

#define SNAP_MAGIC               0x53445151  /* magic; no special meaning */
#define SNAP_VERSION             1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_count;
    uint32_t pad;
    uint64_t quota;
    uint64_t total_data;
} SnapHeader;

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
    const char *dir, *file, *quota;

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

    g_free(dirty_save_dir);
    dirty_save_dir = g_strdup(dir);
    g_free(dirty_save_file);
    dirty_save_file = file ? g_strdup(file) : NULL;
    dirty_save_enabled = true;
    dirty_save_quota = 0;
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

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t r = write(fd, p, len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_report("dirty-save: write: %s", strerror(errno));
            return false;
        }
        p += r;
        len -= r;
    }
    return true;
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
        .magic = SNAP_MAGIC, .version = SNAP_VERSION,
        .block_count = 0, .pad = 0,
        .quota = effective_quota, .total_data = 0,
    };
    GArray *ranges = NULL;
    uint64_t total_saved_pages = 0, total_skipped_pages = 0;
    uint64_t written_pages = 0, written_bytes = 0;
    unsigned int blocks_saved = 0;
    gint64 t0;
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

    t0 = g_get_monotonic_time();

    path = dirty_save_path();
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        int err = errno;

        error_report("dirty-save: open %s: %s", path, strerror(err));
        stats_set_error(stats, "open %s: %s", path, strerror(err));
        g_free(path);
        return;
    }
    if (!write_all(fd, &h, sizeof(h))) {
        int err = errno;

        stats_set_error(stats, "write %s: %s", path, strerror(err));
        goto out;
    }

    ranges = g_array_new(false, false, sizeof(RamDirtyRange));

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb) {
        uint8_t *buf, *slot_map;
        GArray *idx;
        uint64_t slots, i, nsave, dirty_seen = 0;
        uint64_t psz;
        size_t ps, map_bytes, k;
        uint32_t idl;
        uint64_t used;
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

        idl = strlen(rb->idstr);
        used = rb->used_length;
        psz = ps;
        if (!write_all(fd, &idl, 4) ||
            !write_all(fd, rb->idstr, idl) ||
            !write_all(fd, &used, 8) ||
            !write_all(fd, &psz, 8) ||
            !write_all(fd, &nsave, 8) ||
            !write_all(fd, slot_map, map_bytes)) {
            int err = errno;

            stats_set_error(stats, "write %s: %s", path, strerror(err));
            g_free(buf);
            g_free(slot_map);
            g_array_unref(idx);
            goto out;
        }

        for (k = 0; k < nsave; k++) {
            uint64_t slot = g_array_index(idx, uint64_t, k);
            if (!write_all(fd, rb->host + slot * ps, ps)) {
                int err = errno;

                stats_set_error(stats, "write %s: %s", path, strerror(err));
                g_free(buf);
                g_free(slot_map);
                g_array_unref(idx);
                goto out;
            }
            written_pages++;
            written_bytes += ps;
            RamDirtyRange one = { .start = rb->host + slot * ps, .len = ps };
            g_array_append_val(ranges, one);
        }

        h.block_count++;

        g_free(buf);
        g_free(slot_map);
        g_array_unref(idx);
    }

    /* The snapshot is complete on disk; now the pages may be dropped. */
    if (fsync(fd) < 0) {
        int err = errno;

        error_report("dirty-save: fsync: %s", strerror(err));
        stats_set_error(stats, "fsync %s: %s", path, strerror(err));
    }
    madvise_ranges(ranges);

    if (lseek(fd, 0, SEEK_SET) < 0) {
        int err = errno;

        error_report("dirty-save: seek: %s", strerror(err));
        stats_set_error(stats, "seek %s: %s", path, strerror(err));
    } else {
        ssize_t rew = write(fd, &h, sizeof(h));

        if (rew != sizeof(h)) {
            int err = rew < 0 ? errno : EIO;

            error_report("dirty-save: write header: %s", strerror(err));
            stats_set_error(stats, "write header %s: %s", path,
                            strerror(err));
        }
    }

    /* Make the (header) data durable, then drop the file's page cache so the
     * snapshot does not keep resident memory that we are trying to reclaim. */
    if (fsync(fd) < 0) {
        int err = errno;

        error_report("dirty-save: fsync(header): %s", strerror(err));
        stats_set_error(stats, "fsync %s: %s", path, strerror(err));
    }
    if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) < 0) {
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
                " skipped (over quota), took %" G_GINT64_FORMAT " us\n",
                blocks_saved, total_saved_pages, h.total_data >> 10,
                total_skipped_pages, g_get_monotonic_time() - t0);
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

    if (!read_all(fd, &h, sizeof(h)) ||
        h.magic != SNAP_MAGIC || h.version != SNAP_VERSION) {
        error_report("dirty-save: invalid snapshot header");
        goto out;
    }

    for (uint32_t b = 0; b < h.block_count; b++) {
        uint32_t idl;
        uint64_t used, psz, nsave, slots;
        char *idstr;
        RAMBlock *rb;
        size_t map_bytes;
        uint8_t *slot_map;

        if (!read_all(fd, &idl, 4)) {
            goto out;
        }
        idstr = g_malloc(idl + 1);
        if (!read_all(fd, idstr, idl)) {
            g_free(idstr);
            goto out;
        }
        idstr[idl] = '\0';
        if (!read_all(fd, &used, 8) || !read_all(fd, &psz, 8) ||
            !read_all(fd, &nsave, 8)) {
            g_free(idstr);
            goto out;
        }

        rb = NULL;
        if (psz != 0) {
            RAMBlock *it;
            RCU_READ_LOCK_GUARD();
            RAMBLOCK_FOREACH(it) {
                if (strcmp(it->idstr, idstr) == 0) {
                    rb = it;
                    break;
                }
            }
        }

        if (!rb || used % psz != 0) {
            /* Unknown or mismatched block: skip its payload. */
            g_free(idstr);
            if (psz == 0) {
                goto out;
            }
            slots = used / psz;
            map_bytes = (slots + 7) / 8;
            if (lseek(fd, (off_t)map_bytes + (off_t)(nsave * psz), SEEK_CUR) < 0) {
                goto out;
            }
            continue;
        }

        slots = used / psz;
        map_bytes = (slots + 7) / 8;
        slot_map = g_malloc(map_bytes);
        if (!read_all(fd, slot_map, map_bytes)) {
            g_free(slot_map);
            g_free(idstr);
            goto out;
        }
        for (uint64_t slot = 0; slot < slots; slot++) {
            if (!(slot_map[slot / 8] & (1u << (slot % 8)))) {
                continue;
            }
            if (!read_all(fd, rb->host + slot * psz, psz)) {
                g_free(slot_map);
                g_free(idstr);
                goto out;
            }
            restored_pages++;
            restored_bytes += psz;
        }
        g_free(slot_map);
        g_free(idstr);
    }

    unlink(path);
out:
    close(fd);
    g_free(path);

    fprintf(stderr, "dirty-save: resume: %u blocks, %" PRIu64
                " pages restored (%" PRIu64 " KB), took %" G_GINT64_FORMAT " us\n",
                h.block_count, restored_pages, restored_bytes >> 10,
                g_get_monotonic_time() - t0);
}