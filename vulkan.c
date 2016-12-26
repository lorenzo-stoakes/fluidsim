#include "fluidsim.h"

static const char *const extensions[] = {
	VK_KHR_SURFACE_EXTENSION_NAME,
	VK_KHR_XCB_SURFACE_EXTENSION_NAME
};

/* The queues we care about. */
static VkFlags queue_flags[] = {
	VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_TRANSFER_BIT
};

/* Acceptable colour depths. */
static VkFormat depths_by_preference[] = {
	VK_FORMAT_D32_SFLOAT_S8_UINT,
	VK_FORMAT_D32_SFLOAT,
	VK_FORMAT_D24_UNORM_S8_UINT,
	VK_FORMAT_D16_UNORM_S8_UINT,
	VK_FORMAT_D16_UNORM
};

static char *queue_names[] = {
	"graphics queue", "compute queue", "transfer queue"
};

/* Check if the VkResult is VK_SUCCESS, otherwise report error. */
static void check_err(char *name, VkResult err)
{
	char *msg;

	if (err == VK_SUCCESS)
		return;

	switch (err) {
	case VK_ERROR_OUT_OF_HOST_MEMORY:
		msg = "out of host memory";
		break;
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		msg = "out of device memory";
		break;
	case VK_ERROR_INITIALIZATION_FAILED:
		msg = "initialisation failed";
		break;
	case VK_ERROR_LAYER_NOT_PRESENT:
		msg = "layer not present";
		break;
	case VK_ERROR_EXTENSION_NOT_PRESENT:
		msg = "extension not present";
		break;
	case VK_ERROR_INCOMPATIBLE_DRIVER:
		msg = "incompatible driver";
		break;
	default:
		msg = "unknown error";
		break;
	}

	fatal("%s failed with %s (%d.)", name, msg, err);
}

/* Populate preferred queues for each queue type. */
static void populate_queues(struct vulkan_device *device)
{
	uint32_t i, j, assigned = 0;

	/* Default each index to -1 to indicate unassigned. */
	FOR_EACH(i, queue_flags) {
		VkFlags flag = queue_flags[i];

		device->queue_index_by_flag[flag] = -1;
	}

	for (i = 0; i < device->queue_family_property_count; i++) {
		VkQueueFamilyProperties *queue =
			&device->queue_family_properties[i];
		VkFlags flags = queue->queueFlags & QUEUE_MASK;

		FOR_EACH(j, queue_flags) {
			VkFlags flag = queue_flags[j];

			if (!(flags & flag))
				continue;

			if (device->queue_index_by_flag[flag] == -1)
				assigned++;
			/* If already assigned, prefer an exclusive queue. */
			else if (flags != flag)
				continue;

			device->queue_index_by_flag[flag] = (int)i;
		}
	}

	if (assigned != ARRAY_SIZE(queue_flags)) {
		/* Give some useful feedback if we can't assign all queues. */
		err("Couldn't populate queues:");
		FOR_EACH(i, queue_flags)
			if (device->queue_index_by_flag[queue_flags[i]] == -1)
				err("  Missing %s", queue_names[i]);
		fatal("Aborting.");
	}
}

/*
 * Populate pQueueCreateInfos field in specified VkDeviceCreateInfo
 * structure.
 */
static VkDeviceQueueCreateInfo *populate_device_queue_info(
	struct vulkan_device *device, VkDeviceCreateInfo *create_info)
{
	uint32_t i, count = device->queue_family_property_count;
	VkDeviceQueueCreateInfo *queue_create_infos =
		must_malloc(sizeof(VkDeviceQueueCreateInfo) * count);
	float priority = 0.0f;

	create_info->queueCreateInfoCount = count;
	for (i = 0; i < count; i++) {
		VkDeviceQueueCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = i,
			.queueCount = 1,
			.pQueuePriorities = &priority
		};

		queue_create_infos[i] = info;
	}
	create_info->pQueueCreateInfos = queue_create_infos;

	return queue_create_infos;
}

