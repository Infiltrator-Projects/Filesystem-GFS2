# GFS2: technical reference

## History and cluster model

The Global File System family originated in shared-disk research at the University of Minnesota and was developed by Sistina and Red Hat. GFS2 entered Linux 2.6.19. Multiple cluster nodes may access the same disk; distributed locking, cache invalidation, fencing and per-node journals maintain consistency. GFS2 is not a network filesystem protocol and a shared block device is not by itself safe concurrent storage. A standalone offline reader can parse a stable image without becoming a cluster participant. A writer must either implement the native locking/recovery protocol or prove exclusive offline ownership; it must never write a disk still mounted elsewhere.

## Metadata header and superblock

All native multibyte fields are big-endian. A 24-byte metadata header has magic 0x01161970 at 0, type 4, reserved eight bytes at 8, format 16 and journal/reserved word 20. Superblock is at 128 basic 512-byte sectors = byte 65536, independent of filesystem block size. Its format number is at 24, multihost format 28, block size 36, log2 block size 40, master-directory inode pair 48, root pair 80, 64-byte lock protocol string 96 and lock table 160, UUID 256. An inode pair is formal number then physical block address (64-bit each). Linux v6.12 supports filesystem format 1802 and multihost 1900, with legacy feature distinctions. Confirm geometry, format and lock configuration before trusting master or root references.

## Master directory and resource index

The master directory is a separate namespace containing filesystem-control objects, including rindex, jindex and per-node control metadata. Ordinary root directory is the user-visible namespace. The resource index file has 96-byte records: group header block at 0 (64-bit), header/bitmap block count at 8, reserved at 12, first data block at 16 (64-bit), data-block count at 24, bitmap-byte count at 28, reserved bytes 32..95. Locate rindex through master-directory traversal, not through a hardcoded block number. Validate ordering, group range disjointness, bitmap capacity, group-header identity and all metadata/data boundaries. Journal index lists journal inodes; map each through its own inode data tree.

## Resource group geometry

Resource group header is 128 bytes. After common header: flags 24, free block count 28, dinode count 32, group-skip field 36, inode generation 40 (64-bit), first data block 48 (64-bit), data count 56, bitmap bytes 60 and CRC 64. CRC applies to the native header with checksum field zero; verify the specific revision's CRC routine. Bitmap bytes follow the header in its first block and the 24-byte metadata headers of subsequent resource-bitmap blocks. Each byte holds four two-bit states, least-significant pair first: 0 free, 1 ordinary used, 2 unlinked dinode, 3 allocated dinode. For group-relative block 13, bitmap byte 3, shift 2. Decode the correct bitmap payload block before applying the shift. Do not allocate a group flagged no-alloc or contrary to its metadata/data restrictions.

## Dinode field inventory

Dinode occupies one filesystem block with a 232-byte fixed header. Common metadata header at 0; formal/address inode pair 24/32; mode 40, UID 44, GID 48, links 52; size 56, allocated blocks 64, atime 72, mtime 80, ctime 88 (64-bit); device major/minor 96/100; allocation goals 104/112, generation 120; flags 128, payload format 132, tree height 138; directory depth 146 and entry count 148; EA block address 168; nanosecond times 176/180/184. Validate inode pair against physical location and directory reference. Height zero is stuffed data inside the dinode after byte 232. Flags select journalled data, extended-hash directories and indirect EA formats. The formal inode number and disk address are separate identities; stale references must be detected.

## File block trees and worked example

GFS2 uses block-pointer trees rather than an extent tree. For normal 4096-byte blocks, the dinode has floor((4096-232)/8)=483 pointers; an indirect metadata block with its 24-byte header contains 509 pointers. Height one maps up to 483 ordinary data blocks directly; height two maps up to 483*509 blocks. At height two, logical block 600 selects dinode pointer 1 and indirect pointer 91. Read the indirect block, verify its metadata type and format, then obtain the data block address. Height zero file payload capacity is 3864 bytes. Journalled-data metadata objects and directory data may reserve a block header, so use the correct payload unit. Zero pointers can denote holes; a malformed indirect node is not a hole. File EOF overrides tree capacity.

## Directories and extendible hashing

