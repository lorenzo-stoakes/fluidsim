#include "fluidsim.h"

/* Helper function to retrieve a layout's vulkan object. */
static inline struct vulkan *layout_vulkan(struct layout *ptr)
{
	return container_of(ptr, struct vulkan, layout);
}

/* Helper function to retrieve the device associated with a layout. */
static inline struct vulkan_device *layout_device(struct layout *ptr)
{
	struct vulkan *vulkan = layout_vulkan(ptr);

	return &vulkan->device;
}

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

/* Populate pQueueCreateInfos field in VkDeviceCreateInfo structure. */
static VkDeviceQueueCreateInfo *populate_queue_info(struct vulkan_device *device,
						VkDeviceCreateInfo *create_info)
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

		device->depth_format = depths_by_preference[i];
		vkGetPhysicalDeviceFormatProperties(device->physical,
						device->depth_format, &properties);

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

/* Setup fence sync primitives. */
static void setup_fences(struct vulkan_device *device)
{
	uint32_t i;
	VkFenceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};

	device->fences = must_realloc(device->fences,
				sizeof(VkFence) * device->image_count);

	for (i = 0; i < device->image_count; i++)
		check_err("vkCreateFence",
			vkCreateFence(device->logical, &info, NULL, &device->fences[i]));

}

/* Clean up fence objects. */
static void destroy_fences(struct vulkan_device *device)
{
	uint32_t i;

	for (i = 0; i < device->image_count; i++)
		vkDestroyFence(device->logical, device->fences[i], NULL);

	free(device->fences);
}

/* Setup semaphores and submit info structure. */
static void setup_semaphores(struct vulkan_device *device)
{
	VkSemaphoreCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
	};
	VkSubmitInfo *submit_info = &device->submit_info;

	check_err("vkCreateSemaphore (present complete)",
		vkCreateSemaphore(device->logical, &info, NULL,
				&device->present_complete));
	check_err("vkCreateSemaphore (render complete)",
		vkCreateSemaphore(device->logical, &info, NULL,
				&device->render_complete));

	submit_info->sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info->pWaitDstStageMask = &device->gfx_pipeline_stage_wait;

	submit_info->waitSemaphoreCount = 1;
	submit_info->pWaitSemaphores = &device->present_complete;

	submit_info->signalSemaphoreCount = 1;
	submit_info->pSignalSemaphores = &device->render_complete;
}

/* Clean up semaphore structures. */
static void destroy_semaphores(struct vulkan_device *device)
{
	/* Assume one missing = all missing. */
	if (!device->present_complete)
		return;

	vkDestroySemaphore(device->logical, device->present_complete, NULL);
	vkDestroySemaphore(device->logical, device->render_complete, NULL);
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

	FREE_CLEAR(device->draw_command_buffers);
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

	FREE_CLEAR(device->images);
	FREE_CLEAR(device->views);

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

/* Create surface and link it to the window. */
static void create_link_surface(VkInstance instance, struct window *win,
				struct vulkan_device *device)
{
	VkXcbSurfaceCreateInfoKHR create_info = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = win->conn,
		.window = win->win
	};

	check_err("vkCreateXcbSurfaceKHR",
		vkCreateXcbSurfaceKHR(instance, &create_info, NULL,
				&device->surface));
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

/* Sets up and creates logical device with queue. */
static void setup_logical_device(struct vulkan_device *device)
{
	VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		/* TODO: Default to not enabling any features, later enable? */
		.pEnabledFeatures = NULL
	};

	device->queue_create_infos = populate_queue_info(device, &create_info);

	check_err("vkCreateDevice",
		vkCreateDevice(device->physical, &create_info, NULL,
			&device->logical));

	vkGetDeviceQueue(device->logical,
			device->queue_index_by_flag[VK_QUEUE_GRAPHICS_BIT], 0,
			&device->queue);
}

/* Create and setup new vulkan instance. */
static void create_instance(struct vulkan *vulkan)
{
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

	vulkan->app_info = app_info;
	instance_create_info.pApplicationInfo = &vulkan->app_info;

	check_err("vkCreateInstance",
		vkCreateInstance(&instance_create_info, NULL, &vulkan->instance));
}

