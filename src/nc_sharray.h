/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _NC_SHARRAY_H_
#define _NC_SHARRAY_H_

#include <nc_core.h>


struct sharray {
    uint32_t nelem;  /* # element */
    void     *elem;  /* element */
    size_t   size;   /* element size */
    uint32_t nalloc; /* # allocated element */
};

#define null_sharray { 0, NULL, 0, 0 }

static inline void
sharray_null(struct sharray *a)
{
    a->nelem = 0;
    a->elem = NULL;
    a->size = 0;
    a->nalloc = 0;
}


static inline uint32_t
sharray_n(const struct sharray *a)
{
    return a->nelem;
}

struct sharray *sharray_create(uint32_t n, size_t size);
void sharray_destroy(struct sharray *a);
rstatus_t sharray_init(struct sharray *a, uint32_t n, size_t size);
void sharray_deinit(struct sharray *a);

void *sharray_push(struct sharray *a);
void *sharray_get(struct sharray *a, uint32_t idx);
void *sharray_pop(struct sharray *a);

#endif
