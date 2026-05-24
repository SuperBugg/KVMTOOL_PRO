#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/vfio.h"

#include <linux/kernel.h>

#include <fcntl.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef ARM_PVTIME_BASE
#define VFIO_PLATFORM_MMIO_BASE	(ARM_PVTIME_BASE + ARM_PVTIME_SIZE)
#else
#define VFIO_PLATFORM_MMIO_BASE	KVM_VIRTIO_MMIO_AREA
#endif

static u64 vfio_platform_mmio_base = VFIO_PLATFORM_MMIO_BASE;

#define VFIO_PLATFORM_COMPATIBLE_MAX	4096
#define VFIO_PLATFORM_NODE_NAME_MAX	64

union vfio_irq_eventfd {
	struct vfio_irq_set	irq;
	u8 buffer[sizeof(struct vfio_irq_set) + sizeof(int)];
};

static void set_vfio_irq_eventfd_payload(union vfio_irq_eventfd *evfd, int fd)
{
	memcpy(&evfd->irq.data, &fd, sizeof(fd));
}

static int vfio_platform_read_compatible(struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;
	char path[PATH_MAX];
	char *compatible;
	ssize_t len;
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/of_node/compatible",
		       vdev->sysfs_path);
	if (ret < 0 || ret >= (int)sizeof(path))
		return -EINVAL;

	compatible = malloc(VFIO_PLATFORM_COMPATIBLE_MAX);
	if (!compatible)
		return -ENOMEM;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		goto err_free;
	}

	len = read(fd, compatible, VFIO_PLATFORM_COMPATIBLE_MAX);
	close(fd);
	if (len <= 0) {
		ret = len ? -errno : -EINVAL;
		goto err_free;
	}

	pdev->compatible = compatible;
	pdev->compatible_len = len;

	return 0;

err_free:
	free(compatible);
	return ret;
}

#ifdef CONFIG_HAS_LIBFDT
static void vfio_platform_generate_fdt(void *fdt,
				       struct device_header *dev_hdr,
				       fdt_irq_fn irq_fn)
{
	struct vfio_device *vdev = container_of(dev_hdr, struct vfio_device,
						dev_hdr);
	struct vfio_platform_device *pdev = &vdev->platform;
	struct vfio_region *region = &vdev->regions[pdev->fdt_region_index];
	const char *name = strchr(vdev->params->name, '.');
	char dev_name[VFIO_PLATFORM_NODE_NAME_MAX];
	u64 reg_prop[] = {
		cpu_to_fdt64(region->guest_phys_addr),
		cpu_to_fdt64(region->info.size),
	};

	name = name ? name + 1 : "vfio-platform";
	snprintf(dev_name, sizeof(dev_name), "%s@%llx", name,
		 region->guest_phys_addr);

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property(fdt, "compatible", pdev->compatible,
			  pdev->compatible_len));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property(fdt, "dma-coherent", NULL, 0));

	if (pdev->irq_fd >= 0)
		irq_fn(fdt, pdev->gsi + KVM_IRQ_OFFSET,
		       IRQ_TYPE_LEVEL_HIGH);

	_FDT(fdt_end_node(fdt));
}
#else
#define vfio_platform_generate_fdt	NULL
#endif

static void vfio_platform_unmap_regions(struct kvm *kvm,
					struct vfio_device *vdev)
{
	unsigned int i;

	for (i = 0; i < vdev->info.num_regions; i++) {
		if (!vdev->regions[i].vdev)
			continue;

		vfio_unmap_region(kvm, &vdev->regions[i]);
	}
}

static int vfio_platform_configure_regions(struct kvm *kvm,
					   struct vfio_device *vdev)
{
	u64 mmio_addr = vfio_platform_mmio_base;
	unsigned int i;
	int ret, mapped = 0;