/* Determine memory type index that fulfills property_mask, or -1 if not found. */
static int get_memory_type_index(struct vulkan_device *device, uint32_t type_mask,
			VkMemoryPropertyFlags property_mask)
{
	uint32_t i;
	VkPhysicalDeviceMemoryProperties properties = device->memory_properties;

	for (i = 0; i < properties.memoryTypeCount; i++, type_mask >>= 1) {
		VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;

		if ((type_mask & 1) && (flags & property_mask) == property_mask)
			return i;
	}

	return (uint32_t)-1;
}

/* Setup device depth stencil. */
static void setup_depth_stencil(struct window *win,
				struct vulkan_device *device)
{
	VkMemoryRequirements mem_reqs;
	struct depth_stencil *stencil = &device->depth_stencil;
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = device->depth_format,
		.extent = { win->width, win->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	};
	VkMemoryAllocateInfo mem_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	};
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = device->depth_format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};

	check_err("vkCreateImage",
		vkCreateImage(device->logical, &image_info, NULL, &stencil->image));
	view_info.image = stencil->image;

	vkGetImageMemoryRequirements(device->logical, stencil->image, &mem_reqs);
	mem_info.allocationSize = mem_reqs.size;
	mem_info.memoryTypeIndex = get_memory_type_index(device, mem_reqs.memoryTypeBits,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mem_info.memoryTypeIndex == (uint32_t)-1)
		fatal("Unable to get memory type for local bit property.");
	check_err("vkAllocateMemory",
		vkAllocateMemory(device->logical, &mem_info, NULL,
				&stencil->mem));
	check_err("vkBindImageMemory",
		vkBindImageMemory(device->logical, stencil->image,
				stencil->mem, 0));

	check_err("vkCreateImageView",
		vkCreateImageView(device->logical, &view_info, NULL, &stencil->view));
}

/* Cleanup deptch stencil data structures. */
static void destroy_depth_stencil(struct vulkan_device *device)
{
	struct depth_stencil *stencil = &device->depth_stencil;

	if (!stencil)
		return;

	vkDestroyImageView(device->logical, stencil->view, NULL);
	vkDestroyImage(device->logical, stencil->image, NULL);
	vkFreeMemory(device->logical, stencil->mem, NULL);
}

/* Create and setup render pass. */
static void setup_render_pass(struct vulkan_device *device)
{
	VkAttachmentDescription attachments[] = {
		{
			.format = device->colour_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		},
		{
			.format = device->depth_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		}
	};
	VkAttachmentReference colour_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	};
	VkAttachmentReference depth_ref = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colour_ref,
		.pDepthStencilAttachment = &depth_ref,
		.inputAttachmentCount = 0,
		.pInputAttachments = NULL,
		.preserveAttachmentCount = 0,
		.pPreserveAttachments = NULL,
		.pResolveAttachments = NULL
	};
	VkSubpassDependency dependencies[] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
		}
	};
	VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,

		.attachmentCount = ARRAY_SIZE(attachments),
		.pAttachments = attachments,

		.subpassCount = 1,
		.pSubpasses = &subpass,

		.dependencyCount = ARRAY_SIZE(dependencies),
		.pDependencies = dependencies
	};

	check_err("vkCreateRenderPass",
		vkCreateRenderPass(device->logical, &info, NULL, &device->render_pass));
}

/* Setup pipeline cache. */
static void setup_pipeline_cache(struct vulkan_device *device)
{
	VkPipelineCacheCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
	};

	check_err("vkCreatePipelineCache",
		vkCreatePipelineCache(device->logical, &info, NULL,
				&device->pipeline_cache));
}

