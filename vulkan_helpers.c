#include "fluidsim.h"

/* Check if the VkResult is VK_SUCCESS, otherwise report error. */
void check_err(char *name, VkResult err)
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

/*
 * Load shader binary from specified path (relative to binary directory) and
 * load into shader module.
 */
VkPipelineShaderStageCreateInfo vulkan_load_shader(struct vulkan_device *device,
	char *path, VkShaderStageFlagBits stage)
{
	VkShaderModule mod;
	size_t size;
	unsigned char *buf = read_file(path, &size);
	VkPipelineShaderStageCreateInfo ret = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = stage
	};
	VkShaderModuleCreateInfo mod_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = (uint32_t *)buf
	};

	check_err("vkCreateShaderModule",
		vkCreateShaderModule(device->logical, &mod_info, NULL, &mod));

	free(buf);

	ret.module = mod;

	return ret;
}
