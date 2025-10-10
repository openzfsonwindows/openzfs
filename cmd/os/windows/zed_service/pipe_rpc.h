#ifndef OPENZFS_PIPE_RPC_H
#define	OPENZFS_PIPE_RPC_H

#include <stdint.h>

#undef dprintf
static void
dprintf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof (buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	OutputDebugStringA(buf);
}

// Add in fake pools so we can test UI without a real pool
#define	ENABLE_FAKE_POOLS 1

#define	OPENZFS_PIPE_NAME "\\\\.\\pipe\\openzfs_zed"

#pragma pack(push, 1)
typedef enum {
    OP_GET_STATUS = 1,
    OP_LIST_POOLS = 2,
    OP_IMPORT_ALL = 3,
    OP_IMPORT_SCAN = 4,
    OP_IMPORT_ONE = 5,
    OP_SUBSCRIBE_EVENTS = 6,
    OP_EXPORT_ALL = 7,
    OP_EXPORT_ONE = 8,
} op_t;

typedef struct {
    uint32_t op; // op_t
    uint32_t len; // payload length in bytes (follows header)
} req_hdr_t;

typedef struct {
    uint32_t status; // 0 == OK, else Win32-style or your own
    uint32_t len; // payload length in bytes (follows header)
} rsp_hdr_t;
#pragma pack(pop)

// pipe_rpc.h
typedef enum {
    ZFSV_SUMMARY = 0,  // name/health/size/alloc/free/capacity
    ZFSV_INCLUDE_VDEVS = 1, // + vdev_tree
} zfs_status_verbosity_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t  verbosity; // zfs_status_verbosity_t
    uint8_t  reserved[3];
    uint64_t guid; // target pool GUID
} op_get_status_by_guid_req_t;
#pragma pack(pop)

enum {
    ZIMP_FORCE = 0x01, // zpool import -f
    ZIMP_READONLY = 0x02, // readonly=on
    ZIMP_NOMOUNT = 0x04, // -N
};

#pragma pack(push, 1)
typedef struct {
	uint32_t flags; // ZIMP_*
	// followed by optional UTF-8 altroot (NUL-terminated) in the tail
} op_import_all_req_t;

typedef struct {
	uint32_t flags; // ZIMP_* (can influence scan heuristics)
	// future: search paths, cachefile, pool name filter
} op_import_scan_req_t;

typedef struct {
	uint32_t flags; // ZIMP_*
	uint64_t guid; // which pool to import
	// followed by optional UTF-8 new_name (NUL-terminated)
	// followed by optional UTF-8 altroot (NUL-terminated)
} op_import_one_req_t;
#pragma pack(pop)

enum {
    ZEXP_FORCE = 0x01, // zpool export -f  (force unmount datasets)
    ZEXP_HARD = 0x02, // optional: hard force if your tree supports it
};

#pragma pack(push, 1)
typedef struct {
    uint32_t flags; // ZEXP_*
} op_export_all_req_t;

typedef struct {
    uint32_t flags; // ZEXP_*
    uint64_t guid; // pool to export (we’ll resolve to a handle)
} op_export_one_req_t;
#pragma pack(pop)

#endif // OPENZFS_PIPE_RPC_H