/* Setup frame buffers for each swap chain image. */
static void setup_frame_buffers(struct window *win, struct vulkan_device *device)
{
	uint32_t i;
	VkImageView attachments[2];
	VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = device->render_pass,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.width = win->width,
		.height = win->height
	};

	device->frame_buffers = must_realloc(device->frame_buffers,
		sizeof(VkFramebuffer) * device->image_count);

	/* Always include depth stencil. */
	attachments[1] = device->depth_stencil.view;
	for (i = 0; i < device->image_count; i++) {
		attachments[0] = device->views[i];
		check_err("vkCreateFramebuffer",
			vkCreateFramebuffer(device->logical, &info, NULL,
					&device->frame_buffers[i]));
	}
}

/* Cleanup frame buffers. */
static void destroy_frame_buffers(struct vulkan_device *device)
{
	uint32_t i;

	for (i = 0; i < device->image_count; i++)
		vkDestroyFramebuffer(device->logical, device->frame_buffers[i],
				NULL);

	FREE_CLEAR(device->frame_buffers);
}

/* Flush the setup command buffer. */
static void flush_setup_command_buffer(struct vulkan_device *device)
{
	VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &device->setup_command_buffer
	};

	if (!device->setup_command_buffer)
		return;

	check_err("vkEndCommandBuffer",
		vkEndCommandBuffer(device->setup_command_buffer));
	check_err("vkQueueSubmit",
		vkQueueSubmit(device->queue, 1, &info, VK_NULL_HANDLE));
	check_err("vkQueueWaitIdle",
		vkQueueWaitIdle(device->queue));

	vkFreeCommandBuffers(device->logical, device->command_pool, 1,
			&device->setup_command_buffer);
	device->setup_command_buffer = VK_NULL_HANDLE;
}

