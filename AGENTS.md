# AGENTS.md

## Repository purpose

- This repository is a QEMU 6.2.0 source tree with a custom **vm-swap** feature.
- vm-swap saves guest-dirty host pages when a VM is paused, reclaims their RSS with
  `MADV_DONTNEED`, and restores them when the VM resumes.
- The feature is enabled with `-dirty-save dir=PATH[,quota=N]`; `dir` is both the
  working directory and the enable switch, and `quota` caps the bytes saved per pause
  (`0` or omitted means unlimited).
- Dirty pages are detected through the kernel soft-dirty mechanism
  (`/proc/<pid>/pagemap` + `/proc/<pid>/clear_refs`), so the granularity is the host page.
- `vm_swap/` is a standalone fork/dirty-save test harness. It boots a template VM,
  snapshots it, forks four children from the shared memory file, and exercises repeated
  pause/resume rounds while reporting host memory usage.

## Documentation index

- `vm_swap/README.md` — how to build and run the vm-swap harness, CLI options, logs,
  cleanup, FAQ, and a measured `normal` vs `swap + dirty-save` comparison. Sections
  1.1-1.2 also contain the original feature requirements and their mapping to the
  implementation.
- `vm_swap/BUILD.md` — QEMU build and install guide, including the `vm_swap/build.sh`
  wrapper and the current `/opt/qemu-exp` install prefix.
- `vm_swap/COMPILE_OPTIONS.md` — reference for QEMU `configure` options and how to
  inspect them.
- `docs/dirty-save-format.md` — binary layout of `dirty_snapshot.bin` (32-byte header,
  per-block records, slot map, packed page data).
- `plan.md` — original design and implementation plan for saving/dropping dirty pages
  on pause and restoring them on resume.
- `qemu-options.hx` — user-facing help text for `-dirty-save`
  (`DEF("dirty-save", ...)` and the `SRST` section).

## Key source locations

- `softmmu/save_dirty.c` — option parsing, soft-dirty probing, save/resume file I/O,
  and `madvise`.
- `include/sysemu/save_dirty.h` — `save_dirty_init()`, `save_dirty_pause()`,
  `save_dirty_resume()`.
- `softmmu/cpus.c` — pause hook in `do_vm_stop()` and resume hook in `vm_start()`.
- `softmmu/vl.c` — `-dirty-save` option handling and `save_dirty_init()` call.
- `vm_swap/vm_swap.py` — test harness entry point.
- `vm_swap/virtcontainer.py` — trimmed QEMU template/child and tap helpers.
- `vm_swap/vm_utils.py` — command runner and QMP client.
- `vm_swap/agent/` — `server_qemu`/`client_qemu` sources and binaries.
- `vm_swap/dirtier/` — dirty-page generator used by `--dirtier`.

## Build and test

- Install build dependencies with `./vm_swap/install.sh` from the repository root.
- Build and install QEMU with `./vm_swap/build.sh` (defaults to `/opt/qemu-exp`).
- Verify the installed binary supports the feature:
  `/opt/qemu-exp/bin/qemu-system-x86_64 --help 2>&1 | grep dirty-save`.
- Run the harness from a directory containing `base.qcow2` and `id_rsa`, for example:
  `cd /home/rtzhong/qemu-playground && sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode swap --dirty-save --dirtier`.
- The harness needs root privileges for tap creation, KVM access, and guest ssh.
- The harness uses `input()` prompts; it can run interactively or with piped newlines.
- Test logs and VM state are written under `./vm_swap_log`.

## Commit message conventions

- Write commit messages in English.
- Keep the subject concise and imperative, ideally 72 characters or fewer, without a
  trailing period.
- Every bullet point in the body must begin with a marker such as `- `.
- Use the body only when it adds information; wrap lines at 72 characters.
- Do not include assistant/tool attribution, co-author lines, or generated-by footers.
- Do not mention vendor or assistant product names in commit messages.
- Example:
  ```
  vm_swap: report host memory across pause/resume rounds

  - Sample host used memory with free -b before pause, after pause, and after resume
  - Print reclaimed and post-resume deltas in GiB or MiB
  - Summarize average values after all rounds
  ```

## Coding conventions

- Keep vm-swap changes scoped and minimal; prefer fixing the root cause over adding
  layered defenses.
- Keep `vm_swap/` self-contained: no imports from `../EmuFork` or other out-of-tree paths.
- Do not commit built binaries; `vm_swap/.gitignore` excludes them.
- Rebuild agent binaries automatically through `ensure_agent_binaries()`; rebuild
  `dirtier` automatically through `ensure_dirtier_binary()` when `--dirtier` is used.
- Preserve the existing QEMU coding style in `softmmu/` and public headers.
