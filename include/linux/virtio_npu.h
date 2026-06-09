#ifndef _LINUX_VIRTIO_NPU_H
#define _LINUX_VIRTIO_NPU_H

#include <linux/types.h>

/*
 * Experimental local virtio device id. This is not a reserved upstream
 * virtio id; the guest driver must use the same value during this project.
 */
#define VIRTIO_ID_NPU			0xff00

#define VIRTIO_NPU_QUEUE_SIZE		128

#define VIRTIO_NPU_CMD_GET_INFO		1
#define VIRTIO_NPU_CMD_PING		2
#define VIRTIO_NPU_CMD_INFER_DUMMY	3
#define VIRTIO_NPU_CMD_INFER_RAW	4


#define VIRTIO_NPU_STATUS_OK		0
#define VIRTIO_NPU_STATUS_UNSUPP	1
#define VIRTIO_NPU_STATUS_BAD_REQ	2
#define VIRTIO_NPU_STATUS_IOERR		3


struct virtio_npu_req {
	__le32 cmd;
	__le32 flags;
	__le32 input_len;
	__le32 output_len;
};

struct virtio_npu_resp {
	__le32 status;
	__le32 value;
};

#endif /* _LINUX_VIRTIO_NPU_H */