/* Setup vertex buffer. */
static void setup_vertex_buffer(struct layout *layout)
{
	VkMemoryRequirements mem_reqs;
	void *ptr;
	struct vulkan_device *device = layout_device(layout);
	struct vertex local_vertex_buf[3] = {
		{  .pos = { 1.0f, 1.0f, 0.0f }, .colour = { 1.0f, 0.0f, 0.0f } },
		{  .pos = {-1.0f, 1.0f, 0.0f }, .colour = { 0.0f, 1.0f, 0.0f } },
		{  .pos = { 0.0f,-1.0f, 0.0f }, .colour = { 0.0f, 0.0f, 1.0f } },
	};
	struct vertices *vertices = &layout->vertices;
	size_t vertex_size = sizeof(local_vertex_buf);
	VkBufferCreateInfo src_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.size = vertex_size
	}, dst_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.size = vertex_size
	};
	VkMemoryAllocateInfo src_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	}, dst_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	};

	vertices->size = vertex_size;

	check_err("vkCreateBuffer (vertex source)",
		vkCreateBuffer(device->logical, &src_info, NULL,
			&vertices->staging.buf));

	vkGetBufferMemoryRequirements(device->logical, vertices->staging.buf,
				&mem_reqs);
	src_alloc_info.allocationSize = mem_reqs.size;
	/*
	 * Need it to be host visible so we can copy into it, also make
	 * coherent so device sees immediately.
	 */
	src_alloc_info.memoryTypeIndex = get_memory_type_index(device,
		mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check_err("vkAllocateMemory (vertex source)",
		vkAllocateMemory(device->logical, &src_alloc_info, NULL,
				&vertices->staging.mem));
	/* Map, copy in vertex data, unmap. */
	check_err("vkMapMemory (vertex source)",
		vkMapMemory(device->logical, vertices->staging.mem, 0,
			src_alloc_info.allocationSize, 0, &ptr));
	memcpy(ptr, local_vertex_buf, vertex_size);
	vkUnmapMemory(device->logical, vertices->staging.mem);
	/* Bind the buffer and memory. */
	check_err("vkBindBufferMemory (vertex source)",
		vkBindBufferMemory(device->logical, vertices->staging.buf,
				vertices->staging.mem, 0));

	/* Create target buffer. */
	check_err("vkCreateBuffer (vertex target)",
		vkCreateBuffer(device->logical, &dst_info, NULL,
			&vertices->buf));
	vkGetBufferMemoryRequirements(device->logical, vertices->buf,
				&mem_reqs);
	dst_alloc_info.allocationSize = mem_reqs.size;
	dst_alloc_info.memoryTypeIndex = get_memory_type_index(device,
		mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check_err("vkAllocateMemory (vertex target)",
		vkAllocateMemory(device->logical, &dst_alloc_info, NULL,
				&vertices->mem));
	check_err("vkBindBufferMemory (vertex target)",
		vkBindBufferMemory(device->logical, vertices->buf,
				vertices->mem, 0));
}

/* Setup index buffer. */
static void setup_index_buffer(struct layout *layout)
{
	VkMemoryRequirements mem_reqs;
	void *ptr;
	struct vulkan_device *device = layout_device(layout);
	uint32_t local_index_buf[3] = { 0, 1, 2 };
	struct indices *indices = &layout->indices;
	size_t index_size = sizeof(local_index_buf);
	VkBufferCreateInfo src_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.size = index_size
	}, dst_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.size = index_size
	};
	VkMemoryAllocateInfo src_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	}, dst_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	};

	indices->size = index_size;
	indices->count = ARRAY_SIZE(local_index_buf);

	/* Create index buffer. */
	check_err("vkCreateBuffer (index source)",
		vkCreateBuffer(device->logical, &src_info, NULL,
			&indices->staging.buf));
	vkGetBufferMemoryRequirements(device->logical, indices->staging.buf,
				&mem_reqs);
	src_alloc_info.allocationSize = mem_reqs.size;
	src_alloc_info.memoryTypeIndex = get_memory_type_index(device,
		mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check_err("vkAllocateMemory (index source)",
		vkAllocateMemory(device->logical, &src_alloc_info, NULL,
				&indices->staging.mem));
	/* Map, copy in index data, unmap. */
	check_err("vkMapMemory (index source)",
		vkMapMemory(device->logical, indices->staging.mem, 0,
			index_size, 0, &ptr));
	memcpy(ptr, local_index_buf, index_size);
	vkUnmapMemory(device->logical, indices->staging.mem);
	/* Bind the buffer and memory. */
	check_err("vkBindBufferMemory (index source)",
		vkBindBufferMemory(device->logical, indices->staging.buf,
				indices->staging.mem, 0));

	/* Create target buffer. */
	check_err("vkCreateBuffer (index target)",
		vkCreateBuffer(device->logical, &dst_info, NULL, &indices->buf));
	vkGetBufferMemoryRequirements(device->logical, indices->buf, &mem_reqs);
	dst_alloc_info.allocationSize = mem_reqs.size;
	dst_alloc_info.memoryTypeIndex = get_memory_type_index(device,
		mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check_err("vkAllocateMemory (index target)",
		vkAllocateMemory(device->logical, &dst_alloc_info, NULL,
				&indices->mem));
	check_err("vkBindBufferMemory (index target)",
		vkBindBufferMemory(device->logical, indices->buf, indices->mem, 0));
}

/* Cleanup staging layout data. */
static void destroy_staging_layout(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);

	vkDestroyBuffer(device->logical, layout->vertices.staging.buf, NULL);
	vkFreeMemory(device->logical, layout->vertices.staging.mem, NULL);

	vkDestroyBuffer(device->logical, layout->indices.staging.buf, NULL);
	vkFreeMemory(device->logical, layout->indices.staging.mem, NULL);
}

/* Allocate a command buffer */
static VkCommandBuffer alloc_command_buffer(struct vulkan_device *device)
{
	VkCommandBuffer ret;
	VkCommandBufferAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = device->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};

	check_err("vkAllocateCommandBuffers",
		vkAllocateCommandBuffers(device->logical, &info, &ret));

	return ret;
}

/* Wrapper around vkBeginCommandBuffer. */
static void begin_command_buffer(VkCommandBuffer buf)
{
	VkCommandBufferBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
	};

	check_err("vkBeginCommandBuffer",
		vkBeginCommandBuffer(buf, &info));
}

