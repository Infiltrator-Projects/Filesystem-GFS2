/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native read-only GFS2 namespace. Included by gfs2.c after the bounded
 * recognition helpers; every storage access remains a Common V2 host read.
 * Only stable, exclusively held, lock_nolock media with one clean journal
 * are supported. The current directory reader handles linear stuffed
 * directories; extended-hash directories fail closed.
 */
#define BLOCK_MAX 4096u
#define GROUP_MAX 1024u
#define JOURNAL_MAX (UINT64_C(128) * 1024u * 1024u)
#define DIR_HEADER 40u
#define MODE_TYPE 0170000u
#define MODE_DIR 0040000u
#define MODE_FILE 0100000u
#define MODE_LINK 0120000u

typedef struct {
    uint64_t formal, block;
    uint32_t type;
} Ref;
typedef struct {
    uint64_t header, first;
    uint32_t count, length, bitmap_bytes;
} Group;
typedef struct {
    IfsFsSessionLinkV2 session;
    IfsFsHostRuntimeV2 runtime;
    const IfsFsHostRuntimeV2 *host;
    uint32_t block, groups;
    uint64_t free_blocks;
    Ref root, master;
    Group rg[GROUP_MAX];
    int verified;
} Volume;

static IfsFsSessionRegistryV2 sessions = IFS_FS_SESSION_REGISTRY_INIT_V2;
typedef struct {
    Ref ref;
    uint64_t size;
    uint32_t mode, flags, height, entries;
    uint8_t bytes[BLOCK_MAX];
} Inode;

/* Both on-disk checksum families are reflected CRCs. GFS2's disk hash is
 * IEEE CRC32 with all-ones seed and final complement; its extended journal
 * checksum uses CRC32C and retains the running (uncomplemented) result. */
static uint32_t crc_update(uint32_t crc, const uint8_t *p, uint32_t n,
                           uint32_t polynomial)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8u; ++i)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & polynomial);
    }
    return crc;
}
static uint32_t disk_hash(const uint8_t *p, uint32_t n)
{
    return ~crc_update(UINT32_MAX, p, n, UINT32_C(0xedb88320));
}
static int block_valid(const Volume *v, uint64_t b)
{
    return b > SB_OFFSET / v->block && b < v->host->volume_bytes / v->block;
}
static int block_read(const Volume *v, uint64_t b, uint8_t *out)
{
    if (!block_valid(v, b)) return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(v->host, b * v->block, out, v->block);
}
/* The resource index and its bitmap headers are checked before any
 * externally visible operation. Bootstrap reads to locate rindex are
 * bounded; afterwards every referenced inode/data/indirect block must
 * have the corresponding native allocation state. */
static int state(const Volume *v, uint64_t b, unsigned wanted)
{
    if (!block_valid(v, b)) return IFS_FS_CORRUPT;
    if (!v->verified) return IFS_FS_OK;
    uint32_t lo = 0, hi = v->groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const Group *g = &v->rg[mid];
        if (b < g->first) hi = mid;
        else if (b - g->first >= g->count) lo = mid + 1u;
        else {
            uint64_t relative = b - g->first;
            uint32_t byte = (uint32_t)(relative / 4u);
            uint32_t first = v->block - 128u;
            uint64_t bitmap = g->header;
            uint32_t offset;
            uint8_t value;
            if (byte < first) offset = 128u + byte;
            else {
                byte -= first;
                bitmap += 1u + byte / (v->block - 24u);
                offset = 24u + byte % (v->block - 24u);
            }
            if (bitmap - g->header >= g->length) return IFS_FS_CORRUPT;
            int rc = ifs_fs_host_read_bytes_v2(v->host,
                bitmap * v->block + offset, &value, 1u);
            if (rc != IFS_FS_OK) return rc;
            return ((value >> ((relative % 4u) * 2u)) & 3u) == wanted ?
                IFS_FS_OK : IFS_FS_CORRUPT;
        }
    }
    return IFS_FS_CORRUPT;
}
static uint32_t dtype(uint32_t mode)
{
    return (mode & MODE_TYPE) >> 12;
}
static int load_inode(const Volume *v, Ref ref, Inode *i)
{
    int rc = state(v, ref.block, 3u);
    if (rc != IFS_FS_OK) return rc;
    rc = block_read(v, ref.block, i->bytes);
    if (rc != IFS_FS_OK) return rc;
    const uint8_t *p = i->bytes;
    if (!header(p, 4u, 400u) || !ref.formal ||
        be64(p + 24) != ref.formal || be64(p + 32) != ref.block ||
        !be32(p + 52) || !be64(p + 64) ||
        be64(p + 64) > v->host->volume_bytes / v->block)
        return IFS_FS_CORRUPT;
    i->ref = ref;
    i->mode = be32(p + 40);
    i->size = be64(p + 56);
    i->flags = be32(p + 128);
    i->height = be16(p + 138);
    i->entries = be32(p + 148);
    if ((ref.type && ref.type != dtype(i->mode)) ||
        i->height > 10u || i->size > v->host->volume_bytes ||
        (!i->height && i->size > v->block - DINODE_BYTES))
        return IFS_FS_CORRUPT;
    if (i->flags & UINT32_C(0x20000000)) return IFS_FS_UNSUPPORTED;
    return IFS_FS_OK;
}
/* Walk a bounded native pointer tree. A zero pointer is a sparse extent,
 * not an I/O failure. Reject cycles and any pointer to a non-data block. */
