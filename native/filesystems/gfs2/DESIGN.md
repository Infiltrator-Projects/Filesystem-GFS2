# GFS2 — native filesystem design

## Format scope

Shared-disk GFS2; local read-only qualification first. This is a design contract, not a claim of implemented capabilities.

## On-disk structures and ownership

Superblock, resource groups and allocation bitmaps, inodes, directories, block mappings, journals and quota records; filesystem-defined lock identities. The single native C/C++ engine owns these format semantics; consumers provide bounded Common V2 host services, never competing parsers.

## Integrity and mutation invariants

Reject inconsistent resource groups, unresolved journal replay, active writers or uncertain shared-disk state. Cluster mutation requires qualified distributed locking, fencing, lock ordering and node-failure recovery; exclusive local access alone cannot substitute. All arithmetic, offsets and lengths are checked before I/O. Writable operations require exclusive access, explicit ordering, flush and interruption/recovery evidence. Read-only operations leave the medium unchanged.

## Common V2 boundary

Only `infiltrator_filesystem_api_v2` is public. Target the genuine PROBE/MOUNT/UNMOUNT/STAT/LIST/READ operations first; negotiate actual supported operation masks. VOLUME, METADATA and DEFRAG operations (including MAP, PLAN, EXECUTE, VERIFY, RECOVER) are individually qualified or refused. Test Media uses the same provider's qualified format and namespace operations. No FUSE, legacy ABI, private driver or kernel proxy. Hosted and import-free freestanding profiles share the format engine, with generic environment-specific host services.

## Qualification

Use mkfs.gfs2 images, resource-group boundaries, directory trees, multiple journals and corrupt descriptors. Separately qualify concurrent nodes, loss of lock ownership and journal recovery. Compare file bytes against an independent oracle, hash unchanged read-only images, inject short I/O, corruption and interruption; validate exact published module separately in BOOT and kernel. Use disposable media only. No new independent binary or test pass is claimed.

## Source and migration

[Central source and historical design, pinned revision](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/gfs2). Adapt the retained implementation without erasing inherited notices or licences. New project-authored code uses GPL-3.0-or-later. See [migration status](../../../MIGRATION_STATUS.md), [philosophy](../../../docs/PHILOSOPHY.md) and [Common V2 contract](https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md). Source migration is a separate step.
