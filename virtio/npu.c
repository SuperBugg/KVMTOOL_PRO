#include "kvm/virtio-npu.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/virtio_npu.h>
#include <string.h>

#define NUM_VIRT_QUEUES		1

struct npu_dev {
	struct list_head	list;
	struct virtio_device	vdev;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
};

static LIST_HEAD(ndevs);

static u8 *get_config(struct kvm *kvm, void *dev)
{
	return NULL;
}

static size_t get_config_size(struct kvm *kvm, void *dev)
{
	return 0;
}

static u64 get_host_features(struct kvm *kvm, void *dev)
{
	return 0;
}

/*
	struct iovec{
		void *iov_base;
		size_t iov_len;
	}
*/
static size_t virtio_npu_copy_to_iov(struct iovec iov[],u16 iovcnt,const void *buf,size_t len)
{
	const u8 *src = buf;
	size_t copied = 0;
	u16 i;
	for(i = 0; i < iovcnt && copied < len;i++){
		size_t n = min(iov[i].iov_len,len - copied);

		if (!iov[i].iov_base)
			break;

		memcpy(iov[i].iov_base,src+copied,n);
		copied += n;
	}

	return copied;
}

/*
	这里虽然是写给guest的数据，但是host写，所以面向的也是iovec
	用virtio_npu_copy_to_iov就可以
*/
static size_t virtio_npu_write_resp(struct virt_queue *vq,struct iovec in_iov[],u16 in,u32 status, u32 value)
{
	struct virtio_npu_resp resp = {
		.status = virtio_host_to_guest_u32(vq->endian, status),
		.value = virtio_host_to_guest_u32(vq->endian, value),
	};

	return virtio_npu_copy_to_iov(in_iov,in,&resp,sizeof(resp));
}

static bool virtio_npu_has_resp_buf(struct iovec in_iov[], u16 in)
{
	return in && in_iov[0].iov_base &&
	       in_iov[0].iov_len >= sizeof(struct virtio_npu_resp);
}


static void virtio_npu_do_request(struct kvm *kvm,struct npu_dev *ndev,struct virt_queue *vq)
{
	struct iovec in_iov[VIRTIO_NPU_QUEUE_SIZE];
	struct iovec out_iov[VIRTIO_NPU_QUEUE_SIZE];
	/*
		这里是out，也就是guest -> host的结构体！！
	*/
	struct virtio_npu_req req;
	size_t used_len;
	u32 cmd;
	u16 in, out, head;

	head = virt_queue__get_inout_iov(kvm, vq, in_iov, out_iov, &in, &out);
	if (!virtio_npu_has_resp_buf(in_iov, in)) {
		pr_warning("virtio-npu: request without valid response buffer");
		virt_queue__set_used_elem(vq, head, 0);
		return;
	}

	if (!out || !out_iov[0].iov_base || out_iov[0].iov_len < sizeof(req)) {
		pr_warning("virtio-npu: bad request buffer");
		used_len = virtio_npu_write_resp(vq, in_iov, in,
						 VIRTIO_NPU_STATUS_BAD_REQ, 0);

		/*
			这个 head 对应的请求我已经处理完了。
			虽然结果是 BAD_REQ，但你可以回收这个 descriptor chain 了。
			我写回了 used_len 字节。
		*/
		virt_queue__set_used_elem(vq, head, used_len);
		return;
	}

		// if (!in || in_iov[0].iov_len < sizeof(struct virtio_npu_resp)) {
		// 	;
		// }

	/*
		目前的协议中，每个请求的第一个out buff必须是struct virtio_npu_req

	*/
	memcpy(&req,out_iov[0].iov_base,sizeof(req));
	cmd = virtio_guest_to_host_u32(vq->endian,req.cmd);

	switch(cmd){

		case VIRTIO_NPU_CMD_GET_INFO:
			used_len = virtio_npu_write_resp(vq, in_iov, in,
	                                        VIRTIO_NPU_STATUS_OK, 1);
			break;
        case VIRTIO_NPU_CMD_PING:
                used_len = virtio_npu_write_resp(vq, in_iov, in,
                                                 VIRTIO_NPU_STATUS_OK, 0x4e5055);
                break;
        default:
                used_len = virtio_npu_write_resp(vq, in_iov, in,
                                                 VIRTIO_NPU_STATUS_UNSUPP, cmd);
	                break;
	}

	virt_queue__set_used_elem(vq, head, used_len);
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct npu_dev *ndev = dev;


	/*
		它会把 guest 之前通过 virtio-mmio register 写进来的：
			QUEUE_DESC
			QUEUE_AVAIL
			QUEUE_USED
		转换成：
			ndev->vqs[vq].vring.desc
			ndev->vqs[vq].vring.avail
			ndev->vqs[vq].vring.used

		init_vq() 负责让 kvmtool 真正拿到 guest virtqueue 的 desc/avail/used 地址。
	*/
	virtio_init_device_vq(kvm, &ndev->vdev, &ndev->vqs[vq],
			      VIRTIO_NPU_QUEUE_SIZE);

	return 0;
}

static void exit_vq(struct kvm *kvm, void *dev, u32 vq)
{
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct npu_dev *ndev = dev;
	struct virt_queue *queue = &ndev->vqs[vq];

	while (virt_queue__available(queue))
		virtio_npu_do_request(kvm, ndev, queue);

	ndev->vdev.ops->signal_vq(kvm, &ndev->vdev, vq);

	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct npu_dev *ndev = dev;

	return &ndev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return VIRTIO_NPU_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	return size;
}

static unsigned int get_vq_count(struct kvm *kvm, void *dev)
{
	return NUM_VIRT_QUEUES;
}

static struct virtio_ops npu_dev_virtio_ops = {
	.get_config		= get_config,
	.get_config_size	= get_config_size,
	.get_host_features	= get_host_features,
	.init_vq		= init_vq,
	.exit_vq		= exit_vq,
	.notify_vq		= notify_vq,
	.get_vq			= get_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq		= set_size_vq,
	.get_vq_count		= get_vq_count,
};

/*
	设备注册入口
*/
int virtio_npu__init(struct kvm *kvm)
{
	struct npu_dev *ndev;
	int r;

	if (!kvm->cfg.virtio_npu)
		return 0;

	ndev = calloc(1, sizeof(*ndev));
	if (ndev == NULL)
		return -ENOMEM;

	r = virtio_init(kvm, ndev, &ndev->vdev, &npu_dev_virtio_ops,
			kvm->cfg.virtio_transport, VIRTIO_ID_NPU,
			VIRTIO_ID_NPU, 0);
	if (r < 0) {
		free(ndev);
		return r;
	}

	list_add_tail(&ndev->list, &ndevs);
	pr_info("virtio-npu: registered experimental device id 0x%x",
		VIRTIO_ID_NPU);

	return 0;
}
virtio_dev_init(virtio_npu__init);

int virtio_npu__exit(struct kvm *kvm)
{
	struct npu_dev *ndev, *tmp;

	list_for_each_entry_safe(ndev, tmp, &ndevs, list) {
		list_del(&ndev->list);
		virtio_exit(kvm, &ndev->vdev);
		free(ndev);
	}

	return 0;
}
virtio_dev_exit(virtio_npu__exit);
