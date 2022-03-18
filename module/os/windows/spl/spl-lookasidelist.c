#include <sys/zfs_context.h>
#include <sys/lookasidelist.h>

void *
allocate_func(
    __in POOL_TYPE  PoolType,
    __in SIZE_T  NumberOfBytes,
    __in ULONG  Tag,
    __inout PLOOKASIDE_LIST_EX  Lookaside)
{
	lookasidelist_cache_t *pLookasidelist_cache;
	pLookasidelist_cache = CONTAINING_RECORD(Lookaside,
	    lookasidelist_cache_t, lookasideField);
	void *buf = osif_malloc(NumberOfBytes);
	ASSERT(buf != NULL);

	if (buf != NULL) {
		atomic_inc_32(&pLookasidelist_cache->cache_active_allocations);
		// can also be calculated from the Lookaside list as below
		// Lookaside->L.TotalAllocates - Lookaside->L.TotalFrees
	}

	return (buf);
}

void free_func(
    __in PVOID  Buffer,
    __inout PLOOKASIDE_LIST_EX  Lookaside
) {
	lookasidelist_cache_t *pLookasidelist_cache;
	pLookasidelist_cache = CONTAINING_RECORD(Lookaside,
	    lookasidelist_cache_t, lookasideField);
	osif_free(Buffer, pLookasidelist_cache->cache_chunksize);
	atomic_dec_32(&pLookasidelist_cache->cache_active_allocations);
}

lookasidelist_cache_t *
lookasidelist_cache_create(size_t size)
{
	lookasidelist_cache_t *pLookasidelist_cache;
	pLookasidelist_cache = ExAllocatePoolWithTag(NonPagedPool,
	    sizeof (lookasidelist_cache_t), ZFS_LookAsideList_DRV_TAG);

	if (pLookasidelist_cache != NULL) {
		NTSTATUS retval = ExInitializeLookasideListEx(
		    &pLookasidelist_cache->lookasideField,
		    allocate_func,
		    free_func,
		    NonPagedPoolNx,
		    0,
		    size,
		    ZFS_LookAsideList_DRV_TAG,
		    0);

		ASSERT(retval == STATUS_SUCCESS);

		if (retval == STATUS_SUCCESS) {
			pLookasidelist_cache->cache_chunksize = size;
			pLookasidelist_cache->cache_active_allocations = 0;
		} else {
			ExFreePoolWithTag(pLookasidelist_cache,
			    ZFS_LookAsideList_DRV_TAG);
			pLookasidelist_cache = NULL;
		}
	}

	return (pLookasidelist_cache);
}

void
lookasidelist_cache_destroy(lookasidelist_cache_t *pLookasidelist_cache)
{
	if (pLookasidelist_cache != NULL) {
		ExFlushLookasideListEx(&pLookasidelist_cache->lookasideField);
		ExDeleteLookasideListEx(&pLookasidelist_cache->lookasideField);
		ExFreePoolWithTag(pLookasidelist_cache,
		    ZFS_LookAsideList_DRV_TAG);
	}
}

void *
lookasidelist_cache_alloc(lookasidelist_cache_t *pLookasidelist_cache)
{
	void *buf = ExAllocateFromLookasideListEx(
	    &pLookasidelist_cache->lookasideField);
	ASSERT(buf != NULL);
	return (buf);
}

void
lookasidelist_cache_free(lookasidelist_cache_t *pLookasidelist_cache, void *buf)
{
	ASSERT(buf != NULL);
	if (buf != NULL) {
		ExFreeToLookasideListEx(&pLookasidelist_cache->lookasideField,
		    buf);
	}
}
