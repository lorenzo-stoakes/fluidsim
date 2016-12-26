#include "fluidsim.h"

/* Extensions we want to enable. */
static const char *const extensions[] = {
	VK_KHR_SURFACE_EXTENSION_NAME,
	VK_KHR_XCB_SURFACE_EXTENSION_NAME
};

/* The queues we care about. */
static VkFlags queue_flags[] = {
	VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_TRANSFER_BIT
};
static char *queue_names[] = {
	"graphics queue", "compute queue", "transfer queue"
};

/* Acceptable colour depths. */
static VkFormat depths_by_preference[] = {
	VK_FORMAT_D32_SFLOAT_S8_UINT,
	VK_FORMAT_D32_SFLOAT,
	VK_FORMAT_D24_UNORM_S8_UINT,
	VK_FORMAT_D16_UNORM_S8_UINT,
	VK_FORMAT_D16_UNORM
};

/* Check if the VkResult is VK_SUCCESS, otherwise report error. */
static void check_err(char *name, VkResult err)
{
	char *msg;

	if (err == VK_SUCCESS || err == VK_INCOMPLETE)
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
	case VK_ERROR_SURFACE_LOST_KHR:
		msg = "surface lost";
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

	/*
	 * TODO: This information is kept around since it's not clear whether
	 * this needs to be kept in memory or not and rather than just free it
	 * and hope for the best I'm being cautious. Check if necessary.
	 */

	/* We return a pointer so we can clean this up later. */
	return queue_create_infos;
}

/* Get depth format from preferred list. */
static void get_depth_format(struct vulkan_device *device)
{
	uint32_t i;

	FOR_EACH(i, depths_by_preference) {
		VkFormatProperties properties;

		device->format = depths_by_preference[i];
		vkGetPhysicalDeviceFormatProperties(device->physical,
						device->format, &properties);

		if (properties.optimalTilingFeatures &
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			device->supports_blit =
				properties.optimalTilingFeatures &
				VK_FORMAT_FEATURE_BLIT_DST_BIT;

			return;
		}
	}

	fatal("Unable to find acceptable depth format :(");
}

/* Create device command pool. */
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

/* Cleanup setup command buffer if present. */
static void destroy_setup_command_buffer(struct vulkan_device *device)
{
	if (!device->setup_command_buffer)
		return;

	vkFreeCommandBuffers(device->logical, device->command_pool, 1,
			&device->setup_command_buffer);
	device->setup_command_buffer = VK_NULL_HANDLE;
}

/* Create/starts setup command buffer, cleaning up existing if present. */
static void create_start_setup_command_buffer(struct vulkan_device *device)
{
	VkCommandBufferAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = device->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	VkCommandBufferBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
	};

	destroy_setup_command_buffer(device);
	check_err("vkAllocateCommandBuffers",
		vkAllocateCommandBuffers(device->logical, &alloc_info,
					&device->setup_command_buffer));
	check_err("vkBeginCommandBuffer",
		vkBeginCommandBuffer(device->setup_command_buffer, &info));
}

/* Create draw command buffers. */
static void create_command_buffers(struct vulkan_device *device)
{
	VkCommandBufferAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = device->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = device->image_count
	};

	device->draw_command_buffers =
		must_realloc(device->draw_command_buffers,
			sizeof(VkCommandBuffer) * device->image_count);
	check_err("vkAllocateCommandBuffers",
		vkAllocateCommandBuffers(device->logical, &info,
					device->draw_command_buffers));
}

/* Clean up draw command buffers if they exist. */
static void destroy_command_buffers(struct vulkan_device *device)
{
	if (!device->draw_command_buffers)
		return;

	vkFreeCommandBuffers(device->logical, device->command_pool,
			device->image_count, device->draw_command_buffers);
	free(device->draw_command_buffers);
}


