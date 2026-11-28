/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIZE (16u * 1024u * 1024u)
#define SB 65536u
static uint8_t image[SIZE];
struct context { int fd; uint64_t size; uint32_t sector; int fail; unsigned allocations; };
static int rd(void *p, uint64_t offset, void *out, uint32_t size)
{
    struct context *c = p;
    assert(size && size <= 4096u && size % c->sector == 0 &&
           offset % c->sector == 0 && offset <= c->size &&
           size <= c->size - offset);
    if (c->fail) return -1;
    if (c->fd >= 0)
        return pread(c->fd, out, size, (off_t)offset) == (ssize_t)size ? 0 : -1;
    memcpy(out, image + offset, size);
    return 0;
}
static void *alloc(void *p, size_t size)
{
    struct context *c = p;
    void *mem = malloc(size);
    if (mem) ++c->allocations;
    return mem;
}
static void release(void *p, void *mem)
{
    struct context *c = p;
    assert(mem && c->allocations);
    --c->allocations;
    free(mem);
}
static void p32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (24 - i * 8));
}
static void p64(uint8_t *p, uint64_t n)
{
    p32(p, (uint32_t)(n >> 32)); p32(p + 4, (uint32_t)n);
}
static void meta(uint8_t *p, unsigned type, unsigned format)
{
    p32(p, 0x01161970); p32(p + 4, type); p32(p + 16, format);
}
static void inode(unsigned block, unsigned address, unsigned formal)
{
    uint8_t *p = image + address * block;
    meta(p, 4, 400);
    p64(p + 24, formal); p64(p + 32, address);
    p32(p + 40, 0040755); p32(p + 52, 2);
    p64(p + 56, block - 232); p64(p + 64, 1); p32(p + 148, 2);
}
static void fixture(unsigned block, unsigned shift)
{
    unsigned master = SB / block + 2, root = master + 1;
    uint8_t *p = image + SB;
    memset(image, 0, sizeof(image));
    meta(p, 1, 100); p32(p + 24, 1802); p32(p + 28, 1900);
    p32(p + 36, block); p32(p + 40, shift);
    p64(p + 48, 1); p64(p + 56, master);
    p64(p + 80, 2); p64(p + 88, root);
    memcpy(p + 96, "lock_nolock", 12);
    inode(block, master, 1); inode(block, root, 2);
}
static void expect(const IfsFsCoreOpsV2 *core, const IfsFsHostRuntimeV2 *host,
                   struct context *c, int expected)
{
    uint32_t variant = 99;
    assert(core->probe(host, &variant) == expected);
    assert(variant == (expected == IFS_FS_OK ? 1802u : 0u));
    assert(!c->allocations);
}
/* A populated native byte fixture tests the namespace and allocation
 * invariants independently of the production reader's implementation.
 * It complements, rather than replaces, the independently formatted image. */