static int map_block(const Volume *v, const Inode *i, uint64_t logical,
                     uint64_t *physical)
{
    uint32_t slots = (v->block - 24u) / 8u, path[10];
    uint64_t previous[10];
    uint8_t indirect[BLOCK_MAX];
    if (!i->height || i->height > 10u) return IFS_FS_CORRUPT;
    for (uint32_t d = i->height; d > 1u; --d) {
        path[d - 1u] = (uint32_t)(logical % slots);
        logical /= slots;
    }
    if (logical >= (v->block - DINODE_BYTES) / 8u) return IFS_FS_CORRUPT;
    path[0] = (uint32_t)logical;
    uint64_t p = be64(i->bytes + DINODE_BYTES + path[0] * 8u);
    for (uint32_t d = 0; d < i->height && p; ++d) {
        if (p == i->ref.block) return IFS_FS_CORRUPT;
        for (uint32_t k = 0; k < d; ++k)
            if (previous[k] == p) return IFS_FS_CORRUPT;
        previous[d] = p;
        int rc = state(v, p, 1u);
        if (rc != IFS_FS_OK) return rc;
        if (d + 1u == i->height) break;
        rc = block_read(v, p, indirect);
        if (rc != IFS_FS_OK) return rc;
        if (!header(indirect, 5u, 500u)) return IFS_FS_CORRUPT;
        p = be64(indirect + 24u + path[d + 1u] * 8u);
    }
    *physical = p;
    return IFS_FS_OK;
}
static int inode_read(const Volume *v, const Inode *i, uint64_t at,
                      void *destination, uint32_t length, int allow_holes)
{
    uint8_t *out = destination;
    if ((i->mode & MODE_TYPE) != MODE_FILE || (!out && length) ||
        at > i->size || length > i->size - at)
        return IFS_FS_INVALID;
    if (!i->height) {
        for (uint32_t k = 0; k < length; ++k)
            out[k] = i->bytes[DINODE_BYTES + at + k];
        return IFS_FS_OK;
    }
    /* Do not apply the regular-file layout to journalled data variants
     * before independent qualification of those media. */
    if (i->flags & 1u) return IFS_FS_UNSUPPORTED;
    while (length) {
        uint64_t p;
        uint32_t skip = (uint32_t)(at % v->block);
        uint32_t n = v->block - skip;
        if (n > length) n = length;
        int rc = map_block(v, i, at / v->block, &p);
        if (rc != IFS_FS_OK) return rc;
        if (!p) {
            if (!allow_holes) return IFS_FS_CORRUPT;
            for (uint32_t k = 0; k < n; ++k) out[k] = 0;
        } else {
            rc = ifs_fs_host_read_bytes_v2(v->host, p * v->block + skip, out, n);
            if (rc != IFS_FS_OK) return rc;
        }
        out += n; at += n; length -= n;
    }
    return IFS_FS_OK;
}
static int equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}
static int dot(const uint8_t *p, uint32_t n)
{
    return n == 1u && p[0] == '.';
}
static int dotdot(const uint8_t *p, uint32_t n)
{
    return n == 2u && p[0] == '.' && p[1] == '.';
}
/* Validate the entire stuffed directory before returning a result. Hidden
 * dot entries are required on disk, but excluded from user enumeration.
 * An initial deleted sentinel is allowed; later sentinels are corruption. */