	for (i = 0; i < vdev->info.num_regions; i++) {
		struct vfio_region *region = &vdev->regions[i];

		/*
			argsz: 我传进去的结构体大小
			index: 我要查询第几个 region
		*/
		region->info.argsz = sizeof(region->info);
		region->info.index = i;

		//问内核region的信息
		/*
			struct vfio_region_info {
			__u32	argsz;
			__u32	flags;
			......
			}

		flags:
			VFIO_REGION_INFO_FLAG_READ
			VFIO_REGION_INFO_FLAG_WRITE
			VFIO_REGION_INFO_FLAG_MMAP

		size:
			这个 region 多大，比如 0x10000

		offset:
			这个 region 在 VFIO device fd 里的偏移
		*/
		ret = ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO,
			    &region->info);

		if (ret) {
			vfio_dev_warn(vdev, "failed to get region %u info",
				      i);
			continue;
		}

		if (!region->info.size)
			continue;

		region->vdev = vdev;
		region->guest_phys_addr = mmio_addr;


		//填完region信息之后，再在guest里map MMIO内存
		ret = vfio_map_region(kvm, vdev, region);
		if (ret) {
			vfio_dev_err(vdev, "failed to map region %u", i);
			goto err_unmap_regions;
		}

		vfio_dev_info(vdev,
			      "mapped platform region %u at 0x%llx, size 0x%llx",
			      i, region->guest_phys_addr, region->info.size);

		if (!mapped)
			vdev->platform.fdt_region_index = i;
		mapped++;

		mmio_addr += ALIGN(region->info.size, PAGE_SIZE);
	}

	if (!mapped) {
		vfio_dev_err(vdev, "no mappable platform regions");
		return -ENODEV;
	}

	vfio_platform_mmio_base = mmio_addr;

	return 0;

err_unmap_regions:
	vfio_platform_unmap_regions(kvm, vdev);

	return ret;
}

static void vfio_platform_disable_irq(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;
	union vfio_irq_eventfd unmask;
	struct vfio_irq_set irq_set = {
		.argsz	= sizeof(irq_set),
		.flags	= VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index	= pdev->irq_info.index,
	};

	if (pdev->irq_fd < 0)
		return;

	ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);

	if (pdev->unmask_fd >= 0) {
		unmask.irq = (struct vfio_irq_set) {
			.argsz	= sizeof(unmask),
			.flags	= VFIO_IRQ_SET_DATA_EVENTFD |
				  VFIO_IRQ_SET_ACTION_UNMASK,
			.index	= pdev->irq_info.index,
			.start	= 0,
			.count	= 1,
		};
		set_vfio_irq_eventfd_payload(&unmask, -1);
		ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
		close(pdev->unmask_fd);
		pdev->unmask_fd = -1;
	}

	irq__del_irqfd(kvm, pdev->gsi, pdev->irq_fd);
	close(pdev->irq_fd);
	pdev->irq_fd = -1;
}

