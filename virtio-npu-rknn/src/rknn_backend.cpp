#include "rknn_backend.h"

#include "rknn_api.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

struct virtio_npu_rknn_backend {
	rknn_context ctx;
	uint32_t input_count;
	uint32_t output_count;
};
static int load_file(const char *path, std::vector<uint8_t> *data)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	std::streamsize size;

	if (!file)
		return -ENOENT;

	size = file.tellg();
	if (size <= 0)
		return -EINVAL;

	data->resize(static_cast<size_t>(size));
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char *>(data->data()), size))
		return -EIO;

	return 0;
}

int virtio_npu_rknn_backend_create(const char *model_path,
				   struct virtio_npu_rknn_backend **out)
{
	std::unique_ptr<struct virtio_npu_rknn_backend> backend;
	std::vector<uint8_t> model;
	rknn_input_output_num io_num;
	int ret;

	if (!model_path || !out)
		return -EINVAL;

	ret = load_file(model_path, &model);
	if (ret)
		return ret;

	backend.reset(new virtio_npu_rknn_backend());
	memset(backend.get(), 0, sizeof(*backend));

	ret = rknn_init(&backend->ctx, model.data(), model.size(), 0, nullptr);
	if (ret != RKNN_SUCC)
		return -EIO;

	memset(&io_num, 0, sizeof(io_num));
	ret = rknn_query(backend->ctx, RKNN_QUERY_IN_OUT_NUM,
			 &io_num, sizeof(io_num));
	if (ret != RKNN_SUCC) {
		rknn_destroy(backend->ctx);
		return -EIO;
	}

	backend->input_count = io_num.n_input;
	backend->output_count = io_num.n_output;
	if (backend->input_count < 1 || backend->output_count < 1) {
		rknn_destroy(backend->ctx);
		return -EINVAL;
	}

	*out = backend.release();
	return 0;
}

int virtio_npu_rknn_backend_create_from_buffer(
	const void *model_data,
	size_t model_len,
	struct virtio_npu_rknn_backend **out)
{
	std::unique_ptr<struct virtio_npu_rknn_backend> backend;
	rknn_input_output_num io_num;
	int ret;

	if (!model_data || !model_len || !out)
		return -EINVAL;

	backend.reset(new virtio_npu_rknn_backend());
	memset(backend.get(), 0, sizeof(*backend));

	ret = rknn_init(&backend->ctx,
			const_cast<void *>(model_data),
			model_len,
			0,
			nullptr);
	if (ret != RKNN_SUCC)
		return -EIO;

	memset(&io_num, 0, sizeof(io_num));
	ret = rknn_query(backend->ctx,
			 RKNN_QUERY_IN_OUT_NUM,
			 &io_num,
			 sizeof(io_num));
	if (ret != RKNN_SUCC) {
		rknn_destroy(backend->ctx);
		return -EIO;
	}

	backend->input_count = io_num.n_input;
	backend->output_count = io_num.n_output;

	if (backend->input_count < 1 || backend->output_count < 1) {
		rknn_destroy(backend->ctx);
		return -EINVAL;
	}

	*out = backend.release();
	return 0;
}


void virtio_npu_rknn_backend_destroy(struct virtio_npu_rknn_backend *backend)
{
	if (!backend)
		return;

	rknn_destroy(backend->ctx);
	delete backend;
}

int virtio_npu_rknn_backend_infer_raw(struct virtio_npu_rknn_backend *backend,
				      const void *input, size_t input_len,
				      void *output, size_t output_len,
				      uint32_t *actual_output_len)
{
	std::vector<rknn_output> outputs;
	rknn_input rknn_input_desc;
	size_t copied = 0;
	int ret;

	if (!backend || !input || !input_len || !output ||
	    !output_len || !actual_output_len)
		return -EINVAL;

	memset(&rknn_input_desc, 0, sizeof(rknn_input_desc));
	rknn_input_desc.index = 0;
	rknn_input_desc.type = RKNN_TENSOR_UINT8;
	rknn_input_desc.fmt = RKNN_TENSOR_NHWC;
	rknn_input_desc.size = input_len;
	rknn_input_desc.buf = const_cast<void *>(input);

	ret = rknn_inputs_set(backend->ctx, 1, &rknn_input_desc);
	if (ret != RKNN_SUCC)
		return -EIO;

	ret = rknn_run(backend->ctx, nullptr);
	if (ret != RKNN_SUCC)
		return -EIO;

	outputs.resize(backend->output_count);
	memset(outputs.data(), 0, outputs.size() * sizeof(outputs[0]));
	for (auto &out : outputs)
		out.want_float = 0;

	ret = rknn_outputs_get(backend->ctx, backend->output_count,
			       outputs.data(), nullptr);
	if (ret != RKNN_SUCC)
		return -EIO;

	for (auto &out : outputs) {
		size_t n = std::min(static_cast<size_t>(out.size),
				    output_len - copied);

		if (n)
			memcpy(static_cast<uint8_t *>(output) + copied,
			       out.buf, n);
		copied += n;

		if (copied == output_len)
			break;
	}

	rknn_outputs_release(backend->ctx, backend->output_count,
			     outputs.data());

	*actual_output_len = copied;
	return copied ? 0 : -ENOSPC;
}