static int scan_dir(const Volume *v, const Inode *dir, const uint8_t *name,
                    uint32_t name_n, uint32_t index, Ref *ref,
                    const uint8_t **found, uint32_t *found_n)
{
    const uint8_t *p = dir->bytes;
    uint32_t seen[BLOCK_MAX / DIR_HEADER], count = 0, visible = 0;
    uint32_t dots = 0, parents = 0;
    int match = 0;
    if ((dir->mode & MODE_TYPE) != MODE_DIR) return IFS_FS_INVALID;
    if (dir->height || (dir->flags & 2u)) return IFS_FS_UNSUPPORTED;
    if (dir->size != v->block - DINODE_BYTES || be16(p + 146) ||
        be32(p + 132) != 1200u) return IFS_FS_CORRUPT;
    for (uint32_t off = DINODE_BYTES; off < v->block;) {
        if (v->block - off < DIR_HEADER) return IFS_FS_CORRUPT;
        const uint8_t *d = p + off;
        uint32_t record = be16(d + 20), n = be16(d + 22);
        Ref item = {be64(d), be64(d + 8), be16(d + 24)};
        if (record < DIR_HEADER || record % 8u || record > v->block - off)
            return IFS_FS_CORRUPT;
        if (!item.formal || !item.block) {
            if (off != DINODE_BYTES || item.formal || item.block)
                return IFS_FS_CORRUPT;
        } else {
            const uint8_t *s = d + DIR_HEADER;
            if (!n || n > 255u || n > record - DIR_HEADER ||
                !block_valid(v, item.block) || !item.type ||
                disk_hash(s, n) != be32(d + 16))
                return IFS_FS_CORRUPT;
            for (uint32_t k = 0; k < n; ++k)
                if (!s[k] || s[k] == '/') return IFS_FS_CORRUPT;
            for (uint32_t k = 0; k < count; ++k) {
                const uint8_t *old = p + seen[k];
                if (be16(old + 22) == n && equal(old + DIR_HEADER, s, n))
                    return IFS_FS_CORRUPT;
            }
            if (count >= BLOCK_MAX / DIR_HEADER) return IFS_FS_CORRUPT;
            seen[count++] = off;
            if (dot(s, n)) {
                if (item.formal != dir->ref.formal || item.block != dir->ref.block ||
                    item.type != 4u) return IFS_FS_CORRUPT;
                ++dots;
            } else if (dotdot(s, n)) {
                if (item.type != 4u) return IFS_FS_CORRUPT;
                ++parents;
            } else {
                if ((name && n == name_n && equal(s, name, n)) ||
                    (!name && visible == index)) {
                    *ref = item; *found = s; *found_n = n; match = 1;
                }
                ++visible;
            }
        }
        off += record;
    }
    if (count != dir->entries || dots != 1u || parents != 1u)
        return IFS_FS_CORRUPT;
    return match ? IFS_FS_OK : name ? IFS_FS_NOT_FOUND : IFS_FS_END;
}
static int lookup(const Volume *v, const Inode *dir, const char *name,
                  uint32_t n, Ref *ref)
{
    const uint8_t *ignored; uint32_t ignored_n;
    return scan_dir(v, dir, (const uint8_t *)name, n, 0, ref, &ignored, &ignored_n);
}
/* Resource group geometry comes from actual rindex records; header and
 * bitmap content must corroborate them. No counters are fabricated. */
