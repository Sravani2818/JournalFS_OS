/*
 * checksum.c -- CRC-32C implementation (table driven, bit-reflected).
 *
 * Polynomial 0x1EDC6F41 (Castagnoli), reversed form 0x82F63B78.  CRC-32C is
 * used because it is cheap in software and detects the burst errors a
 * torn/partial block write typically produces.
 */
#include "checksum.h"

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void crc_build_table(void)
{
    uint32_t i, j, crc;

    for (i = 0; i < 256; i++) {
        crc = i;
        for (j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0x82F63B78u & (~((crc & 1u) - 1u)));
        crc_table[i] = crc;
    }
    crc_table_ready = 1;
}

uint32_t jfs_crc32c_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    if (!crc_table_ready)
        crc_build_table();

    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint64_t jfs_hash64(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ull;    /* FNV offset basis */

    while (len--) {
        h ^= (uint64_t)(*p++);
        h *= 1099511628211ull;              /* FNV prime */
    }
    return h;
}

uint32_t jfs_crc32c(const void *data, size_t len)
{
    return jfs_crc32c_update(0u, data, len);
}

uint32_t jfs_crc_struct(const void *data, size_t len, size_t crc_off)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc;

    if (crc_off + sizeof(uint32_t) > len)
        return jfs_crc32c(data, len);

    crc = jfs_crc32c_update(0u, p, crc_off);
    crc = jfs_crc32c_update(crc, p + crc_off + sizeof(uint32_t),
                            len - crc_off - sizeof(uint32_t));
    return crc;
}
