# GFS2 Common V2 migration

The independent engine implements the restricted offline read-only subset described in [README.md](README.md). The original design and pinned central Linux v6.12.107 source remain reference material, not production backends.

## Executed development evidence

- [Initial recognition](https://github.com/Infiltrator-Projects/Filesystem-GFS2/actions/runs/46112970882): hosted, import-free, independent formatted-image and sanitizer tests passed.
- [Offline mount and root namespace](https://github.com/Infiltrator-Projects/Filesystem-GFS2/actions/runs/46113206664): exact-source hosted/import-free tests, independent clean image mount, root enumeration, unchanged media hash and sanitizer tests passed.
- [Populated namespace and corruption fixture](https://github.com/Infiltrator-Projects/Filesystem-GFS2/actions/runs/46113277248): exact-revision test workflow; verify its final conclusion before claiming its success.

## Remaining engineering

Independent populated-media payload comparison, extended-hash and overflow directory traversal, full allocation ownership reconciliation, multiple journal recognition and correct recovery, distributed locking and fencing, native mutation, format, check/repair, defragmentation and interruption recovery, BOOT and kernel integration. Do not advertise these operations before their implementation and qualification.

Publication of the bounded read-only runtime is gated by the actual development and package qualification workflow. It does not establish any of the outstanding capabilities.

### Mounted volume lifetime

The provider copies the caller's size-versioned host descriptor and assigns each mount an opaque generation. An operation reserves its mount until its callback returns. A concurrent unmount or second operation on that mount returns `IFS_FS_BUSY`. Independent mounts remain usable. Unmount removes the generation before releasing native state, so a closed handle returns `IFS_FS_INVALID` even after a new mount reuses the same memory. The Common V2 regression holds a directory read inside a host I/O callback while a second thread queries and unmounts both volumes.