/* Terminate recording of command buffer and submit to queue. */
static void flush_command_buffer(struct vulkan_device *device,
				VkCommandBuffer buf)
{
	VkFence fence; /* So we know command buffer has finished. */
	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &buf
	};
	VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
	};

	if (buf == VK_NULL_HANDLE)
		fatal("NULL command buffer.");

	check_err("vkEndCommandBuffer", vkEndCommandBuffer(buf));

	check_err("vkCreateFence",
		vkCreateFence(device->logical, &fence_info, NULL, &fence));

	check_err("vkQueueSubmit",
		vkQueueSubmit(device->queue, 1, &submit_info, fence));
	/* Now wait for fence... */
	check_err("vkWaitForFences",
		vkWaitForFences(device->logical, 1, &fence, VK_TRUE,
				FENCE_TIMEOUT));

	vkDestroyFence(device->logical, fence, NULL);
	vkFreeCommandBuffers(device->logical, device->command_pool, 1, &buf);
}

/* Submit staging buffers. */
static void submit_staging(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);
	struct vertices *vertices = &layout->vertices;
	struct indices *indices = &layout->indices;
	VkCommandBuffer copy_cmd = alloc_command_buffer(device);
	VkBufferCopy vertex_copy_region = {
		.size = vertices->size
	}, index_copy_region = {
		.size = indices->size
	};

	begin_command_buffer(copy_cmd);

	vkCmdCopyBuffer(copy_cmd, vertices->staging.buf, vertices->buf, 1,
			&vertex_copy_region);
	vkCmdCopyBuffer(copy_cmd, indices->staging.buf, indices->buf, 1,
			&index_copy_region);

	flush_command_buffer(device, copy_cmd);

	destroy_staging_layout(layout);
}

/* Update uniform buffers to reflect camera angle. */
static void update_uniform_buffers(struct layout *layout)
{
	void *ptr;
	struct vulkan *vulkan = layout_vulkan(layout);
	struct window *win = vulkan->win;
	struct vulkan_device *device = layout_device(layout);
	struct uniform_buffer_object *ubo = &layout->ubo;
	size_t buf_size = sizeof(struct uniform_buffer_object);

	ubo->projection = linear_perspective(deg_to_rad(60.0f),
		(float)win->width / (float)win->height, 0.1f, 256.0f);

	ubo->model = linear_identity;
	linear_rotate_x(&ubo->model, layout->rot_x);
	linear_rotate_y(&ubo->model, layout->rot_y);
	linear_rotate_z(&ubo->model, layout->rot_z);

	ubo->view = linear_identity;
	linear_translate(&ubo->view, 0.0f, 0.0f, layout->zoom);

	/* Now copy to device. Coherent so should have immediate effect. */
	check_err("vkMapMemory",
		vkMapMemory(device->logical, layout->uniform_data.mem, 0,
			buf_size, 0, &ptr));
	memcpy(ptr, ubo, buf_size);
	vkUnmapMemory(device->logical, layout->uniform_data.mem);
}

/* Setup uniform buffers. */
static void setup_uniform_buffers(struct layout *layout)
{
	VkMemoryRequirements mem_reqs;
	struct vulkan_device *device = layout_device(layout);
	struct uniform_data *uniform_data = &layout->uniform_data;
	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = sizeof(struct uniform_buffer_object),
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
	};
	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
	};

	check_err("vkCreateBuffer",
		vkCreateBuffer(device->logical, &buffer_info, NULL,
			&uniform_data->buf));
	vkGetBufferMemoryRequirements(device->logical, uniform_data->buf,
				&mem_reqs);
	alloc_info.allocationSize = mem_reqs.size;
	/*
	 * We want something host-visible and instantly visible to the device
	 * (coherent.)
	 */
	alloc_info.memoryTypeIndex = get_memory_type_index(device,
		mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	check_err("vkAllocateMemory",
		vkAllocateMemory(device->logical, &alloc_info, NULL,
				&uniform_data->mem));
	check_err("vkBindBufferMemory",
		vkBindBufferMemory(device->logical, uniform_data->buf,
				uniform_data->mem, 0));

	/* Setup descriptor. */
	uniform_data->descriptor.buffer = uniform_data->buf;
	uniform_data->descriptor.offset = 0;
	uniform_data->descriptor.range = sizeof(struct uniform_buffer_object);

	update_uniform_buffers(layout);
}