Directory records have a 40-byte fixed header: formal inode at 0, physical address 8, hash 16, record length 20, name length 22, type 24, readahead 26, reserved 28..39; name begins at 40. Minimum record length is align_up(40+name_length,8). A zero formal number or address denotes a sentinel. Validate records against their stuffed or leaf payload boundary. Small directories are stuffed; larger directories use extended hashing. With global depth d, the hash table contains 2^d big-endian 64-bit leaf addresses indexed by the high d bits of the native 32-bit filename hash. Local leaf depth can be smaller than global depth, so multiple table entries legitimately identify the same leaf. Compare names after hash matches and avoid counting aliased leaves repeatedly.

## Leaf structure and namespace mutation

A leaf has common metadata header, 16-bit depth at 24, entry count 26, dirent format 28, overflow leaf address 32, and 64-byte version-dependent area from 40; records start at 104. Some revisions store directory identity and chain information in that area. Verify local depth, bounded overflow chains, hash-prefix membership and correct owner. Insertion may split a leaf, redistribute records and double the global table when local depth reaches global depth; if further splitting is impractical the format supports overflow chains. Rename across directories requires coherent parent and link-count changes. Cluster writers must hold appropriate ordered locks across source and destination. Native filename hash and directory format are compatibility requirements, not interchangeable hash choices.

## Extended attributes and quotas

EA records begin with a 16-byte header: record length 0, value length 4, name length 8, namespace type 9, flags 10, external pointer count 11, padding 12. The name follows without a terminator; values are either stuffed or referenced by aligned 64-bit block pointers. EA and external data blocks carry their own metadata headers. The inode's EA address may point directly to an EA block or to an indirect list. Validate lengths and namespace type; do not discard security attributes. Quota records contain 64-bit limit, warning and value followed by reserved bytes; per-node quota changes and statfs deltas must be consolidated under the appropriate locking protocol. Global counters alone can be stale in a live cluster.

## Journal format and replay

Each journal is a circular file, independent of the physical placement of other journals. Log header has common metadata header, 64-bit sequence at 24, flags 32, tail 36, logical journal block number 40, hash 44; later header adds CRC32C at 48, nanosecond time 52 and further identity and local-accounting fields. Log descriptors have type at 24, chunk length 28, descriptor-specific counts 32/36, reserved through 71. Types 300,301,302 describe metadata, revokes and journalled data. Validate complete committed log intervals and descriptor target addresses; process revokes to prevent replay of stale images. Replay only under appropriate journal ownership, fencing and recovery locks. A clean unmount flag is useful evidence but not a substitute for metadata validation.

## Allocation and consistency

Allocate from the correct resource group under its allocation lock, update two-bit state and counters, and journal the bitmap and owner metadata as one transaction. Distinguish a live dinode from an unlinked dinode awaiting reclaim. A block can be reclaimed only when references and the native locking/recovery lifecycle allow it. Structural checking reconstructs root and master namespace ownership, indirect blocks, EA blocks, journal/control files and group metadata, then reconciles resource maps. Formal inode generations and physical inode addresses must remain coherent. For offline repair, obtain exclusive stable access and do not apply an online cluster's cached view to a changed disk.

## Relocation and qualification

Defragmentation can consolidate physical pointer runs without changing file identity or contents. A plan must derive actual ownership, reserve destination blocks, copy and validate payload, publish the new pointers under journal protection and retire old ownership after durable commit. Cluster mode additionally requires native glock ordering and cache coherency; offline exclusive mode still needs journal recovery and all metadata invariants. Test two-bit bitmap boundaries, stuffed-to-unstuffed transition, indirect depth growth, leaf aliasing and overflow, dirty multiple journals, revoke conflicts and orphaned dinodes. Independent mkfs.gfs2, fsck.gfs2 and gfs2_edit images establish interoperability; exact cluster-lock and fencing failure testing remains necessary before concurrent write claims.

## References and implementation boundary

Pinned structures: https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/gfs2_ondisk.h . Algorithms: https://github.com/torvalds/linux/tree/v6.12/fs/gfs2 (rgrp.c, bmap.c, dir.c, glock.c, recovery.c, log.c). Cluster locking notes: https://docs.kernel.org/filesystems/gfs2-glocks.html . User tools: https://pagure.io/gfs2-utils . The exact journal checksum/tag handling, legacy format compatibility, filename hash and distributed recovery protocols require additional focused specification and fixture validation. The original provider-status documents are retained as historical appendices. No operational provider or current release qualification is asserted by this manual.

The PDF preserves both earlier documents as historical appendices.
