#ifndef OPENZFS_PIPE_RPC_H
#define	OPENZFS_PIPE_RPC_H

#include <stdint.h>

#define	OPENZFS_PIPE_NAME "\\\\.\\pipe\\openzfs_zed"

#pragma pack(push, 1)
typedef enum {
    OP_GET_STATUS = 1,
    OP_IMPORT = 2,
    OP_EXPORT = 3,
    OP_SCRUB_START = 4,
    OP_SCRUB_STOP = 5,
    OP_CLEAR = 6,
    OP_SUBSCRIBE_EVENTS = 7,
    OP_LIST_POOLS = 8,
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

#endif // OPENZFS_PIPE_RPC_H
