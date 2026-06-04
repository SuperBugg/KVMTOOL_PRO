#ifndef KVM__NPU_VIRTIO_H
#define KVM__NPU_VIRTIO_H

struct kvm;

int virtio_npu__init(struct kvm *kvm);
int virtio_npu__exit(struct kvm *kvm);

#endif /* KVM__NPU_VIRTIO_H */
