#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/vfio.h"

#include <linux/kernel.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
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

#define VFIO_PLATFORM_PROP_MAX		4096
#define VFIO_PLATFORM_NODE_NAME_MAX	64

union vfio_irq_eventfd {
	struct vfio_irq_set	irq;
	u8 buffer[sizeof(struct vfio_irq_set) + sizeof(int)];
};

static void set_vfio_irq_eventfd_payload(union vfio_irq_eventfd *evfd, int fd)
{
	memcpy(&evfd->irq.data, &fd, sizeof(fd));
}

static int vfio_platform_read_of_property(struct vfio_device *vdev,
					  const char *property, char **data,
					  size_t *data_len, bool required)
{
	char path[PATH_MAX];
	char *buf;
	ssize_t len;
	int fd, ret;

	ret = snprintf(path, sizeof(path), "%s/of_node/%s", vdev->sysfs_path,
		       property);
	if (ret < 0 || ret >= (int)sizeof(path))
		return -EINVAL;

	buf = malloc(VFIO_PLATFORM_PROP_MAX);
	if (!buf)
		return -ENOMEM;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = required ? -errno : 0;
		goto err_free;
	}

	len = read(fd, buf, VFIO_PLATFORM_PROP_MAX);
	close(fd);
	if (len <= 0) {
		ret = required ? (len ? -errno : -EINVAL) : 0;
		goto err_free;
	}

	*data = buf;
	*data_len = len;

	return 0;

err_free:
	free(buf);
	return ret;
}

static int vfio_platform_read_compatible(struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;

	return vfio_platform_read_of_property(vdev, "compatible",
					      &pdev->compatible,
					      &pdev->compatible_len, true);
}

static int vfio_platform_read_irq_names(struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;

	return vfio_platform_read_of_property(vdev, "interrupt-names",
					      &pdev->irq_names,
					      &pdev->irq_names_len, false);
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
	u64 *reg_prop;
	u32 *irq_prop = NULL;
	unsigned int i, nr_regions = 0, nr_irqs = 0;

	(void)irq_fn;

	name = name ? name + 1 : "vfio-platform";
	snprintf(dev_name, sizeof(dev_name), "%s@%llx", name,
		 region->guest_phys_addr);

	for (i = 0; i < vdev->info.num_regions; i++) {
		if (vdev->regions[i].vdev)
			nr_regions++;
	}

	reg_prop = calloc(nr_regions * 2, sizeof(*reg_prop));
	if (!reg_prop)
		die_perror("calloc");

	nr_regions = 0;
	for (i = 0; i < vdev->info.num_regions; i++) {
		region = &vdev->regions[i];
		if (!region->vdev)
			continue;

		reg_prop[nr_regions * 2] =
			cpu_to_fdt64(region->guest_phys_addr);
		reg_prop[nr_regions * 2 + 1] =
			cpu_to_fdt64(region->info.size);
		nr_regions++;
	}

	for (i = 0; i < pdev->num_irqs; i++) {
		if (pdev->irqs[i].irq_fd >= 0)
			nr_irqs++;
	}

	if (nr_irqs) {
		irq_prop = calloc(nr_irqs * 3, sizeof(*irq_prop));
		if (!irq_prop)
			die_perror("calloc");

		nr_irqs = 0;
		for (i = 0; i < pdev->num_irqs; i++) {
			struct vfio_platform_irq *irq = &pdev->irqs[i];

			if (irq->irq_fd < 0)
				continue;

			irq_prop[nr_irqs * 3] = cpu_to_fdt32(0);
			irq_prop[nr_irqs * 3 + 1] =
				cpu_to_fdt32(irq->guest_irq - KVM_IRQ_OFFSET);
			irq_prop[nr_irqs * 3 + 2] =
				cpu_to_fdt32(IRQ_TYPE_LEVEL_HIGH);
			nr_irqs++;
		}
	}

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property(fdt, "compatible", pdev->compatible,
			  pdev->compatible_len));
	_FDT(fdt_property(fdt, "reg", reg_prop,
			  nr_regions * 2 * sizeof(*reg_prop)));
	_FDT(fdt_property(fdt, "dma-coherent", NULL, 0));

	if (nr_irqs) {
		_FDT(fdt_property(fdt, "interrupts", irq_prop,
				  nr_irqs * 3 * sizeof(*irq_prop)));

		if (pdev->irq_names)
			_FDT(fdt_property(fdt, "interrupt-names",
					  pdev->irq_names,
					  pdev->irq_names_len));
	}

	_FDT(fdt_end_node(fdt));

	free(irq_prop);
	free(reg_prop);
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
/*
	把 VFIO platform 设备暴露出来的每个 MMIO region，查询出来，
	然后分配一个 guest 物理地址，并映射进虚拟机地址空间。
	kvm:表示当前虚拟机实例。
	vdev:表示一个已经通过 VFIO 打开的设备，比如你的 NPU platform 设备。
*/
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
	unsigned int i;

	if (!pdev->irqs)
		return;

	for (i = 0; i < pdev->num_irqs; i++) {
		struct vfio_platform_irq *irq = &pdev->irqs[i];
		union vfio_irq_eventfd unmask;
		struct vfio_irq_set irq_set = {
			.argsz	= sizeof(irq_set),
			.flags	= VFIO_IRQ_SET_DATA_NONE |
				  VFIO_IRQ_SET_ACTION_TRIGGER,
			.index	= irq->info.index,
			.start	= 0,
			.count	= 0,
		};

		if (irq->irq_fd < 0)
			continue;

		ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);

		if (irq->unmask_fd >= 0) {
			unmask.irq = (struct vfio_irq_set) {
				.argsz	= sizeof(unmask),
				.flags	= VFIO_IRQ_SET_DATA_EVENTFD |
					  VFIO_IRQ_SET_ACTION_UNMASK,
				.index	= irq->info.index,
				.start	= 0,
				.count	= 1,
			};
			set_vfio_irq_eventfd_payload(&unmask, -1);
			ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
			close(irq->unmask_fd);
			irq->unmask_fd = -1;
		}

		if (irq->irqfd_added) {
			irq__del_irqfd(kvm, irq->gsi, irq->irq_fd);
			irq->irqfd_added = 0;
		}
		close(irq->irq_fd);
		irq->irq_fd = -1;
	}

	free(pdev->irqs);
	pdev->irqs = NULL;
	pdev->num_irqs = 0;
}


