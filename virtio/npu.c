#include "kvm/virtio-npu.h"

#include "kvm/virtio.h"
#include "kvm/util.h"
#include "kvm/kvm.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/virtio_npu.h>
#include <string.h>
#include <dlfcn.h>

#define NUM_VIRT_QUEUES	1

struct virtio_npu_backend_ops;

struct npu_dev {
	struct list_head	list;
	struct virtio_device	vdev;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];

	/*
		backend_ops:
		当前使用哪个后端，比如 dummy 或 rknn

		backend_data:
		后端自己的私有数据
	*/
	struct kvm *kvm;
	const struct virtio_npu_backend_ops *backend_ops;
	void *backend_data;
};

struct virtio_npu_rknn_backend;

struct virtio_npu_rknn_data {

	//动态库？打开的 librknn_backend.so
	void *dl_handle;
	//模型上下文
	struct virtio_npu_rknn_backend *backend;

	int(*create)(const char *model_path, struct virtio_npu_rknn_backend **out);
	void(*destroy)(struct virtio_npu_rknn_backend *backend);
	int(*infer_raw)(struct virtio_npu_rknn_backend *backend,
				const void *input, size_t input_len,
				void *output, size_t output_len,
				u32 *actual_output_len);

	int(*create_from_buffer)(const void *model_data, size_t model_len, struct virtio_npu_rknn_backend **out);
};


struct virtio_npu_backend_result {
	u32 status;
	u32 actual_output_len;
};

struct virtio_npu_backend_ops {
	int (*init)(struct npu_dev *ndev);
	void (*exit)(struct npu_dev *ndev);
	struct virtio_npu_backend_result (*infer)(struct npu_dev *ndev,
						  const void *input,
						  size_t input_len,
						  void *output,
						  size_t output_len);
};

static LIST_HEAD(ndevs);

static struct virtio_npu_rknn_data *virtio_npu_rknn_open(void)
{
	struct virtio_npu_rknn_data *data;

	data = calloc(1, sizeof(*data));
	if (!data)
		return NULL;

	data->dl_handle = dlopen("./virtio-npu-rknn/librknn_backend.so", RTLD_NOW);
	if (!data->dl_handle) {
		pr_warning("virtio-npu: failed to dlopen RKNN backend: %s", dlerror());
		free(data);
		return NULL;
	}

	//从动态库里拿到函数指针
	/*
		create: 根据模型文件路径创建 RKNN 后端实例,模型在 host 文件系统里
	*/
	data->create = dlsym(data->dl_handle, "virtio_npu_rknn_backend_create");
	data->destroy = dlsym(data->dl_handle, "virtio_npu_rknn_backend_destroy");
	data->infer_raw = dlsym(data->dl_handle, "virtio_npu_rknn_backend_infer_raw");
	/*
		create_from_buffer: 模型已经在内存里
	*/
	data->create_from_buffer =
		dlsym(data->dl_handle, "virtio_npu_rknn_backend_create_from_buffer");

	if (!data->create || !data->destroy || !data->infer_raw ||
	    !data->create_from_buffer) {
		pr_warning("virtio-npu: missing RKNN backend symbols");
		dlclose(data->dl_handle);
		free(data);
		return NULL;
	}

	return data;
}