#define B 4096u
#define RG 17u
#define FIRST 18u
#define JOURNAL_FIRST 32u
#define JOURNAL_BLOCKS 2048u
#define DATA (JOURNAL_FIRST + JOURNAL_BLOCKS)
static uint8_t *blockptr(unsigned b) { return image + (size_t)b * B; }
static uint32_t hash(const uint8_t *p, unsigned n)
{
    uint32_t value = 0xffffffffu;
    while (n--) {
        value ^= *p++;
        for (unsigned i = 0; i < 8; ++i) {
            if (value & 1u) value = (value >> 1) ^ 0xedb88320u;
            else value >>= 1;
        }
    }
    return ~value;
}
static void p16(uint8_t *p, unsigned n)
{
    p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
}
static void file_inode(unsigned b, unsigned formal, uint64_t size,
                       unsigned height, unsigned blocks)
{
    inode(B, b, formal);
    uint8_t *p = blockptr(b);
    p32(p + 40, 0100644); p32(p + 52, 1);
    p32(p + 148, 0); p64(p + 56, size); p64(p + 64, blocks);
    p16(p + 138, height);
}
struct dir_item { const char *name; unsigned b, formal, type; };
static void directory(unsigned b, unsigned formal, const struct dir_item *items,
                      unsigned count)
{
    inode(B, b, formal);
    uint8_t *p = blockptr(b);
    p32(p + 132, 1200); p32(p + 148, count);
    unsigned off = 232;
    for (unsigned i = 0; i < count; ++i) {
        const struct dir_item *d = &items[i];
        unsigned n = (unsigned)strlen(d->name);
        unsigned record = i + 1 == count ? B - off : (40 + n + 7) & ~7u;
        assert(record >= 40 + n && off + record <= B);
        p64(p + off, d->formal); p64(p + off + 8, d->b);
        p32(p + off + 16, hash((const uint8_t *)d->name, n));
        p16(p + off + 20, record); p16(p + off + 22, n);
        p16(p + off + 24, d->type);
        memcpy(p + off + 40, d->name, n);
        off += record;
    }
    assert(off == B);
}
static void mark(unsigned b, unsigned state)
{
    unsigned relative = b - FIRST;
    assert(b >= FIRST && b < SIZE / B);
    uint8_t *byte = blockptr(RG) + 128 + relative / 4;
    unsigned shift = (relative % 4) * 2;
    assert(((*byte >> shift) & 3u) == 0);
    *byte |= (uint8_t)(state << shift);
}
static void log_header(unsigned n, unsigned flags)
{
    uint8_t *p = blockptr(JOURNAL_FIRST + n);
    memset(p, 0, B);
    meta(p, 8, 800);
    p64(p + 24, (uint64_t)n + 1);
    p32(p + 32, flags); p32(p + 40, n);
    p32(p + 44, hash(p, 48));
}
static void populated(void)
{
    fixture(B, 12);
    memset(image, 0, sizeof(image));
    uint8_t *sb = image + SB;
    meta(sb, 1, 100); p32(sb + 24, 1802); p32(sb + 28, 1900);
    p32(sb + 36, B); p32(sb + 40, 12);
    p64(sb + 48, 1); p64(sb + 56, 18);
    p64(sb + 80, 2); p64(sb + 88, 19);
    memcpy(sb + 96, "lock_nolock", 12);
    static const struct dir_item master[] = {
        {".",18,1,4},{"..",18,1,4},{"rindex",20,3,8},{"jindex",21,4,4}
    }, root[] = {
        {".",19,2,4},{"..",19,2,4},{"hello.txt",23,6,8},
        {"sub",24,7,4},{"frag.bin",26,9,8}
    }, journals[] = {
        {".",21,4,4},{"..",18,1,4},{"journal0",22,5,8}
    }, sub[] = {{".",24,7,4},{"..",19,2,4},{"child",25,8,8}};
    directory(18,1,master,4); directory(19,2,root,5);
    directory(21,4,journals,3); directory(24,7,sub,3);
    file_inode(20,3,96,0,1);
    unsigned count = ((SIZE / B - FIRST) / 4) * 4;
    uint8_t *ri = blockptr(20) + 232;
    p64(ri, RG); p32(ri + 8, 1); p64(ri + 16, FIRST);
    p32(ri + 24, count); p32(ri + 28, count / 4);
    file_inode(23,6,5,0,1); memcpy(blockptr(23)+232,"hello",5);
    file_inode(25,8,5,0,1); memcpy(blockptr(25)+232,"child",5);
    file_inode(26,9,B*3u+17,1,4);
    p64(blockptr(26)+232,DATA);
    p64(blockptr(26)+248,DATA+2);
    p64(blockptr(26)+256,DATA+1);
    memset(blockptr(DATA),0x41,B);
    memset(blockptr(DATA+1),0x43,B);
    memset(blockptr(DATA+2),0x42,B);
    file_inode(22,5,(uint64_t)JOURNAL_BLOCKS*B,2,JOURNAL_BLOCKS+6);
    for (unsigned i = 0; i < 5; ++i) {
        p64(blockptr(22)+232+i*8,27+i);
        meta(blockptr(27+i),5,500);
    }
    for (unsigned n = 0; n < JOURNAL_BLOCKS; ++n) {
        unsigned indirect = 27+n/509;
        p64(blockptr(indirect)+24+(n%509)*8,JOURNAL_FIRST+n);
        log_header(n,1);
    }
    uint8_t *rg = blockptr(RG);
    meta(rg,2,200);
    for (unsigned b = FIRST; b <= 26; ++b) mark(b,3);
    for (unsigned b = 27; b < DATA+3; ++b) mark(b,1);
    unsigned used = DATA+3-FIRST;
    p32(rg+28,count-used); p32(rg+32,9);
    p64(rg+48,FIRST); p32(rg+56,count); p32(rg+60,count/4);
    p32(rg+64,hash(rg,128));
}
static void mount_expect(const IfsFsCoreOpsV2 *core,
                         IfsFsHostRuntimeV2 *host, struct context *c,
                         int expected)
{
    void *volume = NULL;
    IfsFsVolumeInfoV2 info = {.structure_bytes=sizeof(info)};
    int rc = core->mount(host,&volume,&info);
    if (rc != expected) fprintf(stderr,"mount expected %d got %d\n",expected,rc);
    assert(rc == expected);
    if (!rc) assert(volume && core->unmount(volume)==0);
    else assert(!volume);
    assert(!c->allocations);
}
static void read_tests(const IfsFsCoreOpsV2 *core,
                       IfsFsHostRuntimeV2 *host, struct context *c)
{
    populated();
    void *volume = NULL;
    IfsFsVolumeInfoV2 info = {.structure_bytes=sizeof(info)};
    host->capabilities |= IFS_FS_HOST_EXCLUSIVE;
    assert(core->mount(host,&volume,&info)==0 && volume);
    unsigned count = ((SIZE / B - FIRST) / 4) * 4;
    assert(info.free_bytes == (uint64_t)(count - (DATA+3-FIRST))*B);
    char name[256];
    IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
    assert(core->stat(volume,"/hello.txt",&e)==0 && e.type==IFS_FS_ENTRY_FILE && e.size_bytes==5);
    assert(core->directory_entry(volume,"/",0,&e)==0 && !strcmp(name,"hello.txt"));
    assert(core->directory_entry(volume,"/",1,&e)==0 && !strcmp(name,"sub"));
    assert(core->directory_entry(volume,"/",2,&e)==0 && !strcmp(name,"frag.bin"));
    assert(core->directory_entry(volume,"/",3,&e)==IFS_FS_END);
    assert(core->stat(volume,"/sub/child",&e)==0 && e.size_bytes==5);
    assert(core->stat(volume,"/sub/../hello.txt",&e)==IFS_FS_INVALID);
    uint8_t out[2*B+17];
    assert(core->read(volume,"/hello.txt",0,out,5)==0 && !memcmp(out,"hello",5));
    assert(core->read(volume,"/sub/child",1,out,4)==0 && !memcmp(out,"hild",4));
    assert(core->read(volume,"/frag.bin",B-1,out,sizeof(out))==0);
    assert(out[0]==0x41);
    for (unsigned n=1;n<=B;++n) assert(out[n]==0);
    for (unsigned n=B+1;n<=2*B;++n) assert(out[n]==0x42);
    for (unsigned n=2*B+1;n<sizeof(out);++n) assert(out[n]==0x43);
    assert(core->read(volume,"/hello.txt",5,out,1)==IFS_FS_INVALID);
    p64(blockptr(26)+232,DATA+10);
    assert(core->read(volume,"/frag.bin",0,out,1)==IFS_FS_CORRUPT);
    p64(blockptr(26)+232,DATA);
    p16(blockptr(19)+232+20,0);
    assert(core->directory_entry(volume,"/",0,&e)==IFS_FS_CORRUPT);
    assert(core->unmount(volume)==0 && !c->allocations);
    populated(); log_header(JOURNAL_BLOCKS-1,0);
    mount_expect(core,host,c,IFS_FS_UNSUPPORTED);
    populated(); blockptr(JOURNAL_FIRST+JOURNAL_BLOCKS-1)[44]^=1;
    mount_expect(core,host,c,IFS_FS_CORRUPT);
    populated(); blockptr(RG)[28]^=1;
    mount_expect(core,host,c,IFS_FS_CORRUPT);
    populated(); memcpy(image+SB+96,"lock_dlm\0\0\0\0",12);
    mount_expect(core,host,c,IFS_FS_UNSUPPORTED);
    host->capabilities &= ~IFS_FS_HOST_EXCLUSIVE;
}
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    IfsFsModuleApiEntryV2 entry =
        (IfsFsModuleApiEntryV2)dlsym(lib, IFS_FS_COMMON_EXPORT_V2);
    assert(entry);
    const IfsFsModuleApiV2 *api = entry();
    assert(ifs_fs_api_v2_valid(api, "gfs2"));
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    assert(core && core->header.capabilities == (IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
           IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ));
    assert(core->mount && core->read && !core->write &&
           ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME) &&
           !ifs_fs_group_v2(api, IFS_FS_GROUP_DEFRAG));
    struct context c = {.fd = -1, .size = SIZE};
    IfsFsHostRuntimeV2 host = {
        .abi_version = 2, .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_READ_ONLY, .volume_bytes = SIZE,
        .maximum_transfer_bytes = 4096, .context = &c,
        .read_at = rd, .allocate = alloc, .release = release
    };
    if (argc == 3) {
        struct stat st;
        c.fd = open(argv[2], O_RDONLY);
        assert(c.fd >= 0 && fstat(c.fd, &st) == 0 && st.st_size > 0);
        c.size = host.volume_bytes = (uint64_t)st.st_size;
    }
    for (c.sector = 512; c.sector <= 4096; c.sector *= 8) {
        host.logical_sector_bytes = host.physical_sector_bytes = c.sector;
        if (argc == 3) {
            expect(core, &host, &c, IFS_FS_OK);
            void *volume = NULL;
            IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
            assert(core->mount(&host, &volume, &info) == IFS_FS_ACCESS && !volume);
            host.capabilities |= IFS_FS_HOST_EXCLUSIVE;
            int rc = core->mount(&host, &volume, &info);
            if (rc) fprintf(stderr, "mount failed: %d, sector=%u\n", rc, c.sector);
            assert(rc == 0 && volume && info.block_bytes == 4096u && info.free_bytes > 0);
            char name[256];
            IfsFsEntryV2 e = {.structure_bytes = sizeof(e), .name = name, .name_capacity = sizeof(name)};
            assert(core->stat(volume, "/", &e) == 0 && e.type == IFS_FS_ENTRY_DIRECTORY);
            assert(core->directory_entry(volume, "/", 0, &e) == IFS_FS_END);
            assert(core->stat(volume, "/nonexistent", &e) == IFS_FS_NOT_FOUND);
            assert(core->unmount(volume) == 0 && !c.allocations);
            host.capabilities &= ~IFS_FS_HOST_EXCLUSIVE;
            continue;
        }
        for (unsigned shift = 9; shift <= 12; ++shift) {
            unsigned block = 1u << shift;
            unsigned root = SB / block + 3;
            fixture(block, shift);
            expect(core, &host, &c, IFS_FS_OK);
            c.fail = 1; expect(core, &host, &c, IFS_FS_IO_ERROR); c.fail = 0;
            p32(image + SB, 0);
            expect(core, &host, &c, IFS_FS_NOT_FOUND);
            fixture(block, shift); p32(image + SB + 4, 4);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p32(image + SB + 24, 1800);
            expect(core, &host, &c, IFS_FS_UNSUPPORTED);
            fixture(block, shift); p32(image + SB + 40, 31);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p64(image + SB + 88, UINT64_MAX);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p64(image + SB + 88, SB / block + 2);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p64(image + root * block + 24, 999);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p32(image + root * block + 40, 0100644);
            expect(core, &host, &c, IFS_FS_CORRUPT);
            fixture(block, shift); p64(image + root * block + 56, block);
            expect(core, &host, &c, IFS_FS_CORRUPT);
        }
        read_tests(core, &host, &c);
    }
    if (c.fd >= 0) assert(close(c.fd) == 0);
    assert(dlclose(lib) == 0);
    puts("GFS2 bounded offline Common V2 reader PASS (no mutation claim)");
    return 0;
}