static int vfio_platform_init_irq(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;
	union vfio_irq_eventfd trigger;
	union vfio_irq_eventfd unmask;
	int irq_line, trigger_fd, unmask_fd = -1;
	int ret;

	pdev->irq_info = (struct vfio_irq_info) {
		.argsz	= sizeof(pdev->irq_info),
		.index	= 0,
	};

	ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &pdev->irq_info);
	if (ret || pdev->irq_info.count == 0) {
		vfio_dev_warn(vdev, "no platform IRQ reported by VFIO");
		return 0;
	}

	if (!(pdev->irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
		vfio_dev_err(vdev, "platform IRQ is not eventfd capable");
		return -EINVAL;
	}

	irq_line = irq__alloc_line();
	pdev->gsi = irq_line - KVM_IRQ_OFFSET;

	trigger_fd = eventfd(0, 0);
	if (trigger_fd < 0) {
		vfio_dev_err(vdev, "failed to create trigger eventfd");
		return trigger_fd;
	}

	if (pdev->irq_info.flags & VFIO_IRQ_INFO_MASKABLE) {
		unmask_fd = eventfd(0, 0);
		if (unmask_fd < 0) {
			vfio_dev_err(vdev, "failed to create unmask eventfd");
			ret = unmask_fd;
			goto err_close_trigger;
		}
	}

	ret = irq__add_irqfd(kvm, pdev->gsi, trigger_fd, unmask_fd);
	if (ret)
		goto err_close_unmask;

	trigger.irq = (struct vfio_irq_set) {
		.argsz	= sizeof(trigger),
		.flags	= VFIO_IRQ_SET_DATA_EVENTFD |
			  VFIO_IRQ_SET_ACTION_TRIGGER,
		.index	= pdev->irq_info.index,
		.start	= 0,
		.count	= 1,
	};
	set_vfio_irq_eventfd_payload(&trigger, trigger_fd);

	ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);
	if (ret) {
		vfio_dev_err(vdev, "failed to setup platform IRQ trigger");
		goto err_del_irqfd;
	}

	if (unmask_fd >= 0) {
		unmask.irq = (struct vfio_irq_set) {
			.argsz	= sizeof(unmask),
			.flags	= VFIO_IRQ_SET_DATA_EVENTFD |
				  VFIO_IRQ_SET_ACTION_UNMASK,
			.index	= pdev->irq_info.index,
			.start	= 0,
			.count	= 1,
		};
		set_vfio_irq_eventfd_payload(&unmask, unmask_fd);

		ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
		if (ret) {
			vfio_dev_err(vdev, "failed to setup platform IRQ unmask");
			goto err_unset_trigger;
		}
	}

	pdev->irq_fd = trigger_fd;
	pdev->unmask_fd = unmask_fd;

	vfio_dev_info(vdev, "mapped platform IRQ to guest IRQ %d",
		      irq_line);

	return 0;

err_unset_trigger:
	trigger.irq.flags = VFIO_IRQ_SET_DATA_NONE |
			    VFIO_IRQ_SET_ACTION_TRIGGER;
	trigger.irq.count = 0;
	ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);
err_del_irqfd:
	irq__del_irqfd(kvm, pdev->gsi, trigger_fd);
err_close_unmask:
	if (unmask_fd >= 0)
		close(unmask_fd);
err_close_trigger:
	close(trigger_fd);
	return ret;
}

int vfio_platform_setup_device(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	struct vfio_platform_device *pdev = &vdev->platform;

	pdev->irq_fd = -1;
	pdev->unmask_fd = -1;

	ret = vfio_platform_configure_regions(kvm, vdev);
	if (ret)
		return ret;

	ret = vfio_platform_init_irq(kvm, vdev);
	if (ret)
		goto err_unmap_regions;

	ret = vfio_platform_read_compatible(vdev);
	if (ret) {
		vfio_dev_err(vdev, "failed to read platform compatible");
		goto err_disable_irq;
	}

	vdev->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_MMIO,
		.data		= vfio_platform_generate_fdt,
	};

	/*
		把这个 VFIO platform NPU 作为一个 MMIO 设备注册到 kvmtool 的 MMIO device list
	*/
	ret = device__register(&vdev->dev_hdr);
	if (ret) {
		vfio_dev_err(vdev, "failed to register VFIO platform device");
		goto err_free_compatible;
	}

	return 0;

err_free_compatible:
	free(pdev->compatible);
	pdev->compatible = NULL;
err_disable_irq:
	vfio_platform_disable_irq(kvm, vdev);
err_unmap_regions:
	vfio_platform_unmap_regions(kvm, vdev);
	return ret;
}

void vfio_platform_teardown_device(struct kvm *kvm, struct vfio_device *vdev)
{
	vfio_platform_disable_irq(kvm, vdev);
	vfio_platform_unmap_regions(kvm, vdev);
	device__unregister(&vdev->dev_hdr);
	free(vdev->platform.compatible);
}
