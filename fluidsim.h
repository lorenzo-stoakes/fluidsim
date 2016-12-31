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

/* Find minimum of two values. */
#define MIN(__x, __y) (__x <= __y ? __x : __y)

/* Free pointer, then set to NULL. */
#define FREE_CLEAR(__ptr) do { free(__ptr); __ptr = NULL; } while(0)

/* Dodgy implementation of the kernel equivalents. */
#ifndef offsetof
#define offsetof(__type, __fieldname) ((size_t)(&((__type *)0)->__fieldname))
#endif
#define container_of(__ptr, __type, __fieldname) \
	((__type *)((unsigned char *)__ptr - offsetof(__type, __fieldname)))

/* The vulkan queues we care about. */
#define QUEUE_MASK (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | \
	VK_QUEUE_TRANSFER_BIT)
/* Size guaranteed to contain all queue bits masked by QUEUE_MASK. */
#define QUEUE_MAX (QUEUE_MASK + 1)
/* Number of nanoseconds in a second. */
#define SEC_NANO (1000000000UL)
/* How long we'll wait for a fence to timeout. */
#define FENCE_TIMEOUT (1UL * SEC_NANO)
/* Default zoom level. */
#define DEFAULT_ZOOM -2.5f

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

/* Attempt to replicate mat4 from GLM. */
struct mat4 {
	float m[16];
};

struct uniform_buffer_object {
	struct mat4 projection, model, view;
};

struct uniform_data {
	VkDeviceMemory mem;
	VkBuffer buf;
	VkDescriptorBufferInfo descriptor;
};

struct depth_stencil {
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
};

struct vertex {
	float pos[3];
	float colour[3];
};

struct staging_buffer {
	VkDeviceMemory mem;
	VkBuffer buf;
};

struct vertices {
	size_t size;

	VkDeviceMemory mem;
	VkBuffer buf;

	struct staging_buffer staging;
};

struct indices {
	size_t size;

	struct staging_buffer staging;

	VkDeviceMemory mem;
	VkBuffer buf;
};

struct layout {
	struct vertices vertices;
	struct indices indices;

	struct uniform_data uniform_data;
	struct uniform_buffer_object ubo;

	float zoom, rot_x, rot_y, rot_z;
};

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
	VkCommandPool command_pool;
	VkCommandBuffer setup_command_buffer, *draw_command_buffers;

	bool supports_blit;

	VkFormat colour_format;
	VkColorSpaceKHR colour_space;

	VkSurfaceKHR surface;

	VkSwapchainKHR swap_chain;

	uint32_t image_count;
	VkImage *images;
	VkImageView *views;

	VkFormat depth_format;
	struct depth_stencil depth_stencil;

	VkSemaphore present_complete, render_complete;
	VkSubmitInfo submit_info;
	VkFence *fences;

	VkPipelineStageFlags gfx_pipeline_stage_wait;

	VkRenderPass render_pass;

	VkPipelineCache pipeline_cache;

	VkFramebuffer *frame_buffers;
};

struct vulkan {
	struct window *win;
	struct vulkan_device device;

	VkApplicationInfo app_info;
	VkInstance instance;

	struct layout layout;
};

enum fluidsim_event {
	FLUIDSIM_EVENT_NONE,
	FLUIDSIM_EVENT_QUIT
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
float deg_to_rad(float deg);

/* linear.c */
extern struct mat4 linear_identity;
struct mat4 linear_perspective(float fovy, float aspect_ratio, float near_plane,
			float far_plane);
void linear_translate(struct mat4 *matrix, float x, float y, float z);
void linear_rotate_x(struct mat4 *matrix, float rads);
void linear_rotate_y(struct mat4 *matrix, float rads);
void linear_rotate_z(struct mat4 *matrix, float rads);

/* vulkan.c */
struct vulkan *vulkan_make(struct window *win);
void vulkan_destroy(struct vulkan *vulkan);

/* win_<target>.c */
void win_destroy(struct window *win);
enum fluidsim_event win_handle_events(struct window *win);
struct window *win_make(char *title, uint16_t width, uint16_t height);
void win_update(struct window *win);

#endif /* __fluidsim_h */
