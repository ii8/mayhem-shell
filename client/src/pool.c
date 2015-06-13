
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "pool.h"

struct pool {
	struct wl_shm_pool* shm_pool;
	struct wl_list buffers;
	char *name;
	int fd;
	int size;
};

static long pagesize;

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	assert(mybuf->flags & BUFFER_ACTIVE);
	mybuf->flags &= ~BUFFER_BUSY;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

struct buffer *buffer_create(struct pool *pool, int width, int height,
			     uint32_t format)
{
	struct buffer *buffer, *segment;
	int stride, size;

	stride = width * 4;
	size = stride * height;
	if(size % pagesize)
		size += pagesize - size % pagesize;

	wl_list_for_each(segment, &pool->buffers, link) {
		if(~segment->flags & BUFFER_ACTIVE && segment->size >= size) {
			if(segment->size == size) {
				buffer = segment;
			} else {
				buffer = calloc(1, sizeof *buffer);
				wl_list_insert(&segment->link, &buffer->link);
				buffer->offset = segment->offset;
				buffer->size = size;
				segment->offset += size;
				segment->size -= size;
			}
			goto derp;
		}
	}


	if(wl_list_empty(&pool->buffers)) {
		buffer = calloc(1, sizeof *buffer);
		buffer->flags |= BUFFER_FIRST;
		assert(buffer->offset == 0);
	} else {
		segment = wl_container_of(pool->buffers.next, segment, link);
		assert(segment->flags & BUFFER_LAST);
		if(~segment->flags & BUFFER_ACTIVE) {
			buffer = segment;
			pool->size += size - segment->size;
			buffer->size = size;
			goto grow;
		} else {
			buffer = calloc(1, sizeof *buffer);
			segment->flags &= ~BUFFER_LAST;
			buffer->offset = segment->offset + segment->size;
		}
	}

	wl_list_insert(&pool->buffers, &buffer->link);
	buffer->size = size;
	buffer->flags |= BUFFER_LAST;
	pool->size += size;

grow:
	ftruncate(pool->fd, pool->size);
	wl_shm_pool_resize(pool->shm_pool, pool->size);

derp:
	buffer->flags |= BUFFER_ACTIVE;
	buffer->addr = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, pool->fd, buffer->offset);
	if(buffer->addr == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		buffer->flags &= ~BUFFER_ACTIVE;
		return NULL;
	}

	buffer->buffer = wl_shm_pool_create_buffer(pool->shm_pool,
						   buffer->offset, width,
						   height, stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	return buffer;
}

void buffer_destroy(struct buffer* buffer)
{
	struct buffer *segment;

	assert(~buffer->flags & BUFFER_BUSY);
	assert(buffer->flags & BUFFER_ACTIVE);

	buffer->flags &= ~BUFFER_ACTIVE;
	wl_buffer_destroy(buffer->buffer);
	munmap(buffer->addr, buffer->size);

	if(buffer->flags & BUFFER_LAST)
		goto skip;

	segment = wl_container_of(buffer->link.prev, segment, link);
	if(~segment->flags & BUFFER_ACTIVE) {
		buffer->size += segment->size;
		buffer->flags |= segment->flags & BUFFER_LAST;
		wl_list_remove(&segment->link);
		free(segment);
	}

skip:
	if(buffer->flags & BUFFER_FIRST)
		return;

	segment = wl_container_of(buffer->link.next, segment, link);
	if(~segment->flags & BUFFER_ACTIVE) {
		segment->size += buffer->size;
		wl_list_remove(&buffer->link);
		free(buffer);
	}
}

struct pool *pool_create(struct wl_shm *shm, char *name, int initial_size)
{
	static unsigned seed = 0;
	struct pool *pool;
	int max = 100;
	int len;

	pool = malloc(sizeof *pool);
	if(pool == NULL)
		return NULL;

	srand(++seed);
	len = strlen(name);
	pool->name = malloc(len + 8);
	if(pool->name == NULL) {
		free(pool);
		return NULL;
	}
	strcpy(pool->name, name);

	/* Create file */
	do {
		errno = 0;
		sprintf(pool->name + len, "-%i", rand() % 1000000);
		pool->fd = shm_open(pool->name, O_RDWR | O_CREAT | O_EXCL, 0600);
	} while(errno == EEXIST && max--);

	if(pool->fd < 0) {
		fprintf(stderr, "Fail create pool: %s\n", strerror(errno));
		free(pool);
		return NULL;
	}

	/* Create pool */
	pagesize = sysconf(_SC_PAGESIZE);
	if(initial_size % pagesize)
		initial_size += pagesize - initial_size % pagesize;
	pool->size = initial_size;
	ftruncate(pool->fd, pool->size);
	pool->shm_pool = wl_shm_create_pool(shm, pool->fd, pool->size);
	wl_list_init(&pool->buffers);

	return pool;
}

void pool_destroy(struct pool* pool)
{
	struct buffer *segment, *tmp;

	wl_list_for_each_safe(segment, tmp, &pool->buffers, link) {
		if(segment->flags & BUFFER_ACTIVE)
			buffer_destroy(segment);
	}

	assert(pool->buffers.next == pool->buffers.prev);
	assert(pool->buffers.next->prev == &pool->buffers);

	segment = wl_container_of(pool->buffers.next, segment, link);
	free(segment);

	wl_shm_pool_destroy(pool->shm_pool);
	shm_unlink(pool->name);
	close(pool->fd);
	free(pool->name);
	free(pool);
}

