#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <memory.h>
#include <unistd.h> //pread pwrite
#include <linux/types.h>


#include "config.h"
#include "libzone.h"

#include "zbd-cache.h"
#include "../util/timerUtils.h"
#include "../util/hashtable.h"
#include "../util/log.h"

#include "bits.h"
#include "bitmap.h"

#include "strategy/algorithms-general.h"

struct hash_table; 
struct zbc_device;



/* cache space runtime info. */
struct cache_algorithm {
    int (*init)();
    int (*hit)(struct cache_page *page, int op);
    int (*login)(struct cache_page *page, int op);
    int (*logout)(struct cache_page *page, int op); 
    
    int (*GC_privillege)();
};


/* Global objects */
struct zbc_device *zbd;
int cache_dev; 

struct hash_table *hashtb_cblk;  // hash index for cached block
struct cache_algorithm algorithm;
struct cache_runtime cache_rt = {
    .n_page_used = 0,
    .pages = NULL,
    .header_free_page = NULL
};

struct zbd_zone *zones_collection;

static void* buf_rmw; // private buffer used to RMW. Note that, this only for single thread version. 


/* If Defined R/W Cache Space Static Allocated */

static int init_cache_pages();
static int init_StatisticObj();

// Utilities of cache pages
static inline struct cache_page * pop_freebuf();
static inline void push_freebuf(struct cache_page *page);

static inline int init_page(struct cache_page *page, uint64_t blkoff, int op); 
static inline int try_recycle_page(struct cache_page *page, int op);

static inline struct cache_page * alloc_page_for(uint64_t blkoff, int op); // Entry Point


// Utilitis of cache device. 
static inline int pread_cache(void *buf, int64_t blkoff, uint64_t blkcnt);
static inline int pwrite_cache(void *buf, int64_t blkoff, uint64_t blkcnt);

/* STT */
struct RuntimeSTAT STT = 
{
    .traceId = 0,
    .workload_mode = 0x02, //0x01 | 0x02,
    .start_Blkoff = N_ZONEBLK * (N_ZONES - N_SEQ_ZONES),
    .n_cache_pages = 8000000, //default 32GB
    .op_algorithm = ALG_CARS,
    .isPart = 1,

    /* 1. Workload */
    .reqcnt_s = 0,
    .reqcnt_r = 0,
    .reqcnt_w = 0,

    .hitnum_s = 0,
    .hitnum_r = 0,
    .hitnum_w = 0,

    .missnum_s = 0,
    .missnum_r = 0,
    .missnum_w = 0,

    .time_req_s = 0,
    .time_req_r = 0,
    .time_req_w = 0,

    /* 2. Cache Device */
    .cpages_s = 0,
    .cpages_w = 0, 
    .cpages_r = 0,

    .gc_cpages_s = 0,
    .gc_cpages_w = 0,
    .gc_cpages_r = 0,

    .time_cache_s = 0,
    .time_cache_r = 0,
    .time_cache_w = 0,

    /* 3. ZBD */
    .rmw_scope = 0,
    .rmw_times = 0,

    .time_zbd_read = 0,
    .time_zbd_rmw = 0,

    .debug = NULL,
};


/* stopwatch */
static struct timeval tv_start, tv_stop;
// static timeval tv_bastart, tv_bastop;
// static timeval tv_cmstart, tv_cmstop;
microsecond_t msec_r_hdd, msec_w_hdd, msec_r_ssd, msec_w_ssd, msec_bw_hdd = 0;


/*
 * init buffer hash table, strategy_control, buffer, work_mem
 */
void CacheLayer_Init()
{
    int r_init_cachepages = init_cache_pages();
    int r_init_hashtb = HashTab_crt(STT.n_cache_pages, &hashtb_cblk); 

    switch (STT.op_algorithm)
    {
        case ALG_CARS:
            algorithm.init = cars_init;
            algorithm.login = cars_login;
            algorithm.logout = cars_logout;
            algorithm.hit = cars_hit;
            algorithm.GC_privillege = cars_writeback_privi;
            break;
        case ALG_MOST:
            algorithm.init = most_init;
            algorithm.login = most_login;
            algorithm.logout = most_logout;
            algorithm.hit = most_hit;
            algorithm.GC_privillege = most_writeback_privi;
            break;
        case ALG_UNKNOWN:
            log_err_sac("[error]func:%s, unknown algorithm. \n");
            exit(-1);
        default:
            break;
    }


    int r_init_algorithm = algorithm.init();

    printf("r_init_cachepages: %d, r_init_hashtb: %d, r_init_algorithm: %d\n",
           r_init_cachepages, r_init_hashtb, r_init_algorithm);

    if (r_init_cachepages == -1 || r_init_hashtb == -1 || r_init_algorithm == -1)
        exit(EXIT_FAILURE);

}