static void get_depth_format(struct vulkan_device *device)
{
	uint32_t i;

	FOR_EACH(i, depths_by_preference) {
		VkFormatProperties properties;

		device->format = depths_by_preference[i];
		vkGetPhysicalDeviceFormatProperties(device->physical,
						device->format, &properties);

		if (properties.optimalTilingFeatures &
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return;
	}

	fatal("Unable to find acceptable depth format :(");
}

/* Populate device details contained within vulkan struct. */
static void populate_device(struct vulkan *vulkan)
{
	uint32_t gpu_count = 0;
	uint32_t count;
	struct vulkan_device *device = &vulkan->device;
	VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		/* TODO: Default to not enabling any features, later enable? */
		.pEnabledFeatures = NULL
	};

	check_err("vkEnumeratePhysicalDevices (count)",
		vkEnumeratePhysicalDevices(vulkan->instance, &gpu_count, NULL));
	if (gpu_count < 1)
		fatal("Need at least 1 compatible GPU.");

	/* We only retrieve the 1st physical device. */
	gpu_count = 1;
	check_err("vkEnumeratePhysicalDevices (enumerate)",
		vkEnumeratePhysicalDevices(vulkan->instance, &gpu_count,
					&device->physical));

	vkGetPhysicalDeviceProperties(device->physical,
				&device->properties);
	vkGetPhysicalDeviceFeatures(device->physical,
				&device->features);
	vkGetPhysicalDeviceMemoryProperties(device->physical,
				&device->memory_properties);

	vkGetPhysicalDeviceQueueFamilyProperties(device->physical,
		&count, NULL);
	device->queue_family_property_count = count;
	device->queue_family_properties =
		must_malloc(sizeof(VkQueueFamilyProperties) * count);
	vkGetPhysicalDeviceQueueFamilyProperties(device->physical,
		&count, device->queue_family_properties);
	populate_queues(device);

	check_err("vkEnumerateDeviceExtensionProperties (count)",
		vkEnumerateDeviceExtensionProperties(device->physical,
			NULL, &count, NULL));
	device->extension_property_count = count;
	device->extension_properties =
		must_malloc(sizeof(VkExtensionProperties) * count);
	check_err("vkEnumerateDeviceExtensionProperties (enumerate)",
		vkEnumerateDeviceExtensionProperties(device->physical,
			NULL, &count, device->extension_properties));

	device->queue_create_infos =
		populate_device_queue_info(device, &create_info);

	check_err("vkCreateDevice",
		vkCreateDevice(device->physical, &create_info, NULL,
			&device->logical));

	vkGetDeviceQueue(device->logical,
			device->queue_index_by_flag[VK_QUEUE_GRAPHICS_BIT], 0,
			&device->queue);

	get_depth_format(device);
}

static void create_command_pool(struct vulkan_device *device)
{
	uint32_t graphics_index =
		device->queue_index_by_flag[VK_QUEUE_GRAPHICS_BIT];
	VkCommandPoolCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = graphics_index,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
	};

	check_err("vkCreateCommandPool",
		vkCreateCommandPool(device->logical, &create_info, NULL,
				&device->command_pool));
}

/* Set up vulkan using the specified window. */
struct vulkan *vulkan_make(struct window *win)
{
	struct vulkan *ret = must_malloc(sizeof(struct vulkan));
	char *name = strdup("fluidsim");
	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = name,
		.applicationVersion = FLUIDSIM_VER,
		.pEngineName = name,
		.engineVersion = FLUIDSIM_VER,
		.apiVersion = VK_API_VERSION_1_0
	};
	VkInstanceCreateInfo instance_create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.enabledExtensionCount = ARRAY_SIZE(extensions),
		.ppEnabledExtensionNames = extensions
	};

	ret->win = win;
	ret->app_info = app_info;

	instance_create_info.pApplicationInfo = &ret->app_info;

	check_err("vkCreateInstance",
		vkCreateInstance(&instance_create_info, NULL, &ret->instance));

	populate_device(ret);

	create_command_pool(&ret->device);

	return ret;
}

void vulkan_destroy(struct vulkan *vulkan)
{
	if (vulkan == NULL)
		return;

	if (vulkan->device.command_pool)
		vkDestroyCommandPool(vulkan->device.logical,
				vulkan->device.command_pool, NULL);

	if (vulkan->device.logical)
		vkDestroyDevice(vulkan->device.logical, NULL);

	free(vulkan->device.queue_family_properties);
	free(vulkan->device.extension_properties);
	free(vulkan->device.queue_create_infos);
	free((char *)vulkan->app_info.pApplicationName);

	vkDestroyInstance(vulkan->instance, NULL);

	free(vulkan);
}
