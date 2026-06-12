#ifndef VIRTIO_NPU_RKNN_BACKEND_H
#define VIRTIO_NPU_RKNN_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct virtio_npu_rknn_backend;

int virtio_npu_rknn_backend_create(const char *model_path,
				   struct virtio_npu_rknn_backend **out);
void virtio_npu_rknn_backend_destroy(struct virtio_npu_rknn_backend *backend);

int virtio_npu_rknn_backend_infer_raw(struct virtio_npu_rknn_backend *backend,
				      const void *input, size_t input_len,
				      void *output, size_t output_len,
				      uint32_t *actual_output_len);
int virtio_npu_rknn_backend_create_from_buffer(const void *model_data, size_t model_len,
						struct virtio_npu_rknn_backend **out);

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_NPU_RKNN_BACKEND_H */