static int
init_cache_pages()
{
    /* init cache pages metadata */
    cache_rt.pages = 
    cache_rt.header_free_page = (struct cache_page *)malloc(sizeof(struct cache_page) * STT.n_cache_pages);
    
    if(cache_rt.pages == NULL)
        return -1;
    
    struct cache_page *page = cache_rt.pages;
    for (unsigned long i = 0; i < STT.n_cache_pages; page++, i++)
    {
        page->pos = i;
        page->status = 0;
        page->next_page = page + 1;
    }
    page --;
    page->next_page = NULL; // the last one

    /* init cached zone metadata */ 
    zones_collection = (struct zbd_zone *)calloc(N_ZONES, sizeof(struct zbd_zone));
    if(!zones_collection)
        return -1;

    struct zbd_zone* zone = zones_collection; 
    
    for(uint64_t i = 0; i < N_ZONES; i++, zone++){
        zone->zoneId = i;
        zone->wtpr = N_ZONEBLK - 1;

        zone->bitmap = NULL;
        zone->priv = NULL;
    }

    /* init buffer for RMW */
    buf_rmw = malloc(ZONESIZE);
    if(!buf_rmw)
        return -1;
    return 0;
}

static struct cache_page * 
retrive_cache_page(uint64_t tg_blk, int op)
{
    /* Lookup if already cached. */
    struct cache_page *page; 
    uint64_t tg_page;

    /* Cache miss */
    if(HashTab_Lookup(hashtb_cblk, tg_blk, &tg_page) < 0) {return NULL;}

    /* Cache hit */
    page = cache_rt.pages + tg_page;

    /* STT */
    if ((page->status & op) == 0){
        op == FOR_READ ? STT.cpages_r ++ : STT.cpages_w ++;
    }
    /* metadata */
    page->status |= op;
    
    /* algorithm */
    algorithm.hit(page, op);
    return page;
}

static int 
flush_zone_pages()
{
    // call algorithm to decide which zone to be flush
    int ret = algorithm.GC_privillege();

    return ret;
}


static inline struct cache_page *
alloc_page_for(uint64_t blkoff, int op)
{
    struct cache_page *page = NULL;
    while(1)
    {
        // alloc free cache page
        page = pop_freebuf();
        if(page) {break;}

        flush_zone_pages();
    }

/* metadata */ 
    init_page(page, blkoff, op);

/* algorithm */    
    algorithm.login(page, op);

/* STT */
    STT.cpages_s ++ ;
    op == FOR_READ ? STT.cpages_r ++ : STT.cpages_w ++;

    return page; 
}


int read_block(uint64_t blkoff, void *buf)
{
    uint32_t zoneId = blkoff / N_ZONEBLK;
    if(zoneId > N_SEQ_ZONES){
        log_err_sac("func %s: block LBA overflow. \n", __func__);
        return -1;
    }

    int ret;
    int op = FOR_READ;
    // Lap(&tv_cmstart);
    static struct cache_page *page;

    page = retrive_cache_page(blkoff, op);
    if(page) // cache hit
    {
        STT.hitnum_r ++, STT.hitnum_s ++;
        ret = pread_cache(buf, page->pos, 1);
        return ret;
    }
    
/* cache miss */ 
    STT.missnum_r ++, STT.missnum_s ++;
    page = alloc_page_for(blkoff, op);
    if(!page)
        return -1;

/* handle data */
    //read from zbd
    ret = zbd_read_zblk(zbd, buf, page->belong_zoneId, page->blkoff_inzone, 1);
    if(ret < 0) {log_err_sac("read from zbd error. \n");}

    // write to cache
    ret = pwrite_cache(buf, page->pos, 1);
    if(ret < 0) {log_err_sac("write cache error. \n");}

    return ret;


        // msec_r_hdd = TimerInterval_MICRO(&tv_start, &tv_stop);
        // STT.time_read_hdd += Mirco2Sec(msec_r_hdd);
        // STT.load_hdd_blocks++;

        // dev_pwrite(cache_dev, ssd_buffer, BLKSZ, page->pos * BLKSZ);
        // msec_w_ssd = TimerInterval_MICRO(&tv_start, &tv_stop);
        // STT.time_write_ssd += Mirco2Sec(msec_w_ssd);
        // STT.flush_ssd_blocks++;



        // dev_pread(cache_fd, ssd_buffer, BLKSZ, page->pos * BLKSZ);
        // msec_r_ssd = TimerInterval_MICRO(&tv_start, &tv_stop);

        // STT.hitnum_r++;
        // STT.time_read_ssd += Mirco2Sec(msec_r_ssd);
        // STT.load_ssd_blocks++;
    }




