# Filesystem-GFS2

Independent native Global File System 2 provider using only Common V2 and partition-relative host I/O. This is a bounded offline reader, not a clustered filesystem writer.

## Supported media and operations

- GFS2 filesystem format 1802 and multihost format 1900; 512–4096-byte filesystem blocks.
- Recognition validates superblock geometry, lock protocol and master/root dinode identities.
- Read-only mount requires a stable, exclusively held host, lock_nolock, one clean journal, and the supported resource-group and namespace layouts.
- Native rindex and resource bitmaps establish measured free space and allocation states. Directory and file lookup validate actual inode identities, directory hashes and metadata boundaries.
- Stuffed directories, stuffed files, ordinary direct/indirect data pointers and sparse file reads; CORE probe/mount/stat/list/read and VOLUME query.
- Extended-hash directory traversal, unqualified journalled data layouts, multiple journals, dirty journal recovery, clustered mount, mutation, formatting, repair, allocation maps, defragmentation and physical BOOT/kernel integration are not supported. Unsupported callbacks are absent or reject the media profile.

## Build and release

`make check` uses the pinned Common V2 header. The release workflow requires the actual qualification job, including independent mkfs.gfs2 media, populated byte fixtures, malformed-media tests, and sanitizer runs, before publishing the native RUN and portable DEB. The formatter is an independent test oracle only; it is not invoked by the provider.

The source-downloading installer builds the hosted module and installs `/System/Filesystems/GFS2.so` plus `/System/Resources/Documentation/Filesystem-GFS2.pdf`. The documentation-only v0.0.1 release remains immutable; v0.1.0 is a candidate until published. Import-free development tests do not qualify the module for actual UEFI execution. Independent populated-media payload interoperability remains an open qualification item.

## References

- [Technical reference PDF](docs/GFS2.pdf)
- [Technical reference source](docs/GFS2-TECHNICAL-REFERENCE.md)
- [Qualification and migration](MIGRATION_STATUS.md)
- [Original native design](native/filesystems/gfs2/DESIGN.md)
- [Pinned central format source](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/gfs2)
- [Common V2 ABI](https://github.com/Infiltrator-Projects/Libraries)
- [Project philosophy](docs/PHILOSOPHY.md)

New implementation code is GPL-3.0-or-later. Historical source and evidence retain their original notices and licensing.
