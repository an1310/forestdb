/*
 * Copyright 2013 Jung-Sang Ahn <jungsang.ahn@gmail.com>.
 * All Rights Reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hash_functions.h"
#include "common.h"
#include "hash.h"
#include "list.h"
#include "rbwrap.h"
#include "blockcache.h"
#include "crc32.h"

#include "memleak.h"

#ifdef __DEBUG
#ifndef __DEBUG_BCACHE
    #undef DBG
    #undef DBGCMD
    #undef DBGSW
    #define DBG(args...)
    #define DBGCMD(command...)
    #define DBGSW(n, command...) 
#endif
static uint64_t _dirty = 0;
#endif

// global lock
static spin_t bcache_lock;

// hash table for filename
static struct hash fnamedic;

// free block list
static size_t freelist_count=0;
static struct list freelist;
static spin_t freelist_lock;

// file structure list
static struct list file_lru, file_empty;
static spin_t filelist_lock;

//static struct list cleanlist, dirtylist;
//static uint64_t nfree, nclean, ndirty;
static uint64_t bcache_nblock;

static int bcache_blocksize;
static size_t bcache_flush_unit;
static size_t bcache_sys_pagesize;

struct fnamedic_item {
    char *filename;
    uint16_t filename_len;
    uint32_t hash;

    // current opened filemgr instance (can be changed on-the-fly when file is closed and re-opened)
    struct filemgr *curfile;

    // list for clean blocks
    struct list cleanlist;
    // red-black tree for dirty blocks
    struct rb_root rbtree;
    // hash table for block lookup
    struct hash hashtable;

    // list elem for FILE_LRU
    struct list_elem le;    // offset -96
    // current list poitner (FILE_LRU or FILE_EMPTY)
    struct list *curlist;
    // hash elem for FNAMEDIC
    struct hash_elem hash_elem;

    spin_t lock;
};

#define BCACHE_DIRTY 0x1

struct bcache_item {
    // BID
    bid_t bid;
    // contents address
    void *addr;
    struct fnamedic_item *fname;
    // pointer of the list to which this item belongs
    //struct list *list;
    // hash elem for lookup hash table
    struct hash_elem hash_elem;
    // list elem for {free, clean, dirty} lists
    struct list_elem list_elem;     // offset -48
    // flag
    uint8_t flag;
    // spin lock
    spin_t lock;
};

struct dirty_item {
    struct bcache_item *item;
    struct rb_node rb;
};

#ifdef __DEBUG
uint64_t _gn(struct list *list)
{
    uint64_t c = 0;
    struct list_elem *e;
    e = list_begin(list);
    while(e) {
        c++;
        e = list_next(e);
    }
    return c;
}
void _pl(struct list *list, uint64_t begin, uint64_t n)
{
    uint64_t c = 0;
    struct list_elem *e;
    struct bcache_item *item;
    char fname_buf[256];
    uint8_t marker;
    
    e = list_begin(list);
    while(e) {
        if (begin <= c && c < begin+n) {
            item = _get_entry(e, struct bcache_item, list_elem);
            memcpy(fname_buf, item->fname->filename, item->fname->filename_len);
            fname_buf[item->fname->filename_len] = 0;
            memcpy(&marker, item->addr + 4095, 1);
            printf("#%"_F64": BID %"_F64", marker 0x%x, file %s\n", c, item->bid, marker, fname_buf);
        }
        c++;
        e = list_next(e);
    }
}
#endif

INLINE int _dirty_cmp(struct rb_node *a, struct rb_node *b, void *aux)
{
    struct dirty_item *aa, *bb;
    aa = _get_entry(a, struct dirty_item, rb);
    bb = _get_entry(b, struct dirty_item, rb);
    
    #ifdef __BIT_CMP
        return _CMP_U64(aa->item->bid , bb->item->bid);
    
    #else
        if (aa->item->bid < bb->item->bid) return -1;
        else if (aa->item->bid > bb->item->bid) return 1;
        else return 0;
    
    #endif
}

INLINE uint32_t _fname_hash(struct hash *hash, struct hash_elem *e)
{
    struct fnamedic_item *item = _get_entry(e, struct fnamedic_item, hash_elem);
    return item->hash & ((unsigned)(BCACHE_NDICBUCKET-1));
}

INLINE int _fname_cmp(struct hash_elem *a, struct hash_elem *b) 
{
    size_t len;
    struct fnamedic_item *aa, *bb;
    aa = _get_entry(a, struct fnamedic_item, hash_elem);
    bb = _get_entry(b, struct fnamedic_item, hash_elem);

    if (aa->filename_len == bb->filename_len) {
        return memcmp(aa->filename, bb->filename, aa->filename_len);
    }else {
        len = MIN(aa->filename_len , bb->filename_len);
        int cmp = memcmp(aa->filename, bb->filename, len);
        if (cmp != 0) return cmp;
        else {
            return (int)((int)aa->filename_len - (int)bb->filename_len);
        }
    }
}

INLINE uint32_t _bcache_hash(struct hash *hash, struct hash_elem *e)
{
    struct bcache_item *item = _get_entry(e, struct bcache_item, hash_elem);
    return (item->bid) & ((uint32_t)BCACHE_NBUCKET-1);
}

INLINE int _bcache_cmp(struct hash_elem *a, struct hash_elem *b)
{
    struct bcache_item *aa, *bb;
    aa = _get_entry(a, struct bcache_item, hash_elem);
    bb = _get_entry(b, struct bcache_item, hash_elem);

    #ifdef __BIT_CMP

        return _CMP_U64(aa->bid, bb->bid);

    #else

        if (aa->bid == bb->bid) return 0;
        else if (aa->bid < bb->bid) return -1;
        else return 1;
        
    #endif
}

INLINE void _file_to_fname_query(struct filemgr *file, struct fnamedic_item *fname)
{
    fname->filename = file->filename;
    fname->filename_len = file->filename_len;
    fname->hash = crc32_8_last8(fname->filename, fname->filename_len, 0);
}

void _bcache_move_fname_list(struct fnamedic_item *fname, struct list *list)
{
    spin_lock(&filelist_lock);
    if (fname->curlist) list_remove(fname->curlist, &fname->le);
    if (list) list_push_front(list, &fname->le);
    fname->curlist = list;
    spin_unlock(&filelist_lock);
}

struct fnamedic_item *_bcache_get_victim()
{
    struct list_elem *e = NULL;

    spin_lock(&filelist_lock);
    e = list_end(&file_lru);
    if (e==NULL) {
        e = list_begin(&file_empty);
        if (e) {
            struct fnamedic_item *item = _get_entry(e, struct fnamedic_item, le);
            if (item->cleanlist.head == NULL && item->rbtree.rb_node == NULL) {
                e = NULL;
            }
        }
    }
    spin_unlock(&filelist_lock);

    // TODO: what if the victim file is removed by another thread?
    if (e) {
        return _get_entry(e, struct fnamedic_item, le);
    }
    return NULL;
}

struct bcache_item *_bcache_alloc_freeblock()
{
    struct list_elem *e = NULL;    
    struct bcache_item *item;

    spin_lock(&freelist_lock);
    e = list_pop_front(&freelist);
    if (e) freelist_count--;
    spin_unlock(&freelist_lock);
    
    if (e) {
        return _get_entry(e, struct bcache_item, list_elem);
    }
    return NULL;
}

void _bcache_release_freeblock(struct bcache_item *item)
{
    spin_lock(&freelist_lock);
    list_push_front(&freelist, &item->list_elem);
    freelist_count++;
    spin_unlock(&freelist_lock);
}

// flush a bunch of dirty blocks (BCACHE_FLUSH_UNIT) & make then as clean
//2 FNAME_LOCK is already acquired by caller (of the caller)
void _bcache_evict_dirty(struct fnamedic_item *fname_item, int sync)
{
    // get oldest dirty block
    void *buf = NULL;
    struct list_elem *e;
    struct rb_node *r;
    struct dirty_item *ditem;
    int count, ret;
    bid_t start_bid, prev_bid;
    void *ptr = NULL;
    uint8_t marker = 0x0;

    // scan and gather rb-tree items sequentially
    if (sync) {
        ret = posix_memalign(&buf, FDB_SECTOR_SIZE, bcache_flush_unit); assert(ret == 0);
        //buf = memalign(FDB_SECTOR_SIZE, bcache_flush_unit); assert(buf);
    }

    prev_bid = start_bid = BLK_NOT_FOUND;
    count = 0;

    // traverse rb-tree in a sequential order
    r = rb_first(&fname_item->rbtree);
    while(r) {
        ditem = _get_entry(r, struct dirty_item, rb);

        // if BID of next dirty block is not consecutive .. stop
        if (ditem->item->bid != prev_bid + 1 && prev_bid != BLK_NOT_FOUND && sync) break;
        // set START_BID if this is the first loop
        if (start_bid == BLK_NOT_FOUND) start_bid = ditem->item->bid;

        // set PREV_BID and go to next block
        prev_bid = ditem->item->bid;
        r = rb_next(r);

        spin_lock(&ditem->item->lock);
        // set PTR and get block MARKER
        ptr = ditem->item->addr;
        marker = *((uint8_t*)(ptr) + bcache_blocksize-1);

        ditem->item->flag &= ~(BCACHE_DIRTY);
        if (sync) {
            // copy to buffer
            #ifdef __CRC32
                if (marker == BLK_MARKER_BNODE ) {
                    // b-tree node .. calculate crc32 and put it into 8th byte of the block
                    memset(ptr + 8, 0xff, sizeof(void *));
                    uint32_t crc = crc32_8(ptr, bcache_blocksize, 0);
                    memcpy(ptr + 8, &crc, sizeof(crc));
                }
            #endif
            memcpy(buf + count*bcache_blocksize, ditem->item->addr, bcache_blocksize);
        }
        spin_unlock(&ditem->item->lock);

        // remove from rb-tree
        rb_erase(&ditem->rb, &fname_item->rbtree);
        // move to clean list
        list_push_front(&fname_item->cleanlist, &ditem->item->list_elem);

        mempool_free(ditem);
        
        // if we have to sync the dirty block, and
        // the size of dirty blocks exceeds the BCACHE_FLUSH_UNIT
        count++;
        if (count*bcache_blocksize >= bcache_flush_unit && sync) break;        
    }

    // synchronize
    if (sync && count>0) {
        // TODO: we MUST NOT directly call file->ops
        ret = fname_item->curfile->ops->pwrite(
            fname_item->curfile->fd, buf, count * bcache_blocksize, start_bid * bcache_blocksize);    

        assert(ret == count * bcache_blocksize);
        free(buf);
    }
}

// perform eviction
struct list_elem * _bcache_evict(struct fnamedic_item *curfile)
{
    struct list_elem *e = NULL;
    struct bcache_item *item;
    struct hash_elem *h;
    struct fnamedic_item query, *victim = NULL;

    spin_lock(&bcache_lock);
    
    while(victim == NULL) {
        // select victim file (the tail of FILE_LRU)
        victim = _bcache_get_victim();
        while(victim) {
            spin_lock(&victim->lock);
            
            // check whether this file has at least one block to be evictied
            if (list_begin(&victim->cleanlist) != NULL || victim->rbtree.rb_node != NULL) {
                // select this file as victim
                break;
            }else{
                // empty file
                // move this file to empty list (it is ok that this was already moved to empty list by other thread)
                _bcache_move_fname_list(victim, &file_empty);
                spin_unlock(&victim->lock);

                victim = NULL;
            }
        }
    }
    assert(victim);
    spin_unlock(&bcache_lock);

    // select victim clean block of the victim file
    e = list_pop_back(&victim->cleanlist);

    while (e == NULL) {
        // when the victim file has no clean block .. evict dirty block
        _bcache_evict_dirty(victim, 1);

        // pop back from cleanlist
        e = list_pop_back(&victim->cleanlist);
    }

    item = _get_entry(e, struct bcache_item, list_elem);

    // remove from hash and insert into freelist
    hash_remove(&victim->hashtable, &item->hash_elem);

    // add to freelist
    _bcache_release_freeblock(item);

    // check whether the victim file has no cached block
    if (list_begin(&victim->cleanlist) == NULL && victim->rbtree.rb_node == NULL) {
        // remove from FILE_LRU and insert into FILE_EMPTY
        _bcache_move_fname_list(victim, &file_empty);
    }
    
    spin_unlock(&victim->lock);

    return &item->list_elem;
}

struct fnamedic_item * _fname_create(struct filemgr *file) {
    // TODO: we MUST NOT directly read file sturcture

    struct fnamedic_item *fname_new;
    fname_new = (struct fnamedic_item *)malloc(sizeof(struct fnamedic_item));

    fname_new->filename_len = strlen(file->filename);
    fname_new->filename = (char *)malloc(fname_new->filename_len + 1);
    memcpy(fname_new->filename, file->filename, fname_new->filename_len);
    //strcpy(fname_new->filename, file->filename);
    fname_new->filename[fname_new->filename_len] = 0;

    // calculate hash value
    fname_new->hash = crc32_8_last8(fname_new->filename, fname_new->filename_len, 0);
    fname_new->lock = SPIN_INITIALIZER;
    fname_new->curlist = NULL;
    fname_new->curfile = file;
    file->bcache = fname_new;

    // initialize rb-tree
    rbwrap_init(&fname_new->rbtree, NULL);
    // initialize clean list
    list_init(&fname_new->cleanlist);
    // initialize hash table
    hash_init(&fname_new->hashtable, BCACHE_NBUCKET, _bcache_hash, _bcache_cmp);

    // insert into fname dictionary
    spin_lock(&bcache_lock);
    hash_insert(&fnamedic, &fname_new->hash_elem);
    spin_unlock(&bcache_lock);

    return fname_new;    
}

void _fname_free(struct fnamedic_item *fname)
{
    struct rb_node *r;
    struct dirty_item *item;
    
    // remove from corresponding list
    _bcache_move_fname_list(fname, NULL);
    
    // rb tree must be empty
    assert(fname->rbtree.rb_node== NULL);

    // clean list must be empty
    assert(list_begin(&fname->cleanlist) == NULL);

    // free hash
    hash_free(&fname->hashtable);

    free(fname->filename);
}

int bcache_read(struct filemgr *file, bid_t bid, void *buf)
{
    struct hash_elem *h;
    struct bcache_item *item;
    struct bcache_item query;
    struct fnamedic_item fname_query, *fname;

    fname = file->bcache;    
    if (fname) {
        // file exists        
        // set query
        query.bid = bid;
        query.fname = fname;
        query.fname->curfile = file;

        // relay lock
        spin_lock(&fname->lock);

        // move the file to the head of FILE_LRU
        _bcache_move_fname_list(fname, &file_lru);

        // search BHASH
        h = hash_find(&fname->hashtable, &query.hash_elem);
        if (h) {
            // cache hit
            item = _get_entry(h, struct bcache_item, hash_elem);
            assert(item->fname == fname);
            spin_lock(&item->lock);

            // move the item to the head of list if the block is clean (don't care if the block is dirty)
            if (!(item->flag & BCACHE_DIRTY)) {
                list_remove(&item->fname->cleanlist, &item->list_elem);
                list_push_front(&item->fname->cleanlist, &item->list_elem);
            }

            // relay lock
            spin_unlock(&fname->lock);
           
            memcpy(buf, item->addr, bcache_blocksize);    
            spin_unlock(&item->lock);
            
            return bcache_blocksize;
        }else {
            // cache miss
            spin_unlock(&fname->lock);
        }
    }

    // does not exist .. cache miss
    return 0;
}

int bcache_write(struct filemgr *file, bid_t bid, void *buf, bcache_dirty_t dirty)
{
    struct hash_elem *h = NULL;
    struct list_elem *e;
    struct bcache_item *item;
    struct bcache_item query;
    struct fnamedic_item fname_query, *fname_new;
    uint8_t marker;

    fname_new = file->bcache;
    if (fname_new == NULL) {
        // filename doesn't exist in filename dictionary .. create
        fname_new = _fname_create(file);
    }else{
        // file already exists 
    }

    // acquire lock
    spin_lock(&fname_new->lock);
    
    // move to the head of FILE_LRU
    _bcache_move_fname_list(fname_new, &file_lru);

    // set query
    query.bid = bid;
    query.fname = fname_new;
    query.fname->curfile = file;

    // search hash table
    h = hash_find(&fname_new->hashtable, &query.hash_elem);
    if (h == NULL) {
        // cache miss       
        // get a free block        
        while ((item = _bcache_alloc_freeblock()) == NULL) {
            // no free block .. perform eviction
            spin_unlock(&fname_new->lock);
            
            struct list_elem *victim_le = _bcache_evict(fname_new);
            struct bcache_item *victim = _get_entry(victim_le, struct bcache_item, list_elem);

            spin_lock(&fname_new->lock);
        }

        // re-search hash table
        h = hash_find(&fname_new->hashtable, &query.hash_elem);
        if (h == NULL) {
            // insert into hash table
            item->bid = bid;
            item->fname = fname_new;
            item->flag = 0x0;
            hash_insert(&fname_new->hashtable, &item->hash_elem);
            h = &item->hash_elem;
        }else{
            // insert into freelist again
            _bcache_release_freeblock(item);
        }
    }

    assert(h);
    item = _get_entry(h, struct bcache_item, hash_elem);
    spin_lock(&item->lock);
    
    // remove from the list if the block is in clean list
    if (!(item->flag & BCACHE_DIRTY)) {
        list_remove(&fname_new->cleanlist, &item->list_elem);
    }

    if (dirty == BCACHE_DIRTY) {
        // DIRTY request
        // to avoid re-insert already existing item into rb-tree
        if (!(item->flag & BCACHE_DIRTY)) {
            // dirty block
            // insert into rb-tree
            struct dirty_item *ditem;

            ditem = (struct dirty_item *)mempool_alloc(sizeof(struct dirty_item));
            ditem->item = item;
            
            rbwrap_insert(&item->fname->rbtree, &ditem->rb, _dirty_cmp);
        }
        item->flag |= BCACHE_DIRTY;
    }else{ 
        // CLEAN request
        // insert into clean list only when it was originally clean
        if (!(item->flag & BCACHE_DIRTY)) {
            list_push_front(&item->fname->cleanlist, &item->list_elem);
            item->flag &= ~(BCACHE_DIRTY);
        }
    }

    spin_unlock(&fname_new->lock);

    memcpy(item->addr, buf, bcache_blocksize);   
    spin_unlock(&item->lock);
    
    return bcache_blocksize;
}

int bcache_write_partial(struct filemgr *file, bid_t bid, void *buf, size_t offset, size_t len)
{
    struct hash_elem *h;
    struct list_elem *e;
    struct bcache_item *item;
    struct bcache_item query;
    struct fnamedic_item fname_query, *fname_new;
    uint8_t marker;

    fname_new = file->bcache;
    if (fname_new == NULL) {
        // filename doesn't exist in filename dictionary .. create
        fname_new = _fname_create(file);
    }else{
        // file already exists
    }

    // relay lock
    spin_lock(&fname_new->lock);
    
    // set query
    query.bid = bid;
    query.fname = fname_new;
    query.fname->curfile = file;

    // search hash table
    h = hash_find(&fname_new->hashtable, &query.hash_elem);
    if (h == NULL) {
        // cache miss .. partial write fail .. return 0
        spin_unlock(&fname_new->lock);
        return 0;
        
    }else{
        // cache hit .. get the block
        item = _get_entry(h, struct bcache_item, hash_elem);        
    }
    
    // move to the head of FILE_LRU
    _bcache_move_fname_list(fname_new, &file_lru);

    spin_lock(&item->lock);

    // check whether this is dirty block
    // to avoid re-insert already existing item into rb-tree
    if (!(item->flag & BCACHE_DIRTY)) {
        // this block was clean block
        struct dirty_item *ditem;

        // remove from clean list
        list_remove(&item->fname->cleanlist, &item->list_elem);

        ditem = (struct dirty_item *)mempool_alloc(sizeof(struct dirty_item));
        ditem->item = item;
        
        rbwrap_insert(&item->fname->rbtree, &ditem->rb, _dirty_cmp);
    }

    // always set this block as dirty
    item->flag |= BCACHE_DIRTY;

    spin_unlock(&fname_new->lock);

    memcpy(item->addr + offset, buf, len);
    spin_unlock(&item->lock);

    return len;
}

// remove all dirty blocks of the FILE (they are only discarded and not written back)
void bcache_remove_dirty_blocks(struct filemgr *file)
{
    struct hash_elem *h;
    struct list_elem *e;
    struct bcache_item *item;
    struct fnamedic_item fname_query, *fname_item;

    fname_item = file->bcache;
    
    if (fname_item) {
        // acquire lock
        spin_lock(&fname_item->lock);

        // remove all dirty block
        while(fname_item->rbtree.rb_node) {
            _bcache_evict_dirty(fname_item, 0);
        }

        // check whether the victim file is empty
        if (list_begin(&fname_item->cleanlist) == NULL && fname_item->rbtree.rb_node == NULL) {
            // remove from FILE_LRU and insert into FILE_EMPTY
            _bcache_move_fname_list(fname_item, &file_empty);
        }

        spin_unlock(&fname_item->lock);
        return;
    }
}

// remove all clean blocks of the FILE
void bcache_remove_clean_blocks(struct filemgr *file)
{
    struct list_elem *e;
    struct hash_elem *h;
    struct bcache_item *item;
    struct fnamedic_item *fname_item, fname_query;

    fname_item = file->bcache;

    if (fname_item) {
        // acquire lock
        spin_lock(&fname_item->lock);

        // remove all clean blocks
        e = list_begin(&fname_item->cleanlist);
        while(e){
            item = _get_entry(e, struct bcache_item, list_elem);

            // remove from clean list
            e = list_remove(&fname_item->cleanlist, e);
            // remove from hash table
            hash_remove(&fname_item->hashtable, &item->hash_elem);
            // insert into free list
            _bcache_release_freeblock(item);
        }

        // check whether the victim file is empty
        if (list_begin(&fname_item->cleanlist) == NULL && fname_item->rbtree.rb_node == NULL) {
            // remove from FILE_LRU and insert into FILE_EMPTY
            _bcache_move_fname_list(fname_item, &file_empty);
        }       

        spin_unlock(&fname_item->lock);
        return;
    }
}

// remove file from filename dictionary
// MUST sure that there is no dirty block belongs to this FILE (or memory leak occurs)
void bcache_remove_file(struct filemgr *file)
{
    struct hash_elem *h;
    struct fnamedic_item *fname_item, fname_query;

    fname_item = file->bcache;

    if (fname_item) {
        assert(fname_item->rbtree.rb_node == NULL);        
        assert(fname_item->cleanlist.head == NULL);

        // acquire lock
        spin_lock(&bcache_lock);
        spin_lock(&fname_item->lock);

        // remove from fname dictionary hash table
        hash_remove(&fnamedic, &fname_item->hash_elem);
        spin_unlock(&bcache_lock);        
        
        _fname_free(fname_item);

        spin_unlock(&fname_item->lock);

        free(fname_item);
        return;
    }
}

// flush and synchronize all dirty blocks of the FILE
// dirty blocks will be changed to clean blocks (not discarded)
void bcache_flush(struct filemgr *file)
{
    struct hash_elem *h;
    struct list_elem *e;
    struct bcache_item *item;
    struct fnamedic_item fname_query, *fname_item;

    fname_item = file->bcache;

    if (fname_item) {
        // acquire lock
        spin_lock(&fname_item->lock);

        while(fname_item->rbtree.rb_node) {    
            _bcache_evict_dirty(fname_item, 1);
        }

        spin_unlock(&fname_item->lock);
        return;
    }
}

void bcache_init(int nblock, int blocksize)
{
    int i, ret;
    struct bcache_item *item;
    struct list_elem *e;

    list_init(&freelist);
    list_init(&file_lru);
    list_init(&file_empty);
    
    hash_init(&fnamedic, BCACHE_NDICBUCKET, _fname_hash, _fname_cmp);
    
    bcache_blocksize = blocksize;
    bcache_flush_unit = BCACHE_FLUSH_UNIT;
    bcache_sys_pagesize = sysconf(_SC_PAGESIZE);
    bcache_nblock = nblock;
    bcache_lock = freelist_lock = filelist_lock = SPIN_INITIALIZER;

    for (i=0;i<nblock;++i){
        item = (struct bcache_item *)malloc(sizeof(struct bcache_item));
        
        item->bid = BLK_NOT_FOUND;
        item->fname = NULL;
        item->flag = 0x0;
        item->lock = SPIN_INITIALIZER;

        list_push_front(&freelist, &item->list_elem);
        freelist_count++;
        //hash_insert(&bhash, &item->hash_elem);
    }
    e = list_begin(&freelist);
    while(e){
        item = _get_entry(e, struct bcache_item, list_elem);
        //item->addr = (void *)malloc(bcache_blocksize);
        item->addr = (void *)calloc(1, bcache_blocksize);
        e = list_next(e);
    }

}

INLINE void _bcache_free_bcache_item(struct hash_elem *h)
{
    struct bcache_item *item = _get_entry(h, struct bcache_item, hash_elem);
    free(item->addr);
    free(item);
}

INLINE void _bcache_free_fnamedic(struct hash_elem *h)
{
    struct fnamedic_item *item = _get_entry(h, struct fnamedic_item, hash_elem);
    hash_free_active(&item->hashtable, _bcache_free_bcache_item);

    _bcache_move_fname_list(item, NULL);

    free(item->filename);
    free(item);
}

void bcache_shutdown()
{
    struct bcache_item *item;
    struct list_elem *e;

    //__bcache_check_bucket_length();

    e = list_begin(&freelist);
    while(e) {
        item = _get_entry(e, struct bcache_item, list_elem);
        e = list_remove(&freelist, e);
        free(item->addr);
        free(item);
    }

    spin_lock(&bcache_lock);
    hash_free_active(&fnamedic, _bcache_free_fnamedic);
    spin_unlock(&bcache_lock);
}