int write_block(uint64_t blkoff, void *buf)
{
    uint32_t zoneId = blkoff / N_ZONEBLK;
    if(zoneId > N_SEQ_ZONES){
        log_err_sac("func %s: block LBA overflow. \n", __func__);
        return -1;
    }

    int ret;
    int op = FOR_WRITE;

    // Lap(&tv_cmstart);
    static struct cache_page *page;

    page = retrive_cache_page(blkoff, op);
    if(page) // cache hit
    {
        STT.hitnum_w ++, STT.hitnum_s ++;
        ret = pwrite_cache(buf, page->pos, 1);
        return ret;
    }
    
/* cache miss */ 
    STT.missnum_w ++, STT.missnum_s ++;
    page = alloc_page_for(blkoff, op);
    if(!page)
        return -1;

/* handle data */
    // write to cache
    ret = pwrite_cache(buf, page->pos, 1);
    if(ret < 0) {log_err_sac("write cache error. \n");}

    return ret;
}

/******************
**** Cache Pages Utilities *****
*******************/
static inline struct cache_page *
pop_freebuf()
{
    if (!cache_rt.header_free_page)
        return NULL;
    struct cache_page *page = cache_rt.header_free_page;
    cache_rt.header_free_page = page->next_page;
    page->next_page = NULL;
    cache_rt.n_page_used ++;
    return page;
}

static inline void
push_freebuf(struct cache_page *page)
{
    page->next_page = cache_rt.header_free_page;
    cache_rt.header_free_page = page;

    cache_rt.n_page_used --;
}


static inline int init_page(struct cache_page *page, uint64_t blkoff, int op)
{
    /* page metadata */
    page->tg_blk = blkoff;
    page->belong_zoneId = blkoff / N_ZONEBLK;
    page->blkoff_inzone = blkoff % N_ZONEBLK;
    page->status = op;

    /* zone metadata */
    struct zbd_zone *zone = zones_collection + page->belong_zoneId;
    if(zone->cblks == 0){
        size_t nwords = create_Bitmap(&zone->bitmap, N_ZONEBLK);

        if(nwords <= 0) { 
            log_err_sac("func: %s error\n", __func__); 
        }
    }

    zone->cblks ++;
    set_Bit(zone->bitmap, page->blkoff_inzone);

    /* hashtable */
    int ret = HashTab_Insert(hashtb_cblk, blkoff, page->pos);
    if(ret < 0)
    {
        log_err_sac("[error]func:%s, Hashtable insert error.\n", __func__);
        exit(-1);
    }
    return 0;
}


static inline int try_recycle_page(struct cache_page *page, int op) 
{ 

    int status = page->status;
    if(!status) {
        log_err_sac("[error] func:%s, can't double recycle page. \n", __func__);
        return -1;
    } 

    page->status &=  (~op);
    if(page->status) {   // still for read or write
        return 0;
    } 

    /* STT */
    if(status & FOR_READ){
        STT.cpages_r --;
        STT.gc_cpages_r ++ ;
    } else if (status & FOR_WRITE) {
        STT.cpages_w --;
        STT.gc_cpages_w ++ ;
    }
    STT.cpages_s --;
    STT.gc_cpages_s ++;


    /* zone metadata */
    struct zbd_zone *zone = zones_collection + page->belong_zoneId;
    zone->cblks --;
    if(zone->cblks == 0){
        free_Bitmap(zone->bitmap);
        zone->bitmap = NULL;
    } else {
        clean_Bit(zone->bitmap, page->blkoff_inzone);
    }

    // hashtable
    HashTab_Delete(hashtb_cblk, page->tg_blk);

    //recycle page
    push_freebuf(page);
    return 0;
}

/******************
**** Cache Dev Utilities *****
*******************/

static inline int pread_cache(void *buf, int64_t blkoff, uint64_t blkcnt)
{
    #ifdef NO_REAL_DISK_IO
        return blkcnt;
    #endif

    uint64_t offset = blkoff * BLKSIZE, 
             nbytes = blkcnt * BLKSIZE;

    
    Lap(&tv_start);
    int ret = pread(cache_dev, buf, nbytes, offset);
    Lap(&tv_stop);

    return ret;
}

static inline int pwrite_cache(void *buf, int64_t blkoff, uint64_t blkcnt)
{
    #ifdef NO_REAL_DISK_IO
        return blkcnt;
    #endif

    uint64_t offset = blkoff * BLKSIZE, 
             nbytes = blkcnt * BLKSIZE;

    Lap(&tv_start);
    int ret = pwrite(cache_dev, buf, nbytes, offset);
    Lap(&tv_stop);

    return ret;
}