static int load_groups(Volume *v, const Inode *index)
{
    uint8_t rec[96], bitmap[BLOCK_MAX];
    uint64_t limit = v->host->volume_bytes / v->block, end = SB_OFFSET / v->block + 1u;
    if (!index->size || index->size % sizeof(rec)) return IFS_FS_CORRUPT;
    if (index->size / sizeof(rec) > GROUP_MAX) return IFS_FS_UNSUPPORTED;
    v->groups = (uint32_t)(index->size / sizeof(rec));
    for (uint32_t n = 0; n < v->groups; ++n) {
        int rc = inode_read(v, index, (uint64_t)n * sizeof(rec), rec, sizeof(rec), 0);
        if (rc != IFS_FS_OK) return rc;
        Group *g = &v->rg[n];
        g->header = be64(rec); g->length = be32(rec + 8);
        g->first = be64(rec + 16); g->count = be32(rec + 24);
        g->bitmap_bytes = be32(rec + 28);
        if (g->header < end || g->header >= limit || !g->length ||
            g->length > limit - g->header || g->first != g->header + g->length ||
            !g->count || g->count > limit - g->first ||
            g->count % 4u || g->bitmap_bytes != g->count / 4u)
            return IFS_FS_CORRUPT;
        uint64_t capacity = (uint64_t)(v->block - 128u) +
                            (uint64_t)(g->length - 1u) * (v->block - 24u);
        if (g->bitmap_bytes > capacity ||
            (g->length > 1u && g->bitmap_bytes <= capacity - (v->block - 24u)))
            return IFS_FS_CORRUPT;
        end = g->first + g->count;
        uint32_t left = g->bitmap_bytes, free = 0, dinodes = 0;
        uint32_t expected_free = 0, expected_dinodes = 0;
        for (uint32_t k = 0; k < g->length; ++k) {
            rc = block_read(v, g->header + k, bitmap);
            if (rc != IFS_FS_OK) return rc;
            uint32_t offset = k ? 24u : 128u;
            if (!header(bitmap, k ? 3u : 2u, k ? 300u : 200u))
                return IFS_FS_CORRUPT;
            if (!k) {
                if (be64(bitmap + 48) != g->first || be32(bitmap + 56) != g->count ||
                    be32(bitmap + 60) != g->bitmap_bytes)
                    return IFS_FS_CORRUPT;
                expected_free = be32(bitmap + 28);
                expected_dinodes = be32(bitmap + 32);
                uint32_t crc = be32(bitmap + 64);
                if (crc) {
                    for (unsigned j = 64; j < 68; ++j) bitmap[j] = 0;
                    if (disk_hash(bitmap, 128u) != crc) return IFS_FS_CORRUPT;
                }
            }
            uint32_t bytes = v->block - offset;
            if (bytes > left) bytes = left;
            for (uint32_t j = 0; j < bytes; ++j) {
                uint8_t b = bitmap[offset + j];
                for (unsigned s = 0; s < 8u; s += 2u) {
                    unsigned value = (b >> s) & 3u;
                    if (!value) ++free;
                    else if (value == 3u) ++dinodes;
                    else if (value == 2u) return IFS_FS_UNSUPPORTED;
                }
            }
            left -= bytes;
        }
        if (left || free != expected_free || dinodes != expected_dinodes)
            return IFS_FS_CORRUPT;
        v->free_blocks += free;
    }
    v->verified = 1;
    return IFS_FS_OK;
}
/* For the supported single-journal offline profile, inspect the complete
 * ring. A dirty newest valid header, ambiguous sequence, corrupted
 * checksum or unknown journal layout is never silently treated as clean. */
