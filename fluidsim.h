#ifndef __fluidsim_h
#define __fluidsim_h

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define FLUIDSIM_VER 1

/* Output specified error message to stderr (suffixing newline.) */
#define err(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)

/* Output specified error message (suffixing newline) to stderr and exit. */
#define fatal(...) do { err(__VA_ARGS__); exit(EXIT_FAILURE); } while (0)

/* Determine size of static array. */
#define ARRAY_SIZE(__arr) (sizeof(__arr)/sizeof(__arr[0]))

/* Iterate through a static array. */
#define FOR_EACH(__i, __arr) for (__i = 0; __i < ARRAY_SIZE(__arr); __i++)

#define MIN(__x, __y) (__x <= __y ? __x : __y)

/* The vulkan queues we care about. */
#define QUEUE_MASK (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | \
	VK_QUEUE_TRANSFER_BIT)
/* Size guaranteed to contain all queue bits masked by QUEUE_MASK. */
#define QUEUE_MAX (QUEUE_MASK + 1)

#ifdef __linux__
#include <xcb/xcb.h>
struct window {
	xcb_connection_t *conn;
	xcb_window_t win;
	xcb_atom_t event_delete_win;

	uint16_t width, height;
};
#else
#error Only linux supported.
#endif

struct vulkan_device {
	VkPhysicalDevice physical;
	VkDevice logical;

	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceMemoryProperties memory_properties;

	VkPhysicalDeviceFeatures features, enabled_features;

	uint32_t queue_family_property_count;
	VkQueueFamilyProperties *queue_family_properties;
	/* We waste some space here, but it's not much. */
	int queue_index_by_flag[QUEUE_MAX];
	int present_queue_index;

	VkDeviceQueueCreateInfo *queue_create_infos;

	uint32_t extension_property_count;
	VkExtensionProperties *extension_properties;

	VkQueue queue;
	VkFormat format;
	VkCommandPool command_pool;
	VkCommandBuffer command_buffer;

	bool supports_blit;

	VkFormat colour_format;
	VkColorSpaceKHR colour_space;

	VkSurfaceKHR surface;

	VkSwapchainKHR swap_chain;

	uint32_t image_count;
	VkImage *images;
	VkImageView *views;
};

struct vulkan {
	struct window *win;
	struct vulkan_device device;

	VkApplicationInfo app_info;
	VkInstance instance;
};

enum fluidsim_event {
	FLUIDSIM_EVENT_NONE,
	FLUIDSIM_EVENT_QUIT
};

typedef uint64_t dyn_arr_t;

struct dyn_arr {
	dyn_arr_t *vec;

	size_t count, cap;
};

/* malloc, or if no memory is available raise a fatal error and exit. */
static inline void *must_malloc(size_t size)
{
	/* Prefer a calloc size zeroing doesn't take that long and is safer. */
	void *ret = calloc(size, 1);

	if (ret == NULL)
		fatal("Out of memory");

	return ret;
}

/* realloc, or if no memory is available raise a fatal error and exit. */
static inline void *must_realloc(void *arr, size_t size)
{
	void *ret = realloc(arr, size);

	if (ret == NULL)
		fatal("Out of memory");

	return ret;
}

/* helpers.c */
struct dyn_arr *dyn_make(void);
void dyn_push(struct dyn_arr *arr, dyn_arr_t val);
void dyn_destroy(struct dyn_arr *arr);

/* vulkan.c */
struct vulkan *vulkan_make(struct window *win);
void vulkan_destroy(struct vulkan *vulkan);

/* win_<target>.c */
void win_destroy(struct window *win);
enum fluidsim_event win_handle_events(struct window *win);
struct window *win_make(char *title, uint16_t width, uint16_t height);
void win_update(struct window *win);

#endif /* __fluidsim_h */