/* Setup descriptor set layout. */
static void setup_descriptor_set_layout(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);
	VkDescriptorSetLayoutBinding layout_binding = {
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT
	};
	VkDescriptorSetLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &layout_binding
	};
	VkPipelineLayoutCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &layout->descriptor_set_layout
	};

	check_err("vkCreateDescriptorSetLayout",
		vkCreateDescriptorSetLayout(device->logical,
			&info, NULL, &layout->descriptor_set_layout));

	check_err("vkCreatePipelineLayout",
		vkCreatePipelineLayout(device->logical, &pipeline_info, NULL,
			&layout->pipeline_layout));
}

/* Setup pipelines - these describe how we are going to use the GPU. */
static void setup_pipelines(struct layout *layout)
{
	uint32_t i;
	struct vulkan_device *device = layout_device(layout);
	/* Describes how primitives are assembled. */
	VkPipelineInputAssemblyStateCreateInfo assembly_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	};
	VkPipelineRasterizationStateCreateInfo raster_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.depthBiasEnable = VK_FALSE,
		.lineWidth = 1.0f
	};
	/*
	 * Describes how blend factors are calculated, need 1 per colour
	 * attachment even if blending isn't used.
	 */
	VkPipelineColorBlendAttachmentState blend_attach_state[1] = {
		{
			.colorWriteMask = 0xf,
			.blendEnable = VK_FALSE
		}
	};
	VkPipelineColorBlendStateCreateInfo blend_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = blend_attach_state
	};
	/* Number of viewports/scissors used, overriden by dynamic states. */
	VkPipelineViewportStateCreateInfo viewport_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1
	};
	/*
	 * Dynamic states can be changed within a command buffer, we need to
	 * specify what we want to be able to change.
	 */
	VkDynamicState dynamic_state_enables[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamic_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.pDynamicStates = dynamic_state_enables,
		.dynamicStateCount = ARRAY_SIZE(dynamic_state_enables)
	};
	VkStencilOpState stencil_ops = {
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_ALWAYS
	};
	VkPipelineDepthStencilStateCreateInfo depth_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
		.back = stencil_ops,
		.front = stencil_ops
	};
	VkPipelineMultisampleStateCreateInfo multisample_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};
	VkPipelineShaderStageCreateInfo shader_infos[2] = {
		vulkan_load_shader(device, "shaders/1.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
		vulkan_load_shader(device, "shaders/1.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)
	};
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = layout->pipeline_layout,
		.renderPass = device->render_pass,
		.stageCount = ARRAY_SIZE(shader_infos),
		.pStages = shader_infos,
		.pInputAssemblyState = &assembly_info,
		.pRasterizationState = &raster_info,
		.pColorBlendState = &blend_info,
		.pMultisampleState = &multisample_info,
		.pViewportState = &viewport_info,
		.pDepthStencilState = &depth_info,
		.pDynamicState = &dynamic_info
	};

	/* Store so we can clean up later. */
	layout->module_count = ARRAY_SIZE(shader_infos);
	layout->modules = must_malloc(sizeof(VkShaderModule) * layout->module_count);
	for (i = 0; i < layout->module_count; i++)
		layout->modules[i] = shader_infos[i].module;

	check_err("vkCreateGraphicsPipelines",
		vkCreateGraphicsPipelines(device->logical,
			device->pipeline_cache, 1, &info, NULL,
			&layout->pipeline));
}

/* Setup descriptor pool. */
static void setup_descriptor_pool(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);
	VkDescriptorPoolSize type_counts[1] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1
		}
	};
	VkDescriptorPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.poolSizeCount = 1,
		.pPoolSizes = type_counts,
		.maxSets = 1
	};

	check_err("vkCreateDescriptorPool",
		vkCreateDescriptorPool(device->logical, &info, NULL,
			&layout->descriptor_pool));
}