static int clean_journal(const Volume *v, const Inode *journal)
{
    uint8_t b[BLOCK_MAX];
    uint64_t best = 0;
    uint32_t flags = 0;
    int found = 0;
    if ((journal->mode & MODE_TYPE) != MODE_FILE || !journal->height ||
        !journal->size || journal->size % v->block ||
        journal->size > JOURNAL_MAX || (journal->flags & 1u))
        return IFS_FS_UNSUPPORTED;
    uint64_t count = journal->size / v->block;
    for (uint64_t n = 0; n < count; ++n) {
        int rc = inode_read(v, journal, n * v->block, b, v->block, 0);
        if (rc != IFS_FS_OK) return rc;
        if (be32(b) != MAGIC || be32(b + 4) != 8u) continue;
        if (!header(b, 8u, 800u) || be32(b + 40) != n ||
            be32(b + 36) >= count)
            return IFS_FS_CORRUPT;
        uint32_t hash = be32(b + 44), crc = be32(b + 48);
        for (unsigned k = 44; k < 48; ++k) b[k] = 0;
        if (disk_hash(b, 48u) != hash ||
            (crc && crc_update(UINT32_MAX, b + 52, v->block - 52u,
                              UINT32_C(0x82f63b78)) != crc))
            return IFS_FS_CORRUPT;
        uint64_t seq = be64(b + 24);
        if (found && seq == best) return IFS_FS_CORRUPT;
        if (!found || seq > best) { best = seq; flags = be32(b + 32); found = 1; }
    }
    return !found ? IFS_FS_CORRUPT :
        flags & 1u ? IFS_FS_OK : IFS_FS_UNSUPPORTED;
}
static int volume_query(void *volume, IfsFsVolumeInfoV2 *info)
{
    Volume *v = volume;
    if (!v || !info || info->structure_bytes < sizeof(*info)) return IFS_FS_INVALID;
    info->variant = FS_FORMAT;
    info->max_name_bytes = 255u;
    info->block_bytes = v->block;
    info->total_bytes = v->host->volume_bytes;
    info->free_bytes = v->free_blocks * v->block;
    return IFS_FS_OK;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host, void **out,
                        IfsFsVolumeInfoV2 *info)
{
    uint8_t sb[512]; uint32_t variant;
    Inode master, index, jindex, journal;
    Ref index_ref, jindex_ref, journal_ref;
    if (!out || !info || info->structure_bytes < sizeof(*info)) return IFS_FS_INVALID;
    *out = NULL;
    int rc = probe(host, &variant);
    if (rc != IFS_FS_OK) return rc;
    if ((host->capabilities & (IFS_FS_HOST_READ_ONLY | IFS_FS_HOST_EXCLUSIVE)) !=
        (IFS_FS_HOST_READ_ONLY | IFS_FS_HOST_EXCLUSIVE)) return IFS_FS_ACCESS;
    rc = ifs_fs_host_read_bytes_v2(host, SB_OFFSET, sb, sizeof(sb));
    if (rc != IFS_FS_OK) return rc;
    if (!protocol(sb + 96, "lock_nolock")) return IFS_FS_UNSUPPORTED;
    Volume *v = host->allocate(host->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    ifs_fs_session_copy_host_v2(&v->runtime, host);
    v->host = &v->runtime; v->block = be32(sb + 36); v->groups = 0;
    v->free_blocks = 0; v->verified = 0;
    v->master = (Ref){be64(sb + 48), be64(sb + 56), 4u};
    v->root = (Ref){be64(sb + 80), be64(sb + 88), 4u};
    rc = load_inode(v, v->master, &master);
    if (!rc) rc = lookup(v, &master, "rindex", 6u, &index_ref);
    if (!rc) rc = load_inode(v, index_ref, &index);
    if (!rc) rc = load_groups(v, &index);
    /* Revalidate the bootstrap dinodes and rindex data with the native
     * allocation map now available, not just their superblock addresses. */
    if (!rc) rc = load_inode(v, v->master, &master);
    if (!rc) rc = load_inode(v, index_ref, &index);
    if (!rc) {
        uint8_t record[96];
        for (uint32_t n = 0; n < v->groups && !rc; ++n) {
            rc = inode_read(v, &index, (uint64_t)n * 96u, record, 96u, 0);
            if (!rc && (be64(record) != v->rg[n].header ||
                be32(record + 8) != v->rg[n].length ||
                be64(record + 16) != v->rg[n].first ||
                be32(record + 24) != v->rg[n].count ||
                be32(record + 28) != v->rg[n].bitmap_bytes))
                rc = IFS_FS_CORRUPT;
        }
    }
    if (!rc) rc = lookup(v, &master, "jindex", 6u, &jindex_ref);
    if (!rc) rc = load_inode(v, jindex_ref, &jindex);
    if (!rc && jindex.entries != 3u) rc = IFS_FS_UNSUPPORTED;
    if (!rc) rc = lookup(v, &jindex, "journal0", 8u, &journal_ref);
    if (!rc) rc = load_inode(v, journal_ref, &journal);
    if (!rc) rc = clean_journal(v, &journal);
    if (!rc) rc = load_inode(v, v->root, &master);
    if (!rc) {
        const uint8_t *ignored; uint32_t ignored_n; Ref ignored_ref;
        rc = scan_dir(v, &master, NULL, 0, UINT32_MAX,
                      &ignored_ref, &ignored, &ignored_n);
        if (rc == IFS_FS_END) rc = IFS_FS_OK;
    }
    if (rc) {
        host->release(host->context, v);
        return rc == IFS_FS_NOT_FOUND ? IFS_FS_CORRUPT : rc;
    }
    rc = volume_query(v, info);
    if (!rc) rc = ifs_fs_session_publish_v2(&sessions, &v->session, v, out);
    if (rc) v->host->release(v->host->context, v);
    return rc;
}
static int unmount_volume(void *handle)
{
    void *raw;
    int rc = ifs_fs_session_lookup_v2(&sessions, handle, 1, &raw);
    if (rc) return rc;
    Volume *v = raw;
    v->host->release(v->host->context, v);
    return IFS_FS_OK;
}

static int resolve(const Volume *v, const char *path, Inode *i,
                   const char **name, uint32_t *length)
{
    if (!v || !path || *path != '/') return IFS_FS_INVALID;
    int rc = load_inode(v, v->root, i);
    if (rc) return rc;
    *name = path; *length = 1;
    const char *p = path + 1;
    for (uint32_t depth = 0; *p; ++depth) {
        if (depth >= 64u) return IFS_FS_UNSUPPORTED;
        uint32_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n == 255u) return IFS_FS_INVALID;
            ++n;
        }
        if (!n || dot((const uint8_t *)p, n) || dotdot((const uint8_t *)p, n))
            return IFS_FS_INVALID;
        Ref next;
        rc = lookup(v, i, p, n, &next);
        if (rc) return rc;
        rc = load_inode(v, next, i);
        if (rc) return rc;
        *name = p; *length = n;
        p += n;
        if (*p == '/') {
            ++p;
            if (!*p) return IFS_FS_INVALID;
        }
    }
    return IFS_FS_OK;
}
static int entry_info(const Inode *i, const uint8_t *name, uint32_t n,
                      IfsFsEntryV2 *entry)
{
    if (!entry || entry->structure_bytes < sizeof(*entry) ||
        !entry->name || entry->name_capacity <= n) return IFS_FS_INVALID;
    switch (i->mode & MODE_TYPE) {
    case MODE_DIR: entry->type = IFS_FS_ENTRY_DIRECTORY; break;
    case MODE_FILE: entry->type = IFS_FS_ENTRY_FILE; break;
    case MODE_LINK: entry->type = IFS_FS_ENTRY_LINK; break;
    default: return IFS_FS_UNSUPPORTED;
    }
    entry->size_bytes = i->size;
    entry->attributes = 0; /* no generic attribute mapping advertised */
    entry->name_bytes = n;
    for (uint32_t j = 0; j < n; ++j) entry->name[j] = (char)name[j];
    entry->name[n] = 0;
    return IFS_FS_OK;
}
static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    Inode i; const char *name; uint32_t n;
    int rc = resolve(volume, path, &i, &name, &n);
    return rc ? rc : entry_info(&i, (const uint8_t *)name, n, entry);
}
static int directory_entry(void *volume, const char *path, uint32_t index,
                           IfsFsEntryV2 *entry)
{
    Inode dir, i; const char *name; const uint8_t *found; uint32_t n;
    Ref ref;
    int rc = resolve(volume, path, &dir, &name, &n);
    if (!rc) rc = scan_dir(volume, &dir, NULL, 0, index, &ref, &found, &n);
    if (!rc) rc = load_inode(volume, ref, &i);
    return rc ? rc : entry_info(&i, found, n, entry);
}
static int read_path(void *volume, const char *path, uint64_t offset,
                     void *destination, uint32_t length)
{
    Inode i; const char *name; uint32_t n;
    int rc = resolve(volume, path, &i, &name, &n);
    return rc ? rc : inode_read(volume, &i, offset, destination, length, 1);
}
/* File operations reserve the decoded volume for the whole call. A host
 * can reenter from I/O or attempt unmount from another thread. Both paths
 * observe BUSY until the owner releases this mount's reservation. */
