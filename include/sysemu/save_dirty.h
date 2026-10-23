#ifndef SYSEMU_SAVE_DIRTY_H
#define SYSEMU_SAVE_DIRTY_H

#include "qapi/qapi-types-run-state.h"

/*
 * save_dirty: at VM pause, write the guest-dirty pages (detected via the
 * kernel soft-dirty mechanism on /proc/<pid>/pagemap) to a file and madvise
 * them away with MADV_DONTNEED to reclaim RSS; at VM resume, restore the
 * pages from that file.
 */

/* Result of one save-dirty pause.  attempted is false when the save path was
 * not entered (feature disabled, not requested, VM not running); the other
 * fields are valid only when it is true.  error is NULL on success. */
typedef struct SaveDirtyPauseStats {
    bool attempted;
    char *error;
    uint64_t saved_pages;
    uint64_t saved_bytes;
    uint64_t skipped_pages;
} SaveDirtyPauseStats;

void save_dirty_parse_options(const char *optstr, Error **errp);
void save_dirty_init(Error **errp);
bool save_dirty_is_enabled(void);
void save_dirty_pause(RunState state, bool enable_save_dirty,
                      bool has_quota, uint64_t quota,
                      SaveDirtyPauseStats *stats);
void save_dirty_resume(void);

#endif /* SYSEMU_SAVE_DIRTY_H */
