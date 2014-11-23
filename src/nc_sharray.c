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

#include <stdlib.h>
#include <sys/mman.h>
#include <nc_core.h>

struct sharray *
sharray_create(uint32_t n, size_t size)
{
    struct sharray *a;

    ASSERT(n != 0 && size != 0);

    a = nc_alloc(sizeof(*a));
    if (a == NULL) {
        return NULL;
    }

    a->elem = (u_char *) mmap(NULL, n*size,
                                PROT_READ|PROT_WRITE,
                                MAP_ANON|MAP_SHARED, -1, 0); 
    if (a->elem == NULL) {
        nc_free(a);
        return NULL;
    }

    a->nelem = 0;
    a->size = size;
    a->nalloc = n;

    return a;
}

void
sharray_destroy(struct sharray *a)
{
    sharray_deinit(a);
    nc_free(a);
}

rstatus_t
sharray_init(struct sharray *a, uint32_t n, size_t size)
{
    ASSERT(n != 0 && size != 0);

    a->elem = (u_char *) mmap(NULL, n * size,
                                PROT_READ|PROT_WRITE,
                                MAP_ANON|MAP_SHARED, -1, 0); 
    //a->elem = nc_alloc(n * size);
    if (a->elem == NULL) {
        return NC_ENOMEM;
    }

    a->nelem = 0;
    a->size = size;
    a->nalloc = n;

    return NC_OK;
}

void
sharray_deinit(struct sharray *a)
{
    ASSERT(a->nelem == 0);

    if (a->elem != NULL) {
        munmap(a->elem,a->nalloc * a->size);
    }
}


void *
sharray_push(struct sharray *a)
{
    void *elem, *new;
    size_t size;

    if (a->nelem == a->nalloc) {
        //TODO realloc the mem
        return NC_ENOMEM;
    }

    elem = (uint8_t *)a->elem + a->size * a->nelem;
    a->nelem++;

    return elem;
}


void *
sharray_get(struct sharray *a, uint32_t idx)
{
    void *elem;

    ASSERT(a->nelem != 0);
    ASSERT(idx < a->nelem);

    elem = (uint8_t *)a->elem + (a->size * idx);

    return elem;
}

void *
sharray_pop(struct sharray *a)
{
    void *elem;

    ASSERT(a->nelem != 0);

    a->nelem--;
    elem = (uint8_t *)a->elem + a->size * a->nelem;

    return elem;
}