static int mounted_stat(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    void *raw;
    int rc = ifs_fs_session_lookup_v2(&sessions, handle, 0, &raw);
    if (rc) return rc;
    Volume *v = raw;
    rc = stat_path(v, path, entry);
    ifs_fs_session_return_v2(&sessions, &v->session);
    return rc;
}

static int mounted_directory_entry(void *handle, const char *path, uint32_t index, IfsFsEntryV2 *entry)
{
    void *raw;
    int rc = ifs_fs_session_lookup_v2(&sessions, handle, 0, &raw);
    if (rc) return rc;
    Volume *v = raw;
    rc = directory_entry(v, path, index, entry);
    ifs_fs_session_return_v2(&sessions, &v->session);
    return rc;
}

static int mounted_read(void *handle, const char *path, uint64_t offset, void *buffer, uint32_t length)
{
    void *raw;
    int rc = ifs_fs_session_lookup_v2(&sessions, handle, 0, &raw);
    if (rc) return rc;
    Volume *v = raw;
    rc = read_path(v, path, offset, buffer, length);
    ifs_fs_session_return_v2(&sessions, &v->session);
    return rc;
}

static int mounted_query(void *handle, IfsFsVolumeInfoV2 *out)
{
    void *raw;
    int rc = ifs_fs_session_lookup_v2(&sessions, handle, 0, &raw);
    if (rc) return rc;
    Volume *v = raw;
    rc = volume_query(v, out);
    ifs_fs_session_return_v2(&sessions, &v->session);
    return rc;
}

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0, IFS_FS_VOLUME_QUERY},
    .query = mounted_query
};
