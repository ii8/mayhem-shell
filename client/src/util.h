#ifndef UTIL_H
#define UTIL_H

static inline void *zalloc(size_t s)
{
	return calloc(1, s);
}

static inline void *xmalloc(size_t s)
{
	void *p = malloc(s);

	if(p == NULL)
		exit(EXIT_FAILURE);
	return p;
}

static inline void *xzalloc(size_t s)
{
	void *p = zalloc(s);

	if(p == NULL)
		exit(EXIT_FAILURE);
	return p;
}

#endif