/* Determine the graphics queue that supports presentation. */
static void get_present_queue_index(struct vulkan_device *device)
{
	uint32_t i;
	VkBool32 supports_present;
	int gfx_index = device->queue_index_by_flag[VK_QUEUE_GRAPHICS_BIT];

	/* First, check if our graphics */
	vkGetPhysicalDeviceSurfaceSupportKHR(device->physical, gfx_index,
					device->surface, &supports_present);
	if (supports_present) {
		device->present_queue_index = gfx_index;
		return;
	}

	/*
	 * Our gfx queue doesn't support present - look for a separate present
	 * queue.
	 */
	for (i = 0; i < device->queue_family_property_count; i++) {
		VkQueueFamilyProperties *queue =
			&device->queue_family_properties[i];
		VkFlags flags = queue->queueFlags & QUEUE_MASK;

		if (i == (uint32_t)gfx_index ||
			!(flags & VK_QUEUE_GRAPHICS_BIT))
			continue;

		vkGetPhysicalDeviceSurfaceSupportKHR(device->physical, i,
						device->surface,
						&supports_present);

		if (supports_present) {
			device->present_queue_index = i;
			return;
		}
	}

	fatal("Unable to find a present device queue.");
}

/* Determine colour format to use for device. */
static void get_colour_format(struct vulkan_device *device)
{
	uint32_t format_count;
	VkSurfaceFormatKHR surface_format;

	check_err("vkGetPhysicalDeviceSurfaceFormatsKHR (count)",
		vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical,
						device->surface, &format_count,
						NULL));
	if (format_count == 0)
		fatal("Need at least 1 surface format.");
	/* We always take the 1st. */
	format_count = 1;
	check_err("vkGetPhysicalDeviceSurfaceFormatsKHR (enumerate)",
		vkGetPhysicalDeviceSurfaceFormatsKHR(device->physical,
						device->surface, &format_count,
						&surface_format));

	/* If none preferred, make an assumption. */
	device->colour_format =
		surface_format.format == VK_FORMAT_UNDEFINED
		? VK_FORMAT_B8G8R8A8_UNORM
		: surface_format.format;
	device->colour_space = surface_format.colorSpace;
}

/* Destroy current swapchain, if exists. */
static void destroy_swapchain(struct vulkan_device *device)
{
	uint32_t i;

	if (device->swap_chain == VK_NULL_HANDLE)
		return;

	for (i = 0; i < device->image_count; i++)
		vkDestroyImageView(device->logical, device->views[i], NULL);

	free(device->images);
	free(device->views);

	vkDestroySwapchainKHR(device->logical, device->swap_chain, NULL);
}

/* Create swapchain image/image views. */
static void create_swapchain_images(struct vulkan_device *device)
{
	uint32_t i;

	check_err("vkGetSwapchainImagesKHR (count)",
		vkGetSwapchainImagesKHR(device->logical, device->swap_chain,
					&device->image_count, NULL));
	if (device->image_count == 0)
		fatal("Unable to allocate images.");

	device->images = must_malloc(sizeof(VkImage) * device->image_count);
	device->views = must_malloc(sizeof(VkImageView) * device->image_count);

	check_err("vkGetSwapchainImagesKHR (enumerate)",
		vkGetSwapchainImagesKHR(device->logical, device->swap_chain,
					&device->image_count, device->images));

	for (i = 0; i < device->image_count; i++) {
		VkImageViewCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = device->images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = device->colour_format,
			.components = {
				VK_COMPONENT_SWIZZLE_R,
				VK_COMPONENT_SWIZZLE_G,
				VK_COMPONENT_SWIZZLE_B,
				VK_COMPONENT_SWIZZLE_A
			},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		check_err("vkCreateImageView",
			vkCreateImageView(device->logical, &info, NULL,
					&device->views[i]));
	}
}

/* Create a new swapchain, destroying the existing one if present. */
static void create_swapchain(struct vulkan *vulkan)
{
	VkSwapchainKHR new_swap_chain;
	VkSurfaceCapabilitiesKHR caps;
	uint16_t width, height;
	struct vulkan_device *device = &vulkan->device;
	struct window *win = vulkan->win;
	VkSwapchainCreateInfoKHR create_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = device->surface,
		.imageFormat = device->colour_format,
		.imageColorSpace = device->colour_space,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageArrayLayers = 1,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.clipped = VK_TRUE,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, /* vsync */
		.oldSwapchain = device->swap_chain
	};

	check_err("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical,
							device->surface, &caps));

	/* Fixup width/height as needed. */
	if (caps.currentExtent.width == (uint32_t)-1) {
		width = win->width;
		height = win->height;
	} else {
		width = caps.currentExtent.width;
		height = caps.currentExtent.height;

		win->width = width;
		win->height = height;
	}
	create_info.imageExtent.width = width;
	create_info.imageExtent.height = height;

	create_info.minImageCount = MIN(caps.minImageCount + 1,
					caps.maxImageCount);

	/* Prefer non-rotated transform. */
	if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	else
		create_info.preTransform = caps.currentTransform;

	if (device->supports_blit)
		create_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	check_err("vkCreateSwapchainKHR",
		vkCreateSwapchainKHR(device->logical, &create_info, NULL,
				&new_swap_chain));
	/* Clean up memory from existing swap chain if it exists. */
	destroy_swapchain(device);
	device->swap_chain = new_swap_chain;

	create_swapchain_images(device);
}

