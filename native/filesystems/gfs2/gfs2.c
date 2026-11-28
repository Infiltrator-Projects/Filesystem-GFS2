/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native GFS2 recognition through Common V2. Format reference: Linux v6.12
 * include/uapi/linux/gfs2_ondisk.h and this repository's technical manual.
 * This is an original bounded reader, not a Linux VFS adapter.
 * Recognition is deliberately separate from journal and mount qualification.
 */
#include <infiltratr/filesystem_module_v2.h>

#define MAGIC UINT32_C(0x01161970)
#define SB_OFFSET UINT64_C(65536)
#define DINODE_BYTES 232u
#define FS_FORMAT 1802u
#define MULTI_FORMAT 1900u

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}
static uint32_t be32(const uint8_t *p)
{
    return (be16(p) << 16) | be16(p + 2);
}
static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}
static int header(const uint8_t *p, uint32_t type, uint32_t format)
{
    return be32(p) == MAGIC && be32(p + 4) == type &&
           be32(p + 16) == format;
}
/* Compare fixed-size, NUL-terminated lock protocol without unbounded reads. */
static int protocol(const uint8_t *p, const char *name)
{
    uint32_t i = 0;
    while (i < 64u && name[i] != '\0') {
        if (p[i] != (uint8_t)name[i]) return 0;
        ++i;
    }
    return i < 64u && p[i] == 0;
}
static int directory_inode(const IfsFsHostRuntimeV2 *host, uint32_t block,
                           uint64_t formal, uint64_t address)
{
    uint8_t inode[DINODE_BYTES];
    uint32_t height, depth, flags;
    uint64_t blocks = host->volume_bytes / block;
    int rc;
    /* Both inode numbers must agree with the authenticated superblock.
     * Division bounds the later byte multiplication without overflow. */
    if (!formal || address <= SB_OFFSET / block || address >= blocks)
        return IFS_FS_CORRUPT;
    rc = ifs_fs_host_read_bytes_v2(host, address * block, inode,
                                  (uint32_t)sizeof(inode));
    if (rc != IFS_FS_OK) return rc;
    if (!header(inode, 4u, 400u) || be64(inode + 24) != formal ||
        be64(inode + 32) != address ||
        (be32(inode + 40) & 0170000u) != 0040000u ||
        be32(inode + 52) < 2u || be32(inode + 148) < 2u ||
        !be64(inode + 64) || be64(inode + 64) > blocks ||
        be64(inode + 56) > host->volume_bytes)
        return IFS_FS_CORRUPT;
    height = be16(inode + 138);
    depth = be16(inode + 146);
    flags = be32(inode + 128);
    if (height > 10u || depth > 17u) return IFS_FS_CORRUPT;
    if (!(flags & 2u)) {
        if (height || depth || be64(inode + 56) > block - DINODE_BYTES)
            return IFS_FS_CORRUPT;
    } else if (!depth) {
        return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    uint8_t sb[512];
    uint32_t block, shift;
    uint64_t master_formal, master, root_formal, root;
    int rc;
    if (!variant || !ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    *variant = 0;
    if (host->volume_bytes < SB_OFFSET + sizeof(sb) ||
        host->volume_bytes % host->logical_sector_bytes)
        return IFS_FS_NOT_FOUND;
    rc = ifs_fs_host_read_bytes_v2(host, SB_OFFSET, sb, sizeof(sb));
    if (rc != IFS_FS_OK) return rc;
    if (be32(sb) != MAGIC) return IFS_FS_NOT_FOUND;
    if (!header(sb, 1u, 100u)) return IFS_FS_CORRUPT;
    if (be32(sb + 24) != FS_FORMAT || be32(sb + 28) != MULTI_FORMAT)
        return IFS_FS_UNSUPPORTED;
    block = be32(sb + 36);
    shift = be32(sb + 40);
    if (shift < 9u || shift > 12u || block != (1u << shift) ||
        host->volume_bytes % block)
        return IFS_FS_CORRUPT;
    if (!protocol(sb + 96, "lock_nolock") && !protocol(sb + 96, "lock_dlm"))
        return IFS_FS_UNSUPPORTED;
    master_formal = be64(sb + 48);
    master = be64(sb + 56);
    root_formal = be64(sb + 80);
    root = be64(sb + 88);
    if (master == root || master_formal == root_formal) return IFS_FS_CORRUPT;
    rc = directory_inode(host, block, master_formal, master);
    if (rc != IFS_FS_OK) return rc;
    rc = directory_inode(host, block, root_formal, root);
    if (rc != IFS_FS_OK) return rc;
    /* This result recognises a format; it says nothing about journal
     * cleanliness, allocation ownership, cluster fencing or mountability. */
    *variant = FS_FORMAT;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0, IFS_FS_CORE_PROBE},
    .probe = probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t id, uint32_t min, uint32_t max)
{
    return id == IFS_FS_GROUP_CORE && min <= IFS_FS_GROUP_ABI_V2 &&
           max >= IFS_FS_GROUP_ABI_V2 ? &core.header : NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "gfs2",
#ifdef IFS_GFS2_FREESTANDING
    .runtime_profiles = IFS_FS_PROFILE_FREESTANDING,
#else
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
#endif
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .query_group = query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
