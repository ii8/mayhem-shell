#ifndef POOL_H
#define POOL_H

#include <wayland-client-core.h>

struct pool;

enum buffer_state {
	BUFFER_ACTIVE	= 0x01,
	BUFFER_BUSY	= 0x02,
	BUFFER_FIRST	= 0x04,
	BUFFER_LAST	= 0x08
};

struct buffer {
	struct wl_list link;
	int offset;
	int size;
	enum buffer_state flags;
	struct wl_buffer *buffer;
	void *addr;
};

struct buffer *buffer_create(struct pool *pool, int width, int height,
			     uint32_t format);
void buffer_destroy(struct buffer* buffer);

struct pool *pool_create(struct wl_shm *shm, char *name,
			 int initial_size);
void pool_destroy(struct pool* pool);

#endif