/* Setup our swapchain. */
static void setup_swapchain(struct vulkan *vulkan)
{
	struct vulkan_device *device = &vulkan->device;
	VkXcbSurfaceCreateInfoKHR create_info = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = vulkan->win->conn,
		.window = vulkan->win->win
	};

	check_err("vkCreateXcbSurfaceKHR",
		vkCreateXcbSurfaceKHR(vulkan->instance, &create_info, NULL,
				&device->surface));

	get_present_queue_index(device);
	get_colour_format(device);
	create_swapchain(vulkan);
}

/* Setup initial physical device structures. */
static void setup_phys_device(VkInstance instance, struct vulkan_device *device)
{
	uint32_t count, gpu_count = 0;
	check_err("vkEnumeratePhysicalDevices (count)",
		vkEnumeratePhysicalDevices(instance, &gpu_count, NULL));
	if (gpu_count < 1)
		fatal("Need at least 1 compatible GPU.");

	/* We only retrieve the 1st physical device. */
	gpu_count = 1;
	check_err("vkEnumeratePhysicalDevices (enumerate)",
		vkEnumeratePhysicalDevices(instance, &gpu_count,
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
}

/* Retrieve extension properties. */
static void get_extension_properties(struct vulkan_device *device)
{
	uint32_t count;

	check_err("vkEnumerateDeviceExtensionProperties (count)",
		vkEnumerateDeviceExtensionProperties(device->physical,
			NULL, &count, NULL));
	device->extension_property_count = count;
	device->extension_properties =
		must_malloc(sizeof(VkExtensionProperties) * count);
	check_err("vkEnumerateDeviceExtensionProperties (enumerate)",
		vkEnumerateDeviceExtensionProperties(device->physical,
			NULL, &count, device->extension_properties));
}

/* Populate device details contained within vulkan struct and setup device. */
static void setup_device(struct vulkan *vulkan)
{
	struct vulkan_device *device = &vulkan->device;
	VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		/* TODO: Default to not enabling any features, later enable? */
		.pEnabledFeatures = NULL
	};

	setup_phys_device(vulkan->instance, device);
	populate_queues(device);
	device->queue_create_infos =
		populate_device_queue_info(device, &create_info);

	get_extension_properties(device);

	check_err("vkCreateDevice",
		vkCreateDevice(device->physical, &create_info, NULL,
			&device->logical));

	vkGetDeviceQueue(device->logical,
			device->queue_index_by_flag[VK_QUEUE_GRAPHICS_BIT], 0,
			&device->queue);

	get_depth_format(device);

	create_command_pool(device);
	create_start_setup_command_buffer(device);
	setup_swapchain(vulkan);
	create_command_buffers(device);
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

	setup_device(ret);

	return ret;
}

/* Cleanup vulkan data structures. */
void vulkan_destroy(struct vulkan *vulkan)
{
	struct vulkan_device *device = &vulkan->device;

	if (vulkan == NULL)
		return;

	destroy_swapchain(device);

	if (device->surface)
		vkDestroySurfaceKHR(vulkan->instance, device->surface, NULL);

	destroy_command_buffers(device);
	destroy_setup_command_buffer(device);

	if (device->command_pool)
		vkDestroyCommandPool(device->logical,
				device->command_pool, NULL);

	if (device->logical)
		vkDestroyDevice(device->logical, NULL);

	free(device->queue_family_properties);
	free(device->extension_properties);
	free(device->queue_create_infos);
	free((char *)vulkan->app_info.pApplicationName);

	vkDestroyInstance(vulkan->instance, NULL);

	free(vulkan);
}
