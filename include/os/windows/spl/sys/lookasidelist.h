#ifndef _SPL_LOOKASIDELIST_H
#define	_SPL_LOOKASIDELIST_H

extern void* osif_malloc(uint64_t);
extern void osif_free(void*, uint64_t);

#define	ZFS_LookAsideList_DRV_TAG '!SFZ'

typedef struct lookasidelist_cache {
    ULONG cache_active_allocations;
    size_t cache_chunksize; // cache object size
    LOOKASIDE_LIST_EX lookasideField;
} lookasidelist_cache_t;

lookasidelist_cache_t *lookasidelist_cache_create(size_t size);
void lookasidelist_cache_destroy(lookasidelist_cache_t *pLookasidelist_cache);
void* lookasidelist_cache_alloc(lookasidelist_cache_t *pLookasidelist_cache);
void lookasidelist_cache_free(lookasidelist_cache_t *pLookasidelist_cache,
    void *buf);

#endif