static int virtio_npu_rknn_init(struct npu_dev *ndev)
{
	const char *model = ndev->kvm->cfg.virtio_npu_model;
	struct virtio_npu_rknn_data *data;
	int ret;

	if(!model)
		return -EINVAL;

	data = virtio_npu_rknn_open();
	if (!data)
		return -ENOMEM;

	ret = data->create(model, &data->backend);
	if (ret) {
		pr_warning("virtio-npu: failed to create RKNN backend: %d", ret);
		dlclose(data->dl_handle);
		free(data);
		return ret;
	}

	ndev->backend_data = data;
	return 0;
}
static struct virtio_npu_backend_result
virtio_npu_rknn_infer(struct npu_dev *ndev, const void *input,
		      size_t input_len, void *output, size_t output_len)
{
	struct virtio_npu_rknn_data *data = ndev->backend_data;

	struct virtio_npu_backend_result result = {
		.status = VIRTIO_NPU_STATUS_OK,
	};

	u32 actual_len = 0;
	int ret;

	if (!data || !data->infer_raw) {
		result.status = VIRTIO_NPU_STATUS_IOERR;
		return result;
	}

	ret = data->infer_raw(data->backend, input, input_len,
			      output, output_len, &actual_len);
	if (ret) {
		result.status = VIRTIO_NPU_STATUS_IOERR;
		return result;
	}
	result.actual_output_len = actual_len;
	return result;
}
static void virtio_npu_rknn_exit(struct npu_dev *ndev)
{
	struct virtio_npu_rknn_data *data = ndev->backend_data;

	if (!data)
		return;

	if (data->destroy && data->backend)
		data->destroy(data->backend);

	if (data->dl_handle)
		dlclose(data->dl_handle);

	free(data);
	ndev->backend_data = NULL;
}
static const struct virtio_npu_backend_ops virtio_npu_rknn_ops = {
	.init = virtio_npu_rknn_init,
	.exit = virtio_npu_rknn_exit,
	.infer = virtio_npu_rknn_infer,
};



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

static struct virtio_npu_backend_result
virtio_npu_dummy_infer(struct npu_dev *ndev, const void *input,
		       size_t input_len, void *output, size_t output_len)
{
	const char prefix[] = "dummy: ";
	const size_t prefix_len = strlen(prefix);
	char *output_buf = output;
	struct virtio_npu_backend_result result = {
		.status = VIRTIO_NPU_STATUS_OK,
	};
	size_t copy_len;

	(void)ndev;

	if (!input || !input_len || !output || output_len <= prefix_len) {
		result.status = VIRTIO_NPU_STATUS_BAD_REQ;
		return result;
	}

	copy_len = min_t(size_t, input_len, output_len - prefix_len - 1);

	memcpy(output_buf, prefix, prefix_len);
	memcpy(output_buf + prefix_len, input, copy_len);
	output_buf[prefix_len + copy_len] = '\0';

	result.actual_output_len = prefix_len + copy_len;
	return result;
}

static const struct virtio_npu_backend_ops virtio_npu_dummy_ops = {
	.infer = virtio_npu_dummy_infer,
};

static size_t virtio_npu_do_infer_common(struct npu_dev *ndev,
					const struct virtio_npu_backend_ops *ops,
					struct virt_queue *vq,
					struct iovec out_iov[], u16 out,
					struct iovec in_iov[], u16 in,
					struct virtio_npu_req *req)
{
	u32 input_len;
	u32 output_len;
	struct virtio_npu_backend_result result;
	char *guest_input;
	char *guest_output;

	if (out < 2 || in < 2)
		return 0;

	if (!out_iov[0].iov_base ||
		out_iov[0].iov_len < sizeof(struct virtio_npu_req))
		return 0;

	if (!out_iov[1].iov_base || !out_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_BAD_REQ, 0);

	//这里如果连响应头都没有，那没地方写错误响应，所以直接return 0，等于不处理这个请求了，guest那边自然也收不到响应了。
	if (!in_iov[0].iov_base ||
		in_iov[0].iov_len < sizeof(struct virtio_npu_resp))
		return 0;

	if (!in_iov[1].iov_base || !in_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_BAD_REQ, 0);

	input_len = virtio_guest_to_host_u32(vq->endian, req->input_len);
	output_len = virtio_guest_to_host_u32(vq->endian, req->output_len);

	if (!input_len || input_len > out_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
					     VIRTIO_NPU_STATUS_BAD_REQ, 0);

	if (!output_len || output_len > in_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
					     VIRTIO_NPU_STATUS_BAD_REQ, 0);

	guest_input = out_iov[1].iov_base;
	guest_output = in_iov[1].iov_base;

	if (!ops || !ops->infer)
		return virtio_npu_write_resp(vq, in_iov, in,
					     VIRTIO_NPU_STATUS_IOERR, 0);

	result = ops->infer(ndev, guest_input, input_len,
			    guest_output, output_len);
	return virtio_npu_write_resp(vq, in_iov, in,
				    result.status, result.actual_output_len);
}





