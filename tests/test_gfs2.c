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

#define SIZE (256u * 1024u)
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
    assert(core && core->header.capabilities == IFS_FS_CORE_PROBE);
    assert(!core->mount && !core->read && !core->write &&
           !ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME) &&
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
    }
    if (c.fd >= 0) assert(close(c.fd) == 0);
    assert(dlclose(lib) == 0);
    puts("GFS2 bounded Common V2 recognition PASS (no mount or write claim)");
    return 0;
}
