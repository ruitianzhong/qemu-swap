#ifndef SYSEMU_SAVE_DIRTY_H
#define SYSEMU_SAVE_DIRTY_H

#include "qapi/qapi-types-run-state.h"

/*
 * save_dirty: at VM pause, write the guest-dirty pages (detected via the
 * kernel soft-dirty mechanism on /proc/<pid>/pagemap) to a file and madvise
 * them away with MADV_DONTNEED to reclaim RSS; at VM resume, restore the
 * pages from that file.
 */

void save_dirty_parse_options(const char *optstr, Error **errp);
void save_dirty_init(Error **errp);
bool save_dirty_is_enabled(void);
void save_dirty_pause(RunState state, bool enable_save_dirty,
                      bool has_quota, uint64_t quota);
void save_dirty_resume(void);

#endif /* SYSEMU_SAVE_DIRTY_H */
