#include "fluidsim.h"

#define DEFAULT_CAP 8

/* Resize capacity of specified dynamic array. */
static void resize_cap(struct dyn_arr *arr, size_t cap)
{
	arr->vec = must_realloc(arr->vec, cap * sizeof(dyn_arr_t));
	arr->cap = cap;
}

/* Create a new dynamic array. */
struct dyn_arr *dyn_make(void)
{
	struct dyn_arr *ret = must_malloc(sizeof(struct dyn_arr));

	resize_cap(ret, DEFAULT_CAP);

	return ret;
}

/* Push a value into the dynamic array. */
void dyn_push(struct dyn_arr *arr, dyn_arr_t val)
{
	if (arr == NULL)
		return;

	if (arr->cap == arr->count)
		resize_cap(arr, arr->cap * 2);

	arr->vec[arr->count++] = val;
}

void dyn_destroy(struct dyn_arr *arr)
{
	if (arr == NULL)
		return;

	free(arr->vec);
	free(arr);
}
