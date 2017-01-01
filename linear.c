#include <math.h>
#include "vulkan-expers.h"

/*
 * Some [GLM][0] functions ported to C.
 *
 * With thanks to [shua][1], liberally 'borrowing' from his [linearAlg.c][2].
 *
 * [0]:https://github.com/g-truc/glm/tree/master/glm
 * [1]:https://github.com/shua
 * [2]:https://github.com/shua/jams/blob/master/ld26/linearAlg.c
 */

struct mat4 linear_identity = {{
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1
}};

static struct mat4 mult(struct mat4 *m1, struct mat4 *m2) {
	struct mat4 ret = linear_identity;
	unsigned int row, col, row_offset;

	for (row = 0, row_offset = row * 4; row < 4; ++row, row_offset = row * 4)
		for (col = 0; col < 4; ++col)
			ret.m[row_offset + col] =
				(m1->m[row_offset + 0] * m2->m[col + 0]) +
				(m1->m[row_offset + 1] * m2->m[col + 4]) +
				(m1->m[row_offset + 2] * m2->m[col + 8]) +
				(m1->m[row_offset + 3] * m2->m[col + 12]);

	return ret;
}

struct mat4 linear_perspective(float fovy, float aspect_ratio, float near_plane,
			float far_plane)
{
	struct mat4 ret = {{0}};
	float y_scale = (float)(1.0f / cos(fovy/2.0f));
	float x_scale = y_scale / aspect_ratio;
	float frustum_length = far_plane - near_plane;

	ret.m[0] = x_scale;
	ret.m[5] = y_scale;
	ret.m[10] = -((far_plane + near_plane) / frustum_length);
	ret.m[11] = -1.0f;
	ret.m[14] = -((2.0f * near_plane * far_plane) / frustum_length);

	return ret;
}

void linear_translate(struct mat4 *matrix, float x, float y, float z)
{
	struct mat4 res;
	struct mat4 trans = linear_identity;

	trans.m[12] = x;
	trans.m[13] = y;
	trans.m[14] = z;

	res = mult(matrix, &trans);

	memcpy(matrix->m, res.m, sizeof(matrix->m));
}

void linear_rotate_x(struct mat4 *matrix, float rads)
{
	struct mat4 res;
	struct mat4 rot = linear_identity;
	float sine = sin(rads);
	float cosine = cos(rads);

	rot.m[5] = cosine;
	rot.m[6] = -sine;
	rot.m[9] = sine;
	rot.m[10] = cosine;

	res = mult(matrix, &rot);

	memcpy(matrix->m, res.m, sizeof(matrix->m));
}

void linear_rotate_y(struct mat4 *matrix, float rads)
{
	struct mat4 res;
	struct mat4 rot = linear_identity;
	float sine = sin(rads);
	float cosine = cos(rads);

	rot.m[0] = cosine;
	rot.m[8] = sine;
	rot.m[2] = -sine;
	rot.m[10] = cosine;

	res = mult(matrix, &rot);

	memcpy(matrix->m, res.m, sizeof(matrix->m));
}

void linear_rotate_z(struct mat4 *matrix, float rads)
{
	struct mat4 res;
	struct mat4 rot = linear_identity;
	float sine = sin(rads);
	float cosine = cos(rads);

	rot.m[0] = cosine;
	rot.m[1] = -sine;
	rot.m[4] = sine;
	rot.m[5] = cosine;

	res = mult(matrix, &rot);

	memcpy(matrix->m, res.m, sizeof(matrix->m));
}
