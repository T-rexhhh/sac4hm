#ifndef LIBZBC_EXT_H
#define LIBZBC_EXT_H
#include <stdio.h>
#include <stdint.h>

#include <linux/types.h>

#define MULTIPLE_4K(x) !(x & 0x0fff)
#define SECSIZE 512
#define BLKSIZE 4096
#define N_BLKSEC 8
#define ZONESIZE 268435456
#define N_ZONESEC 524288
#define N_ZONEBLK 65536

#define N_ZONES 100
#define N_SEQ_ZONES 90

struct zbd_metadata
{
    uint32_t nr_zones_all, nr_zones_conv, nr_zones_seq;
};

struct zbc_device;
struct zbc_zone;

extern int zbd_open(const char *filename, int flags, struct zbc_device **dev);

extern ssize_t zbd_pwrite(struct zbc_device *dev, const void *buf,
			  size_t count, uint64_t offset);

extern ssize_t zbd_read_zblk(struct zbc_device *dev, void *buf, 
                     uint32_t zoneId, uint64_t inzone_blkoff, size_t blkcnt);
              
extern ssize_t zbd_write_zone(struct zbc_device *dev, const void *wbuf, bool force, 
            uint32_t zoneId, uint64_t inzone_blkoff, size_t blkcnt);

extern int zbd_set_wp(struct zbc_device *dev, uint32_t zoneId, uint64_t inzone_blkoff);


#ifdef _TEST_
#include "bitmap.h"
#endif


#endif