/* Setup descriptor set. */
static void setup_descriptor_set(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);
	VkDescriptorSetAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = layout->descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &layout->descriptor_set_layout
	};
	VkWriteDescriptorSet write_set = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &layout->uniform_data.descriptor,
		.dstBinding = 0 /* Binds uniform buffer to binding point 0. */

	};

	check_err("vkAllocateDescriptorSets",
		vkAllocateDescriptorSets(device->logical, &info,
					&layout->descriptor_set));
	write_set.dstSet = layout->descriptor_set,

	vkUpdateDescriptorSets(device->logical, 1, &write_set, 0, NULL);
}


/* Clean up shader module data structures. */
static void destroy_shader_modules(struct layout *layout)
{
	uint32_t i;
	struct vulkan_device *device = layout_device(layout);

	for (i = 0; i < layout->module_count; i++) {
		vkDestroyShaderModule(device->logical, layout->modules[i], NULL);
	}

	free(layout->modules);
}

/* Cleanup descriptor pool data. */
static void destroy_descriptor_pool(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);

	if (layout->descriptor_pool == VK_NULL_HANDLE)
		return;

	vkDestroyDescriptorPool(device->logical, layout->descriptor_pool, NULL);

}

/* Cleanup scene layout data. */
static void destroy_layout(struct layout *layout)
{
	struct vulkan_device *device = layout_device(layout);

	destroy_descriptor_pool(layout);
	destroy_shader_modules(layout);

	vkDestroyPipeline(device->logical, layout->pipeline, NULL);

	vkDestroyBuffer(device->logical, layout->vertices.buf, NULL);
	vkFreeMemory(device->logical, layout->vertices.mem, NULL);

	vkDestroyBuffer(device->logical, layout->indices.buf, NULL);
	vkFreeMemory(device->logical, layout->indices.mem, NULL);

	vkDestroyBuffer(device->logical, layout->uniform_data.buf, NULL);
	vkFreeMemory(device->logical, layout->uniform_data.mem, NULL);

	vkDestroyPipelineLayout(device->logical,
				layout->pipeline_layout, NULL);
	vkDestroyDescriptorSetLayout(device->logical,
				layout->descriptor_set_layout, NULL);
}

/* Build command buffers for every framebuffer image. */
static void build_command_buffers(struct layout *layout)
{
	uint32_t i;
	struct vulkan *vulkan = layout_vulkan(layout);
	struct window *win = vulkan->win;
	struct vulkan_device *device = layout_device(layout);
	VkClearValue clear_vals[2] = {
		{ .color = { { 0.0f, 0.0f, 0.2f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0.0f } }
	};
	VkRenderPassBeginInfo pass_begin_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = device->render_pass,
		.renderArea = {
			.offset = { .x = 0, .y = 0 },
			.extent = { .width = win->width, .height = win->height }
		},
		.clearValueCount = 2,
		.pClearValues = clear_vals
	};

	for (i = 0; i < device->image_count; i++) {
		VkCommandBufferBeginInfo begin_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
		};
		VkViewport view_port = {
			.height = (float)win->height,
			.width = (float)win->width,
			.minDepth = 0.0f,
			.maxDepth = 1.0f
		};
		VkRect2D scissor = {
			.extent = { .width = win->width, .height = win->height },
			.offset = { .x = 0, .y = 0 }
		};
		VkCommandBuffer cmd_buf = device->draw_command_buffers[i];
		VkDeviceSize offsets[1] = { 0 };

		check_err("vkBeginCommandBuffer",
			vkBeginCommandBuffer(cmd_buf, &begin_info));

		pass_begin_info.framebuffer = device->frame_buffers[i];
		vkCmdBeginRenderPass(cmd_buf, &pass_begin_info,
				VK_SUBPASS_CONTENTS_INLINE);

		/* Update dynamic viewport. */
		vkCmdSetViewport(cmd_buf, 0, 1, &view_port);

		/* Update dynamic scissor. */
		vkCmdSetScissor(cmd_buf, 0, 1, &scissor);

		/* Bind descriptor sets describing shader binding points. */
		vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
			layout->pipeline_layout, 0, 1, &layout->descriptor_set,
			0, NULL);

		/* Bind rendering pipeline. */
		vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
				layout->pipeline);

		/* Bind triangle vertex buffer. */
		vkCmdBindVertexBuffers(cmd_buf, 0, 1, &layout->vertices.buf,
				offsets);

		vkCmdDrawIndexed(cmd_buf, layout->indices.count, 1, 0, 0, 1);

		vkCmdEndRenderPass(cmd_buf);

		/*
		 * Ending the render pass will add an implicit barrier
		 * transitioning the frame buffer color attachment to
		 * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR for presenting it to the
		 * windowing system.
		*/
		check_err("vkEndCommandBuffer",
			vkEndCommandBuffer(cmd_buf));
	}
}