/*
irq->irq_fd = eventfd(0, 0)
        |
        +--> irq__add_irqfd()
        |      告诉 KVM：这个 fd 被 signal 时，向 guest 注入 GIC IRQ
        |
        +--> VFIO_DEVICE_SET_IRQS ACTION_TRIGGER
               告诉 VFIO：真实设备 IRQ 来了，signal 这个 fd
*/
static int vfio_platform_init_irq(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_platform_device *pdev = &vdev->platform;
	unsigned int i;
	int ret;

	pdev->num_irqs = vdev->info.num_irqs;
	if (!pdev->num_irqs)
		return 0;

	pdev->irqs = calloc(pdev->num_irqs, sizeof(*pdev->irqs));
	if (!pdev->irqs)
		return -ENOMEM;

	/*
		这里的框架和region是一样的，都是先获取region->info,再对每个region信息进行查询
		irq->info也是一样
	*/
	for (i = 0; i < pdev->num_irqs; i++) {
		struct vfio_platform_irq *irq = &pdev->irqs[i];
		union vfio_irq_eventfd trigger;
		union vfio_irq_eventfd unmask;
		int irq_line;

		irq->irq_fd = -1;
		irq->unmask_fd = -1;

		irq->info = (struct vfio_irq_info) {
			.argsz = sizeof(irq->info),
			.index = i,
		};

		ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq->info);
		if (ret) {
			vfio_dev_err(vdev,
				     "failed to get platform IRQ %u info: errno=%d (%s)",
				     i, errno, strerror(errno));
			continue;
		}

		vfio_dev_info(vdev,
			      "platform IRQ %u info flags=0x%x count=%u",
			      i, irq->info.flags, irq->info.count);

		if (irq->info.count == 0)
			continue;

		if (!(irq->info.flags & VFIO_IRQ_INFO_EVENTFD)) {
			vfio_dev_err(vdev, "platform IRQ %u is not eventfd capable", i);
			ret = -EINVAL;
			goto err_disable_irqs;
		}
		/*
			这里是分配guest的中断号
		*/
		irq_line = irq__alloc_line();
		irq->guest_irq = irq_line;
		irq->gsi = irq_line - KVM_IRQ_OFFSET;

		/*
			把真实硬件中断转换成 KVM guest 中断注入。KVM监听这个event
			KVM，你以后监听 irq->irq_fd。只要这个 fd 被 signal，就给 guest 注入 irq->gsi 这个中断。
		*/
		irq->irq_fd = eventfd(0, 0);
		if (irq->irq_fd < 0) {
			ret = -errno;
			vfio_dev_err(vdev,
				     "failed to create platform IRQ %u trigger eventfd: errno=%d (%s)",
				     i, errno, strerror(errno));
			goto err_disable_irqs;
		}

		if (irq->info.flags & VFIO_IRQ_INFO_MASKABLE) {
			irq->unmask_fd = eventfd(0, 0);
			if (irq->unmask_fd < 0) {
				ret = -errno;
				vfio_dev_err(vdev,
					     "failed to create platform IRQ %u unmask eventfd: errno=%d (%s)",
					     i, errno, strerror(errno));
				goto err_disable_irqs;
			}
		}

		ret = irq__add_irqfd(kvm, irq->gsi, irq->irq_fd,
				     irq->unmask_fd);
		if (ret) {
			vfio_dev_err(vdev,
				     "failed to add irqfd for platform IRQ %u guest IRQ %d: ret=%d",
				     i, irq_line, ret);
			goto err_disable_irqs;
		}
		irq->irqfd_added = 1;


		/*
			VFIO，你以后把 host 真实硬件 IRQ 绑定到 irq->irq_fd。
			真实 NPU IRQ 来了，就 signal 这个 eventfd。
		*/
		trigger.irq = (struct vfio_irq_set) {
			.argsz = sizeof(trigger),
			.flags = VFIO_IRQ_SET_DATA_EVENTFD |
				 VFIO_IRQ_SET_ACTION_TRIGGER,

			//这里就是绑定了硬件中断号和事件
			.index = irq->info.index,
			.start = 0,
			.count = 1,
		};
		set_vfio_irq_eventfd_payload(&trigger, irq->irq_fd);
		ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);
		if (ret) {
			ret = -errno;
			vfio_dev_err(vdev,
				     "failed to setup platform IRQ %u trigger: errno=%d (%s)",
				     i, errno, strerror(errno));
			goto err_disable_irqs;
		}

		if (irq->unmask_fd >= 0) {
			unmask.irq = (struct vfio_irq_set) {
				.argsz = sizeof(unmask),
				.flags = VFIO_IRQ_SET_DATA_EVENTFD |
					 VFIO_IRQ_SET_ACTION_UNMASK,
				.index = irq->info.index,
				.start = 0,
				.count = 1,
			};
			set_vfio_irq_eventfd_payload(&unmask, irq->unmask_fd);

			ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
			if (ret) {
				ret = -errno;
				vfio_dev_err(vdev,
					     "failed to setup platform IRQ %u unmask: errno=%d (%s)",
					     i, errno, strerror(errno));
				goto err_disable_irqs;
			}
		}

		vfio_dev_info(vdev, "mapped platform IRQ %u to guest IRQ %d",
			      i, irq_line);
	}

	return 0;

err_disable_irqs:
	vfio_platform_disable_irq(kvm, vdev);
	return ret;
}

int vfio_platform_setup_device(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	struct vfio_platform_device *pdev = &vdev->platform;

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

	ret = vfio_platform_read_irq_names(vdev);
	if (ret) {
		vfio_dev_err(vdev, "failed to read platform interrupt names");
		goto err_free_compatible;
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
	free(pdev->irq_names);
	pdev->irq_names = NULL;
	pdev->irq_names_len = 0;
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
	free(vdev->platform.irq_names);
	free(vdev->platform.compatible);
}
