
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include <cairo.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "pool.h"
#include "util.h"

struct pool {
	struct wl_shm_pool* shm_pool;
	struct wl_list buffers;
	char *name;
	int fd;
	int size;
};

static long pagesize;

struct buffer *_buffer_destroy(struct buffer* buffer);

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *buf = data;

	assert(buf->flags & BUFFER_ACTIVE);
	buf->flags &= ~BUFFER_BUSY;
	if(buf->flags & BUFFER_KILL) {
		buf = _buffer_destroy(buf);
		assert(buf);
		if(buf->link.next == buf->link.prev)
			free(buf);
	}
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

struct buffer *buffer_create(struct pool *pool, int width, int height,
			     uint32_t format)
{
	struct buffer *buffer, *segment;
	int stride, size;

	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	size = stride * height;
	if(size % pagesize)
		size += pagesize - size % pagesize;

	wl_list_for_each(segment, &pool->buffers, link) {
		if(~segment->flags & BUFFER_ACTIVE && segment->size >= size) {
			if(segment->size == size) {
				buffer = segment;
			} else {
				buffer = zalloc(sizeof *buffer);
				if(!buffer)
					return NULL;
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
		buffer = zalloc(sizeof *buffer);
		if(!buffer)
			return NULL;
		buffer->flags |= BUFFER_FIRST;
	} else {
		segment = wl_container_of(pool->buffers.next, segment, link);
		assert(segment->flags & BUFFER_LAST);
		if(~segment->flags & BUFFER_ACTIVE) {
			buffer = segment;
			pool->size += size - segment->size;
			buffer->size = size;
			goto grow;
		} else {
			buffer = zalloc(sizeof *buffer);
			if(!buffer)
				return NULL;
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
		/* TODO: remerge segments */
		return NULL;
	}

	buffer->buffer = wl_shm_pool_create_buffer(pool->shm_pool,
						   buffer->offset, width,
						   height, stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	return buffer;
}

struct buffer *_buffer_destroy(struct buffer* buffer)
{
	struct buffer *segment;

	assert(buffer->flags & BUFFER_ACTIVE);
	if(buffer->flags & BUFFER_BUSY) {
		buffer->flags |= BUFFER_KILL;
		return NULL;
	}

	buffer->flags &= ~BUFFER_ACTIVE;
	wl_buffer_destroy(buffer->buffer);
	if(munmap(buffer->addr, buffer->size) < 0)
		fprintf(stderr, "munmap error: %m\n");

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
		return buffer;

	segment = wl_container_of(buffer->link.next, segment, link);
	if(~segment->flags & BUFFER_ACTIVE) {
		segment->size += buffer->size;
		segment->flags |= buffer->flags & BUFFER_LAST;
		wl_list_remove(&buffer->link);
		free(buffer);
		return segment;
	}

	return buffer;
}

void buffer_destroy(struct buffer* buffer)
{
	_buffer_destroy(buffer);
}

struct pool *pool_create(struct wl_shm *shm, char *name, int initial_size)
{
	struct pool *pool;
	int max = 100;
	int len;

	pool = malloc(sizeof *pool);
	if(!pool)
		return NULL;

	srand(time(NULL));
	len = strlen(name);
	pool->name = malloc(len + 8);
	if(!pool->name)
		goto err_name;
	strcpy(pool->name, name);

	/* Create file */
	do {
		errno = 0;
		sprintf(pool->name + len, "-%i", rand() % 1000000);
		pool->fd = shm_open(pool->name, O_RDWR | O_CREAT | O_EXCL, 0600);
	} while(errno == EEXIST && max--);

	if(pool->fd < 0)
		goto err_file;

	/* Create pool */
	if(initial_size % pagesize)
		initial_size += pagesize - initial_size % pagesize;
	pool->size = initial_size;
	ftruncate(pool->fd, pool->size);
	pool->shm_pool = wl_shm_create_pool(shm, pool->fd, pool->size);
	wl_list_init(&pool->buffers);

	return pool;

err_file:
	free(pool->name);
err_name:
	free(pool);
	fprintf(stderr, "Failed to create pool: %s\n", strerror(errno));
	return NULL;
}

void pool_destroy(struct pool* pool)
{
	struct buffer *segment, *tmp;

	wl_list_for_each_safe(segment, tmp, &pool->buffers, link) {
		if(segment->flags & BUFFER_ACTIVE)
			buffer_destroy(segment);
	}

	/* at this point unreleased buffers may still exist.
	 * they will all be destroyed as the server releases them */

	if(pool->buffers.next == pool->buffers.prev) {
		segment = wl_container_of(pool->buffers.next, segment, link);
		if(segment->flags & BUFFER_ACTIVE)
			assert(segment->flags & BUFFER_BUSY);
		else
			free(segment);
	}

	wl_shm_pool_destroy(pool->shm_pool);
	shm_unlink(pool->name);
	close(pool->fd);
	free(pool->name);
	free(pool);
}

void pool_setup(void)
{
	pagesize = sysconf(_SC_PAGESIZE);
}