static int virtio_npu_load_model_from_buffer(struct npu_dev *ndev,
					     const void *model,
					     size_t model_len)
{
	struct virtio_npu_rknn_data *olddata;
	struct virtio_npu_rknn_data *data;
	int ret;

	if (!ndev || !model || !model_len)
		return -EINVAL;

	data = virtio_npu_rknn_open();
	if (!data)
		return -ENOMEM;

	ret = data->create_from_buffer(model, model_len, &data->backend);
	if (ret) {
		pr_warning("virtio-npu: failed to create RKNN backend from uploaded model: %d",
			   ret);
		if (data->dl_handle)
			dlclose(data->dl_handle);
		free(data);
		return ret;
	}

	olddata = ndev->backend_data;
	ndev->backend_data = data;
	ndev->backend_ops = &virtio_npu_rknn_ops;
	if (olddata) {
		if (olddata->destroy && olddata->backend)
			olddata->destroy(olddata->backend);

		if (olddata->dl_handle)
			dlclose(olddata->dl_handle);

		free(olddata);
	}
	return 0;

}
/*
	out = 2
	in = 1
*/
static size_t virtio_npu_do_load_model(struct npu_dev *ndev,
					struct virt_queue *vq,
					struct iovec out_iov[], u16 out,
					struct iovec in_iov[], u16 in,
					struct virtio_npu_req *req)
{
	u32 model_len;
	void *model_data;
	int ret;
	if (out < 2 || in < 1)
		return 0;
	//检查请求头
	if(!out_iov[0].iov_base || out_iov[0].iov_len < sizeof(struct virtio_npu_req))
		return 0;
	//检查模型数据
	if (!out_iov[1].iov_base || !out_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_BAD_REQ, 0);
	//检查响应头
	if (!in_iov[0].iov_base ||
		in_iov[0].iov_len < sizeof(struct virtio_npu_resp))
		return 0;

	model_len = virtio_guest_to_host_u32(vq->endian, req->input_len);
	model_data = out_iov[1].iov_base;

	if(!model_len || model_len > out_iov[1].iov_len)
		return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_BAD_REQ, 0);
	ret = virtio_npu_load_model_from_buffer(ndev,model_data,model_len);

	/*
		LOAD_MODEL 成功返回这里建议返回 handle 1，不是 0,因为 guest 会把 resp.value 当成 model_handle。
	*/
	if(ret)
		return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_IOERR, 0);

	return virtio_npu_write_resp(vq, in_iov, in,
						VIRTIO_NPU_STATUS_OK, 1);
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
		case VIRTIO_NPU_CMD_INFER_DUMMY:
			used_len = virtio_npu_do_infer_common(ndev, &virtio_npu_dummy_ops,
								vq, out_iov, out,
								in_iov, in, &req);
				break;
		case VIRTIO_NPU_CMD_INFER_RAW:
			used_len = virtio_npu_do_infer_common(ndev, ndev->backend_ops,
								vq, out_iov, out,
								in_iov, in, &req);
				break;
		case VIRTIO_NPU_CMD_LOAD_MODEL:
			used_len = virtio_npu_do_load_model(ndev, vq, out_iov, out, in_iov, in, &req);
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

	ndev->kvm = kvm;

	if (kvm->cfg.virtio_npu_model)
		ndev->backend_ops = &virtio_npu_rknn_ops;
	else
		ndev->backend_ops = &virtio_npu_dummy_ops;

	if (ndev->backend_ops->init) {
		r = ndev->backend_ops->init(ndev);
		if (r < 0) {
			free(ndev);
			return r;
		}
	}

	r = virtio_init(kvm, ndev, &ndev->vdev, &npu_dev_virtio_ops,
			kvm->cfg.virtio_transport, VIRTIO_ID_NPU,
			VIRTIO_ID_NPU, 0);
	if (r < 0) {
		if (ndev->backend_ops->exit)
			ndev->backend_ops->exit(ndev);
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
		if (ndev->backend_ops && ndev->backend_ops->exit)
			ndev->backend_ops->exit(ndev);
		free(ndev);
	}

	return 0;
}
virtio_dev_exit(virtio_npu__exit);
