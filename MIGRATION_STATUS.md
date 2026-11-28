# GFS2 Common V2 migration status

## Source and ownership

[Exact central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/gfs2). Editable Linux v6.12.107-derived source remains centrally. Shared-disk mutation requires cluster locking and recovery proof. Extract and adapt the existing implementation; preserve original licensing, attribution and provenance. New project-authored code uses GPL-3.0-or-later. No source transferred by this documentation commit.

## Qualification plan

1. Enumerate supported on-disk variants, validate geometry and features; implement bounded CORE probe.
2. Provide CORE mount/stat/list/read and VOLUME query using native superblock, resource groups, inodes, directory hashes, journals and cluster lock identities.
3. Test independent clean single-node images, resource-group corruption and journal/lock contradictions; compare independent payloads and unchanged-image hashes; inject malformed media and I/O faults.
4. Implement mutation, format, check/repair and DEFRAG map/plan/execute/verify/recover only with proven allocation, durability and interruption/recovery semantics. Unsupported operations must refuse before writing.
5. Qualify exact packaged Common V2 ABI, hosted and import-free freestanding profiles, then actual BOOT and kernel integration independently.

Native recognition development now implements the Common V2 CORE PROBE for format 1802 / multihost 1900. It validates bounded big-endian superblock geometry, supported lock protocol, and independent master/root dinode identities and types. Constructed corruption fixtures and independent formatter-image tests are implemented; exact revision CI must pass before any test claim. The freestanding build has no runtime imports; this is not BOOT qualification. No mount, namespace reads, allocation, journal cleanliness, mutation or defragmentation is advertised. The published installer remains documentation-only until a qualified operational provider exists.

The [design and qualification reference](docs/GFS2.pdf) is available. It is not an operational manual; release and installation qualification remain pending.