/* Setup scene layout data. */
static void setup_layout(struct vulkan *vulkan)
{
	struct layout *layout = &vulkan->layout;

	layout->zoom = DEFAULT_ZOOM;

	setup_vertex_buffer(layout);
	setup_index_buffer(layout);
	submit_staging(layout);
	setup_uniform_buffers(layout);
	setup_descriptor_set_layout(layout);
	setup_pipelines(layout);
	setup_descriptor_pool(layout);
	setup_descriptor_set(layout);
	build_command_buffers(layout);
}

/* Setup our swapchain. */
static void setup_swapchain(struct vulkan *vulkan)
{
	struct vulkan_device *device = &vulkan->device;

	create_link_surface(vulkan->instance, vulkan->win, device);
	get_present_queue_index(device);
	get_colour_format(device);
	create_swapchain(vulkan);
}

/* Populate device details contained within vulkan struct and setup device. */
static void setup_device(struct vulkan *vulkan)
{
	struct vulkan_device *device = &vulkan->device;

	/* The pipeline stage we wait at for gfx queue submissions. */
	device->gfx_pipeline_stage_wait =
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	setup_phys_device(vulkan->instance, device);
	populate_queues(device);
	get_extension_properties(device);
	setup_logical_device(device);
	get_depth_format(device);
	setup_semaphores(device);
	create_command_pool(device);
	create_start_setup_command_buffer(device);
	setup_swapchain(vulkan);
	setup_fences(device);
	create_command_buffers(device);
	setup_depth_stencil(vulkan->win, device);
	setup_render_pass(device);
	setup_pipeline_cache(device);
	setup_frame_buffers(vulkan->win, device);
	flush_setup_command_buffer(device);
	/* TODO: Necessary? */
	create_start_setup_command_buffer(device);
}

/* Set up vulkan using the specified window. */
struct vulkan *vulkan_make(struct window *win)
{
	struct vulkan *ret = must_malloc(sizeof(struct vulkan));

	ret->win = win;
	create_instance(ret);
	setup_device(ret);
	setup_layout(ret);

	return ret;
}

/* Cleanup vulkan data structures. */
void vulkan_destroy(struct vulkan *vulkan)
{
	struct vulkan_device *device;

	if (vulkan == NULL)
		return;

	device = &vulkan->device;

	if (device->surface)
		vkDestroySurfaceKHR(vulkan->instance, device->surface, NULL);

	destroy_layout(&vulkan->layout);
	destroy_swapchain(device);
	destroy_command_buffers(device);
	destroy_setup_command_buffer(device);
	destroy_depth_stencil(device);
	destroy_semaphores(device);
	destroy_fences(device);
	destroy_frame_buffers(device);

	vkDestroyRenderPass(device->logical, device->render_pass, NULL);
	vkDestroyPipelineCache(device->logical, device->pipeline_cache, NULL);

	if (device->command_pool)
		vkDestroyCommandPool(device->logical,
				device->command_pool, NULL);

	free(device->queue_family_properties);
	free(device->extension_properties);
	free(device->queue_create_infos);
	free((char *)vulkan->app_info.pApplicationName);

	vkDestroyDevice(device->logical, NULL);
	vkDestroyInstance(vulkan->instance, NULL);

	free(vulkan);
}