/******************
**** RMW Utilities *****
*******************/

static inline int zbd_partread_by_bitmap(uint32_t zoneId, void * zonebuf, uint64_t from, uint64_t to, zBitmap *bitmap)
{
    int ret = 0, cnt = 0;
    void *buf;
    uint64_t zblkoff;

    uint64_t start_word = BIT_WORD(from), 
               end_word = BIT_WORD(to);
    uint64_t start_word_offset = BIT_WORD_OFFSET(from), 
               end_word_offset = BIT_WORD_OFFSET(to);

    uint64_t this_word = start_word, 
             pos_from = start_word_offset, 
             pos_to = BITS_PER_LONG - 1;

    while(this_word <= end_word){

        if(this_word == end_word){ // end
            pos_to = end_word_offset;
        }

        if(check_Bitword_hasZero(bitmap + this_word, pos_from, pos_to)){ 
            // read clean blocks from zbd by 256KB at once.]
            zblkoff = (this_word * BITS_PER_LONG) + pos_from;
            buf = zonebuf + (zblkoff * BLKSIZE);

            cnt = zbd_read_zblk(zbd, buf, zoneId, zblkoff, pos_to - pos_from + 1);
            if(cnt < 0)
                return cnt;

            ret ++;
        }

        this_word ++;
        pos_from = 0;
    }

    return ret;
}

static inline int cache_partread_by_bitmap(uint32_t zoneId, void * zonebuf, uint64_t from, uint64_t to, zBitmap *bitmap){
    int ret;
    uint64_t tg_zone = zoneId * N_ZONEBLK;
    struct zbd_zone * tg_zone_meta = zones_collection + zoneId;

    uint64_t tg_blk;
    uint64_t tg_page;

    uint32_t start_word = BIT_WORD(from), 
               end_word = BIT_WORD(to);
    uint32_t start_word_offset = BIT_WORD_OFFSET(from), 
               end_word_offset = BIT_WORD_OFFSET(to);

    uint32_t this_word = start_word, 
             pos_from = start_word_offset, 
             pos_to = BITS_PER_LONG - 1;
    
    uint32_t pos = from;


    while(this_word <= end_word){

        if(this_word == end_word){ // end word
            pos_to = end_word_offset;
        }

        zBitmap * word = bitmap + this_word;
        /* check every bit in word. */
        struct cache_page * page; 
        for(uint32_t i = pos_from; i <= pos_to; i++, pos ++)
        {
            if((*word & (1UL << i)) == 0) {continue;} // block not cached

            // target is in cache. and load it from cache layer. 
            tg_blk = tg_zone + pos;
            ret = HashTab_Lookup(hashtb_cblk, tg_blk, &tg_page);
            if(ret < 0){
                log_err_sac("inconsistent bitmap with hashtable.\n");
            }

            uint32_t bufoff = pos * BLKSIZE;
            ret = pread_cache(zonebuf + bufoff, tg_page, 1);
            if(ret <= 0){
                log_err_sac("load block from cache failed, page[%d].\n", tg_page);
            }

            /* clean page metadata: bit, hashtable, page status, STT. (just for demo) */
            page = cache_rt.pages + tg_page;
            algorithm.logout(page, FOR_WRITE);
            try_recycle_page(page, FOR_WRITE); 
            if(tg_zone_meta->cblks == 0)
                return 0;
        }
        this_word ++;
        pos_from = 0;
    }
    return 0;

}

int RMW(uint32_t zoneId, uint64_t from_blk, uint64_t to_blk)  // Algorithms (e.g CARS) can use it. 
{
    int ret;
    struct zbd_zone *tg_zone = zones_collection + zoneId;

    /* Read blocks from ZBD refered to Bitmap*/
    ret = zbd_partread_by_bitmap(zoneId, buf_rmw, from_blk, to_blk, tg_zone->bitmap);
    if(ret < 0)
        return ret;
    
    /* Modify */
    // load dirty pages from cache device
    cache_partread_by_bitmap(zoneId, buf_rmw, from_blk, to_blk, tg_zone->bitmap);


    /* Set target zone write pointer */
    ret = zbd_set_wp(zbd, zoneId, from_blk);
    if(ret < 0)
        return ret;
 
    /* Write-Back */
    ret = zbd_write_zone(zbd, buf_rmw, 0, zoneId, from_blk, to_blk - from_blk + 1);

    if(ret < 0)
        return ret;

    
    STT.rmw_times ++;
    STT.time_zbd_rmw ++;
    STT.rmw_scope += ret;

    return ret;
}

int Page_force_drop(struct cache_page *page)
{
    int status = page->status;
    algorithm.logout(page, status);
    try_recycle_page(page, status); //GC hashtable, bit, STT
}
