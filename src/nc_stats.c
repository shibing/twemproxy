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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include <nc_core.h>
#include <nc_server.h>
#include <nc_process.h>

struct stats_desc {
    char *name; /* stats name */
    char *desc; /* stats description */
};

#define DEFINE_ACTION(_name, _type, _desc) { .type = _type, .name = string(#_name) },
static struct stats_metric stats_pool_codec[] = {
    STATS_POOL_CODEC( DEFINE_ACTION )
};

static struct stats_metric stats_server_codec[] = {
    STATS_SERVER_CODEC( DEFINE_ACTION )
};


#undef DEFINE_ACTION

#define DEFINE_ACTION(_name, _type, _desc) { .name = #_name, .desc = _desc },
static struct stats_desc stats_pool_desc[] = {
    STATS_POOL_CODEC( DEFINE_ACTION )
};

static struct stats_desc stats_server_desc[] = {
    STATS_SERVER_CODEC( DEFINE_ACTION )
};
#undef DEFINE_ACTION

static void
_master_stats_server_set_by(struct context *ctx, struct stats_server *sts, struct stats_packet *sp);
static void
_master_stats_pool_set_by(struct context *ctx, struct stats_pool *pool,
                    struct stats_packet *sp);


void
stats_describe(void)
{
    uint32_t i;

    log_stderr("pool stats:");
    for (i = 0; i < NELEMS(stats_pool_desc); i++) {
        log_stderr("  %-20s\"%s\"", stats_pool_desc[i].name,
                   stats_pool_desc[i].desc);
    }

    log_stderr("");

    log_stderr("server stats:");
    for (i = 0; i < NELEMS(stats_server_desc); i++) {
        log_stderr("  %-20s\"%s\"", stats_server_desc[i].name,
                   stats_server_desc[i].desc);
    }
}

static void
stats_metric_init(struct stats_metric *stm)
{

    stm->plus_counter = 0LL;
    stm->minus_counter = 0LL;

    switch (stm->type) {
    case STATS_COUNTER:
        stm->value.counter = 0LL;
        break;

    case STATS_GAUGE:
        stm->value.counter = 0LL;
        break;

    case STATS_TIMESTAMP:
        stm->value.timestamp = 0LL;
        break;

    default:
        NOT_REACHED();
    }


}

static void
stats_metric_reset(struct sharray *stats_metric)
{
    uint32_t i, nmetric;

    nmetric = sharray_n(stats_metric);
    ASSERT(nmetric == STATS_POOL_NFIELD || nmetric == STATS_SERVER_NFIELD);

    for (i = 0; i < nmetric; i++) {
        struct stats_metric *stm = sharray_get(stats_metric, i);

        stats_metric_init(stm);
    }
}

static rstatus_t
stats_pool_metric_init(struct sharray *stats_metric)
{
    rstatus_t status;
    uint32_t i, nfield = STATS_POOL_NFIELD;

    status = sharray_init(stats_metric, nfield, sizeof(struct stats_metric));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nfield; i++) {
        struct stats_metric *stm = sharray_push(stats_metric);

        /* initialize from pool codec first */
        *stm = stats_pool_codec[i];

        /* initialize individual metric */
        stats_metric_init(stm);
    }

    return NC_OK;
}

static rstatus_t
stats_server_metric_init(struct stats_server *sts)
{
    rstatus_t status;
    uint32_t i, nfield = STATS_SERVER_NFIELD;

    status = sharray_init(&sts->metric, nfield, sizeof(struct stats_metric));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nfield; i++) {
        struct stats_metric *stm = sharray_push(&sts->metric);

        /* initialize from server codec first */
        *stm = stats_server_codec[i];

        /* initialize individual metric */
        stats_metric_init(stm);
    }

    return NC_OK;
}

static void
stats_metric_deinit(struct sharray *metric)
{
    uint32_t i, nmetric;

    nmetric = sharray_n(metric);
    for (i = 0; i < nmetric; i++) {
        sharray_pop(metric);
    }
    sharray_deinit(metric);
}

static rstatus_t
stats_server_init(struct stats_server *sts, struct server *s)
{
    rstatus_t status;

    sts->name = s->name;
    sharray_null(&sts->metric);

    status = stats_server_metric_init(sts);
    if (status != NC_OK) {
        return status;
    }

    log_debug(LOG_VVVERB, "init stats server '%.*s' with %"PRIu32" metric",
              sts->name.len, sts->name.data, sharray_n(&sts->metric));

    return NC_OK;

}

static rstatus_t
stats_server_map(struct array *stats_server, struct array *server)
{
    rstatus_t status;
    uint32_t i, nserver;

    nserver = array_n(server);
    ASSERT(nserver != 0);

    status = array_init(stats_server, nserver, sizeof(struct stats_server));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < nserver; i++) {
        struct server *s = array_get(server, i);
        struct stats_server *sts = array_push(stats_server);

        status = stats_server_init(sts, s);
        if (status != NC_OK) {
            return status;
        }
    }

    log_debug(LOG_VVVERB, "map %"PRIu32" stats servers", nserver);

    return NC_OK;
}

static void
stats_server_unmap(struct array *stats_server)
{
    uint32_t i, nserver;

    nserver = array_n(stats_server);

    for (i = 0; i < nserver; i++) {
        struct stats_server *sts = array_pop(stats_server);
        stats_metric_deinit(&sts->metric);
    }
    array_deinit(stats_server);

    log_debug(LOG_VVVERB, "unmap %"PRIu32" stats servers", nserver);
}

static rstatus_t
stats_pool_init(struct stats_pool *stp, struct server_pool *sp)
{
    rstatus_t status;

    stp->name = sp->name;
    sharray_null(&stp->metric);
    array_null(&stp->server);

    status = stats_pool_metric_init(&stp->metric);
    if (status != NC_OK) {
        return status;
    }

    status = stats_server_map(&stp->server, &sp->server);
    if (status != NC_OK) {
        stats_metric_deinit(&stp->metric);
        return status;
    }

    log_debug(LOG_VVVERB, "init stats pool '%.*s' with %"PRIu32" metric and "
              "%"PRIu32" server", stp->name.len, stp->name.data,
              sharray_n(&stp->metric), sharray_n(&stp->metric));

    return NC_OK;
}

static void
stats_pool_reset(struct array *stats_pool)
{
    uint32_t i, npool;

    npool = array_n(stats_pool);

    for (i = 0; i < npool; i++) {
        struct stats_pool *stp = array_get(stats_pool, i);
        uint32_t j, nserver;

        stats_metric_reset(&stp->metric);

        nserver = array_n(&stp->server);
        for (j = 0; j < nserver; j++) {
            struct stats_server *sts = array_get(&stp->server, j);
            stats_metric_reset(&sts->metric);
        }
    }
}

static rstatus_t
stats_pool_map(struct array *stats_pool, struct array *server_pool)
{
    rstatus_t status;
    uint32_t i, npool;

    npool = array_n(server_pool);
    ASSERT(npool != 0);

    status = array_init(stats_pool, npool, sizeof(struct stats_pool));
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < npool; i++) {
        struct server_pool *sp = array_get(server_pool, i);
        struct stats_pool *stp = array_push(stats_pool);

        status = stats_pool_init(stp, sp);
        if (status != NC_OK) {
            return status;
        }
    }

    log_debug(LOG_VVVERB, "map %"PRIu32" stats pools", npool);

    return NC_OK;
}

static void
stats_pool_unmap(struct array *stats_pool)
{
    uint32_t i, npool;

    npool = array_n(stats_pool);

    for (i = 0; i < npool; i++) {
        struct stats_pool *stp = array_pop(stats_pool);
        stats_metric_deinit(&stp->metric);
        stats_server_unmap(&stp->server);
    }
    array_deinit(stats_pool);

    log_debug(LOG_VVVERB, "unmap %"PRIu32" stats pool", npool);
}

static rstatus_t
stats_create_buf(struct stats *st)
{
    uint32_t int64_max_digits = 20; /* INT64_MAX = 9223372036854775807 */
    uint32_t key_value_extra = 8;   /* "key": "value", */
    uint32_t pool_extra = 8;        /* '"pool_name": { ' + ' }' */
    uint32_t server_extra = 8;      /* '"server_name": { ' + ' }' */
    size_t size = 0;
    uint32_t i;

    ASSERT(st->buf.data == NULL && st->buf.size == 0);

    /* header */
    size += 1;

    size += st->service_str.len;
    size += st->service.len;
    size += key_value_extra;

    size += st->source_str.len;
    size += st->source.len;
    size += key_value_extra;

    size += st->version_str.len;
    size += st->version.len;
    size += key_value_extra;

    size += st->uptime_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    size += st->timestamp_str.len;
    size += int64_max_digits;
    size += key_value_extra;

    /* server pools */
    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);
        uint32_t j;

        size += stp->name.len;
        size += pool_extra;

        for (j = 0; j < sharray_n(&stp->metric); j++) {
            struct stats_metric *stm = sharray_get(&stp->metric, j);

            size += stm->name.len;
            size += int64_max_digits;
            size += key_value_extra;
        }

        /* servers per pool */
        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);
            uint32_t k;

            size += sts->name.len;
            size += server_extra;

            for (k = 0; k < sharray_n(&sts->metric); k++) {
                struct stats_metric *stm = sharray_get(&sts->metric, k);

                size += stm->name.len;
                size += int64_max_digits;
                size += key_value_extra;
            }
        }
    }

    /* footer */
    size += 2;

    size = NC_ALIGN(size, NC_ALIGNMENT);

    st->buf.data = nc_alloc(size);
    if (st->buf.data == NULL) {
        log_error("create stats buffer of size %zu failed: %s", size,
                   strerror(errno));
        return NC_ENOMEM;
    }
    st->buf.size = size;

    log_debug(LOG_DEBUG, "stats buffer size %zu", size);

    return NC_OK;
}

static void
stats_destroy_buf(struct stats *st)
{
    if (st->buf.size != 0) {
        ASSERT(st->buf.data != NULL);
        nc_free(st->buf.data);
        st->buf.size = 0;
    }
}

static rstatus_t
stats_add_string(struct stats *st, struct string *key, struct string *val)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\":\"%.*s\", ", key->len, key->data,
                    val->len, val->data);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_add_num(struct stats *st, struct string *key, int64_t val)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\":%"PRId64", ", key->len, key->data,
                    val);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_add_header(struct stats *st)
{
    rstatus_t status;
    struct stats_buffer *buf;
    int64_t cur_ts, uptime;

    buf = &st->buf;
    buf->data[0] = '{';
    buf->len = 1;

    cur_ts = (int64_t)time(NULL);
    uptime = cur_ts - st->start_ts;

    status = stats_add_string(st, &st->service_str, &st->service);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_string(st, &st->source_str, &st->source);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_string(st, &st->version_str, &st->version);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->uptime_str, uptime);
    if (status != NC_OK) {
        return status;
    }

    status = stats_add_num(st, &st->timestamp_str, cur_ts);
    if (status != NC_OK) {
        return status;
    }

    return NC_OK;
}

static rstatus_t
stats_add_footer(struct stats *st)
{
    struct stats_buffer *buf;
    uint8_t *pos;

    buf = &st->buf;

    if (buf->len == buf->size) {
        return NC_ERROR;
    }

    /* overwrite the last byte and add a new byte */
    pos = buf->data + buf->len - 1;
    pos[0] = '}';
    pos[1] = '\n';
    buf->len += 1;

    return NC_OK;
}

static rstatus_t
stats_begin_nesting(struct stats *st, struct string *key)
{
    struct stats_buffer *buf;
    uint8_t *pos;
    size_t room;
    int n;

    buf = &st->buf;
    pos = buf->data + buf->len;
    room = buf->size - buf->len - 1;

    n = nc_snprintf(pos, room, "\"%.*s\": {", key->len, key->data);
    if (n < 0 || n >= (int)room) {
        return NC_ERROR;
    }

    buf->len += (size_t)n;

    return NC_OK;
}

static rstatus_t
stats_end_nesting(struct stats *st)
{
    struct stats_buffer *buf;
    uint8_t *pos;

    buf = &st->buf;
    pos = buf->data + buf->len;

    pos -= 2; /* go back by 2 bytes */

    switch (pos[0]) {
    case ',':
        /* overwrite last two bytes; len remains unchanged */
        ASSERT(pos[1] == ' ');
        pos[0] = '}';
        pos[1] = ',';
        break;

    case '}':
        if (buf->len == buf->size) {
            return NC_ERROR;
        }
        /* overwrite the last byte and add a new byte */
        ASSERT(pos[1] == ',');
        pos[1] = '}';
        pos[2] = ',';
        buf->len += 1;
        break;

    default:
        NOT_REACHED();
    }

    return NC_OK;
}

static rstatus_t
stats_copy_metric(struct stats *st, struct sharray *metric)
{
    rstatus_t status;
    uint32_t i;

    for (i = 0; i < sharray_n(metric); i++) {
        struct stats_metric *stm = sharray_get(metric, i);

        status = stats_add_num(st, &stm->name, stm->value.counter);
        if (status != NC_OK) {
            return status;
        }
    }

    return NC_OK;
}


static void
stats_aggregate_metric(struct sharray *dst, struct sharray *src)
{
    uint32_t i;

    for (i = 0; i < sharray_n(src); i++) {
        struct stats_metric *stm1, *stm2;

        stm1 = sharray_get(src, i);
        stm2 = sharray_get(dst, i);

        ASSERT(stm1->type == stm2->type);

        switch (stm1->type) {
        case STATS_COUNTER:
            stm2->value.counter += stm1->value.counter;
            stm2->plus_counter += stm1->plus_counter;
            stm2->minus_counter += stm1->minus_counter;
            break;

        case STATS_GAUGE:
            stm2->value.counter += stm1->value.counter;
            stm2->plus_counter += stm1->plus_counter;
            stm2->minus_counter += stm1->minus_counter;
            break;

        case STATS_TIMESTAMP:
            if (stm1->value.timestamp) {
                stm2->value.timestamp = stm1->value.timestamp;
            }
            break;

        default:
            NOT_REACHED();
        }
    }
}

static void
stats_aggregate(struct stats *st)
{
    uint32_t i;

    if (st->aggregate == 0) {
        log_error( "skip aggregate of shadow %p to sum %p as "
                  "generator is slow", st->shadow, st->sum);
        return;
    }

    log_debug(LOG_PVERB, "aggregate stats shadow %p to sum %p", st->shadow.elem,
              st->sum.elem);

    for (i = 0; i < array_n(&st->shadow); i++) {
        struct stats_pool *stp1, *stp2;
        uint32_t j;


        stp1 = array_get(&st->shadow, i);
        stp2 = array_get(&st->sum, i);
        stats_aggregate_metric(&stp2->metric, &stp1->metric);

        for (j = 0; j < array_n(&stp1->server); j++) {
            struct stats_server *sts1, *sts2;
            sts1 = array_get(&stp1->server, j);
            sts2 = array_get(&stp2->server, j);
            stats_aggregate_metric(&sts2->metric, &sts1->metric);
        }
    }

    st->aggregate = 0;
}

static rstatus_t
stats_make_rsp(struct stats *st)
{
    rstatus_t status;
    uint32_t i;

    status = stats_add_header(st);
    if (status != NC_OK) {
        return status;
    }

    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);
        uint32_t j;

        status = stats_begin_nesting(st, &stp->name);
        if (status != NC_OK) {
            return status;
        }

        /* copy pool metric from sum(c) to buffer */
        status = stats_copy_metric(st, &stp->metric);
        if (status != NC_OK) {
            return status;
        }

        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);

            status = stats_begin_nesting(st, &sts->name);
            if (status != NC_OK) {
                return status;
            }

            /* copy server metric from sum(c) to buffer */
            status = stats_copy_metric(st, &sts->metric);
            if (status != NC_OK) {
                return status;
            }

            status = stats_end_nesting(st);
            if (status != NC_OK) {
                return status;
            }
        }

        status = stats_end_nesting(st);
        if (status != NC_OK) {
            return status;
        }
    }

    status = stats_add_footer(st);
    if (status != NC_OK) {
        return status;
    }

    return NC_OK;
}

static rstatus_t
stats_send_rsp(struct stats *st)
{
    rstatus_t status;
    ssize_t n;
    int sd;

    status = stats_make_rsp(st);
    if (status != NC_OK) {
        return status;
    }

    sd = accept(st->sd, NULL, NULL);
    if (sd < 0) {
        log_error("accept on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    log_debug(LOG_VERB, "send stats on sd %d %d bytes", sd, st->buf.len);

    n = nc_sendn(sd, st->buf.data, st->buf.len);
    if (n < 0) {
        log_error("send stats on sd %d failed: %s", sd, strerror(errno));
        close(sd);
        return NC_ERROR;
    }

    close(sd);

    return NC_OK;
}


static uint32_t
make_stats_send_master(struct stats *st,struct stats_packet *st_packet_pool,struct array *st_packet_array)
{
    uint32_t i,j,k;


    //TODO init array 
    int total = 0; 
    for (i = 0; i < array_n(&st->sum); i++) {
        struct stats_pool *stp = array_get(&st->sum, i);

        for (j = 0; j < sharray_n(&stp->metric); j++) {
            struct stats_metric *stm = sharray_get(&stp->metric, j);
            struct stats_packet * sp = array_push(st_packet_array);
            sp->type = 0;
            sp->pidx = i;
            sp->fidx = j;
            sp->plus_counter = stm->plus_counter;
            sp->minus_counter = stm->minus_counter;

//            st_packet_pool[total].type = 0;
//            st_packet_pool[total].pidx = i;
//            st_packet_pool[total].fidx = j;
//            st_packet_pool[total].plus_counter = stm->plus_counter;
//            st_packet_pool[total].minus_counter = stm->minus_counter;
//            //nc_memcpy(&st_packet_pool[total].metric,stm,sizeof(struct stats_metric));
            total++;
            stats_metric_init(stm);

        }


        for (j = 0; j < array_n(&stp->server); j++) {
            struct stats_server *sts = array_get(&stp->server, j);

            for (k = 0; k < sharray_n(&sts->metric); k++) {
                struct stats_metric *stm = sharray_get(&sts->metric, k);
                struct stats_packet * sp = array_push(st_packet_array);
                sp->type = 1;
                sp->pidx = i;
                sp->sidx = j;
                sp->fidx = k;
                sp->plus_counter = stm->plus_counter;
                sp->minus_counter = stm->minus_counter;

//                st_packet_pool[total].type = 1;
//                st_packet_pool[total].pidx = i;
//                st_packet_pool[total].sidx = j;
//                st_packet_pool[total].fidx = k;
//                
////                log_error("sts metric %d",k);
//
//                st_packet_pool[total].plus_counter = stm->plus_counter;
//                st_packet_pool[total].minus_counter = stm->minus_counter;
                //nc_memcpy(&st_packet_pool[total].metric,stm,sizeof(struct stats_metric));
                total++;
                stats_metric_init(stm);

            }

        }

    }


    return total;
}

#define NC_STAT_BUFF_SIZE 2048

struct stats_packet full_stats_packet_pool[NC_MAX_PROCESSES][NC_STAT_BUFF_SIZE];
uint32_t full_stats_packet_pool_pos[NC_MAX_PROCESSES];

static void stats_send_master(struct context *ctx) {

    
    int ret;
 
    struct msghdr msghdr = {0}; 
    struct iovec iov[1];

//    struct stats_packet *send_data = nc_alloc(sizeof(struct stats_packet) *NC_STAT_BUFF_SIZE);
    struct stats_packet send_data[NC_STAT_BUFF_SIZE];
    struct array all_stats_data;
    array_init(&all_stats_data,NC_STAT_BUFF_SIZE,sizeof(struct stats_packet));
    uint32_t total = make_stats_send_master(ctx->stats,send_data,&all_stats_data);
    log_error("total = %d",total);


    int i = 0;
    while(i<total){
        int j = 0;
        for(j=0; j<NC_STAT_BUFF_SIZE - 1; ++j){
            send_data[j] = *(struct stats_packet *)array_get(&all_stats_data,i);
            ++i;
            if(i>=total){
                break;
            }
        }

        //send end sentinel element
        send_data[j].type = 2;

        iov[0].iov_base = (char *) send_data;
        iov[0].iov_len = sizeof(struct stats_packet) * NC_STAT_BUFF_SIZE;
        log_error("length = %d",iov[0].iov_len);
    
     
        msghdr.msg_name = NULL;
        msghdr.msg_namelen = 0;
        msghdr.msg_iov = iov;
        msghdr.msg_iovlen = 1;
     
        ret = sendmsg( nc_processes[nc_current_process_slot].channel[1], &msghdr, MSG_DONTWAIT );
        if( ret < 0 )
        {
            log_error( "sendmsg failed" );
        }

    }

 
 
}




static int receive_message( struct context *ctx ,int fd, int process_slot)
{
    uint32_t ret;
    int i;
    struct stats_packet stats_packet_pool[NC_STAT_BUFF_SIZE];
    uint32_t expect = sizeof(struct stats_packet) * NC_STAT_BUFF_SIZE; 
    struct msghdr msghdr = {0};
    struct iovec iov[1];
    iov[0].iov_base = stats_packet_pool;
    iov[0].iov_len =  expect;

 
    msghdr.msg_name = NULL;
    msghdr.msg_namelen = 0;
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = 1;


        ret = recvmsg( fd, &msghdr,0);
        if( ret < 0 )
        {
            log_error( "recvmsg failed" );
        }
     log_error( "ret=%d expect=%d",ret,expect );

    if(ret<expect){
    
       log_error( "small than expect errno = %d ret=%d",errno,ret );
    }

    nc_memcpy(((char *)full_stats_packet_pool[process_slot]) + full_stats_packet_pool_pos[process_slot], stats_packet_pool, ret);
    full_stats_packet_pool_pos[process_slot] += ret;


    if(full_stats_packet_pool_pos[process_slot] < expect){
        log_error( "xxxxxxxxx %d===%d",full_stats_packet_pool_pos[process_slot],expect );
        return full_stats_packet_pool_pos[process_slot];
    }
 
   log_error( "full_stats_packet_pool_size %d bytes",full_stats_packet_pool_pos[process_slot] );
   full_stats_packet_pool_pos[process_slot] = 0;

    struct stats *st = ctx->stats;
    for(i=0; i<NC_STAT_BUFF_SIZE; ++i){
//        struct stats_packet sp = stats_packet_pool[i]; 
        struct stats_packet sp = full_stats_packet_pool[process_slot][i]; 

            log_error("get stats_packet type=%d, pidx=%d,fidx=%d,plus_counter=%d,minous_counter=%d",sp.type,sp.pidx,sp.fidx,sp.plus_counter,sp.minus_counter);
        if(sp.type==2){
            break;
        }
        //TODO set the data to the mater process data
        
        if(sp.type==0){
                _master_stats_pool_set_by(ctx,array_get(&st->sum,sp.pidx),&sp);
        } 

        if(sp.type==1){
           struct stats_pool *stp = array_get(&st->sum, sp.pidx);
                _master_stats_server_set_by(ctx,array_get(&stp->server,sp.sidx),&sp);
        } 
    }
    return 0;
}


void *
stats_loop(void *arg)
{
    struct context *ctx = arg;
    struct stats *st = ctx->stats;
    int n;
    int i;
    int j;

    for (;;) {
        struct epoll_event events[1024];
        n = epoll_wait(st->ep, events, 1024, st->interval);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll wait on e %d with event m %d failed: %s",
                      st->ep, st->sd, strerror(errno));
            break;
        }

        for (i=0; i<n; ++i){
            if(events[i].data.fd == st->sd){
                //stats thread socket listening comming in
                /* aggregate stats from shadow (b) -> sum (c) */
                st->event = events[i];

                stats_aggregate(st);
                if (n == 0) {
                    continue;
                }
        
                /* send aggregate stats sum (c) to collector */
                stats_send_rsp(st);

            } else { //if( events[i].data.fd == ctx->channel[0] && events[i].events&EPOLLIN ){
                for(j = 0;j<NC_MAX_PROCESSES;++j){
                    if (nc_processes[j].channel[0] == events[i].data.fd){
                        break;
                    }
                }
                if (j>=NC_MAX_PROCESSES){
                    log_error("not find sd in process");
                }

                receive_message(ctx,events[i].data.fd,j);        
                log_error("channel 0 come in"); 
        
            }

            //log_error("event coming in %p",events[i].data.fd);

            

        }

    }

    return NULL;
}


static void *
stats_child_loop(void *arg)
{
    struct context *ctx = arg;

    for (;;) {
        //TODO send aggregate message here
        log_error("need send aggregate message here");
        
        stats_aggregate(ctx->stats);
        /* send aggregate stats sum (c) to master */
        stats_send_master(ctx);

        sleep(1);
    }

    return NULL;
}


static rstatus_t
stats_listen(struct stats *st)
{
    rstatus_t status;
    struct sockinfo si;

    status = nc_resolve(&st->addr, st->port, &si);
    if (status < 0) {
        return status;
    }

    st->sd = socket(si.family, SOCK_STREAM, 0);
    if (st->sd < 0) {
        log_error("socket failed: %s", strerror(errno));
        return NC_ERROR;
    }

    status = nc_set_reuseaddr(st->sd);
    if (status < 0) {
        log_error("set reuseaddr on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    status = bind(st->sd, (struct sockaddr *)&si.addr, si.addrlen);
    if (status < 0) {
        log_error("bind on m %d to addr '%.*s:%u' failed: %s", st->sd,
                  st->addr.len, st->addr.data, st->port, strerror(errno));
        return NC_ERROR;
    }

    status = listen(st->sd, SOMAXCONN);
    if (status < 0) {
        log_error("listen on m %d failed: %s", st->sd, strerror(errno));
        return NC_ERROR;
    }

    log_debug(LOG_NOTICE, "m %d listening on '%.*s:%u'", st->sd,
              st->addr.len, st->addr.data, st->port);

    return NC_OK;
}

static rstatus_t
stats_start_aggregator(struct context *ctx,struct stats *st)
{
    rstatus_t status;
    struct epoll_event ev;

    if (!stats_enabled) {
        return NC_OK;
    }

    status = stats_listen(st);
    if (status != NC_OK) {
        return status;
    }

    st->ep = epoll_create(10);
    if (st->ep < 0) {
        log_error("epoll create failed: %s", strerror(errno));
        return NC_ERROR;
    }

    ev.data.fd = st->sd;
    ev.events = EPOLLIN;

    status = epoll_ctl(st->ep, EPOLL_CTL_ADD, st->sd, &ev);
    if (status < 0) {
        log_error("epoll ctl on e %d sd %d failed: %s", st->ep, st->sd,
                  strerror(errno));
        return NC_ERROR;
    }



    status = pthread_create(&st->tid, NULL, stats_loop, ctx);
    if (status < 0) {
        log_error("stats aggregator create failed: %s", strerror(status));
        return NC_ERROR;
    }

    return NC_OK;
}



//we need send the stat message from child to master 
rstatus_t
stats_start_child_aggregator(struct context *ctx)
{
    rstatus_t status;

    status = pthread_create(&ctx->stats->tid, NULL, stats_child_loop, ctx);
    if (status < 0) {
        log_error("stats child aggregator create failed: %s", strerror(status));
        return NC_ERROR;
    }

    return NC_OK;
}


static void
stats_stop_aggregator(struct stats *st)
{
    if (!stats_enabled) {
        return;
    }

    close(st->sd);
    close(st->ep);
}

struct stats *
stats_create(struct context *ctx,uint16_t stats_port, char *stats_ip, int stats_interval,
             char *source, struct array *server_pool)
{
    rstatus_t status;
    struct stats *st;

    st = nc_alloc(sizeof(*st));
    if (st == NULL) {
        return NULL;
    }

    st->port = stats_port;
    st->interval = stats_interval;
    string_set_raw(&st->addr, stats_ip);

    st->start_ts = (int64_t)time(NULL);

    st->buf.len = 0;
    st->buf.data = NULL;
    st->buf.size = 0;

    array_null(&st->current);
    array_null(&st->shadow);
    array_null(&st->sum);

    st->tid = (pthread_t) -1;
    st->ep = -1;
    st->sd = -1;

    string_set_text(&st->service_str, "service");
    string_set_text(&st->service, "nutcracker");

    string_set_text(&st->source_str, "source");
    string_set_raw(&st->source, source);

    string_set_text(&st->version_str, "version");
    string_set_text(&st->version, NC_VERSION_STRING);

    string_set_text(&st->uptime_str, "uptime");
    string_set_text(&st->timestamp_str, "timestamp");

    st->updated = 0;
    st->aggregate = 0;

    /* map server pool to current (a), shadow (b) and sum (c) */

    status = stats_pool_map(&st->current, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->shadow, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_pool_map(&st->sum, server_pool);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_create_buf(st);
    if (status != NC_OK) {
        goto error;
    }

    status = stats_start_aggregator(ctx,st);
    if (status != NC_OK) {
        goto error;
    }

    return st;

error:
    stats_destroy(st);
    return NULL;
}

void
stats_destroy(struct stats *st)
{
    stats_stop_aggregator(st);
    stats_pool_unmap(&st->sum);
    stats_pool_unmap(&st->shadow);
    stats_pool_unmap(&st->current);
    stats_destroy_buf(st);
    nc_free(st);
}

void
stats_swap(struct stats *st)
{
    if (!stats_enabled) {
        return;
    }

    if (st->aggregate == 1) {
        log_debug(LOG_PVERB, "skip swap of current %p shadow %p as aggregator "
                  "is busy", st->current.elem, st->shadow.elem);
        return;
    }

    if (st->updated == 0) {
        log_debug(LOG_PVERB, "skip swap of current %p shadow %p as there is "
                  "nothing new", st->current.elem, st->shadow.elem);
        return;
    }

    log_debug(LOG_PVERB, "swap stats current %p shadow %p", st->current.elem,
              st->shadow.elem);

    array_swap(&st->current, &st->shadow);

    /*
     * Reset current (a) stats before giving it back to generator to keep
     * stats addition idempotent
     */
    stats_pool_reset(&st->current);
    st->updated = 0;
    st->aggregate = 1;
}

static struct stats_metric *
stats_pool_to_metric(struct context *ctx, struct server_pool *pool,
                     stats_pool_field_t fidx)
{
    struct stats *st;
    struct stats_pool *stp;
    struct stats_metric *stm;
    uint32_t pidx;

    pidx = pool->idx;

    st = ctx->stats;
    stp = array_get(&st->current, pidx);
    stm = sharray_get(&stp->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32"", stm->name.len,
              stm->name.data, pidx);

    return stm;
}

void
_stats_pool_incr(struct context *ctx, struct server_pool *pool,
                 stats_pool_field_t fidx)
{
    struct stats_metric *stm;

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter++;
    stm->plus_counter++;

    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_decr(struct context *ctx, struct server_pool *pool,
                 stats_pool_field_t fidx)
{
    struct stats_metric *stm;

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter--;
    stm->minus_counter--;

    log_debug(LOG_VVVERB, "decr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_incr_by(struct context *ctx, struct server_pool *pool,
                    stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;
    stm->plus_counter += val;

    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_pool_decr_by(struct context *ctx, struct server_pool *pool,
                    stats_pool_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_pool_to_metric(ctx, pool, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter -= val;
    stm->minus_counter -= val;

    log_debug(LOG_VVVERB, "decr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}




static struct stats_metric *
stats_server_to_metric(struct context *ctx, struct server *server,
                       stats_server_field_t fidx)
{
    struct stats *st;
    struct stats_pool *stp;
    struct stats_server *sts;
    struct stats_metric *stm;
    uint32_t pidx, sidx;

    sidx = server->idx;
    pidx = server->owner->idx;

    st = ctx->stats;
    stp = array_get(&st->current, pidx);
    sts = array_get(&stp->server, sidx);
    stm = sharray_get(&sts->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32" server %"PRIu32"",
              stm->name.len, stm->name.data, pidx, sidx);

    return stm;
}

void
_stats_server_incr(struct context *ctx, struct server *server,
                   stats_server_field_t fidx)
{
    struct stats_metric *stm;

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter++;
    stm->plus_counter++;

    log_error("stats_server_incr stm type=%d",stm->type);

    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64" %d", stm->name.len,
              stm->name.data, stm->value.counter,getpid());

//    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64"", stm->name.len,
//              stm->name.data, stm->value.counter);
}

void
_stats_server_decr(struct context *ctx, struct server *server,
                   stats_server_field_t fidx)
{
    struct stats_metric *stm;

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter--;
    stm->minus_counter--;

    log_debug(LOG_VVVERB, "decr field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}

void
_stats_server_incr_by(struct context *ctx, struct server *server,
                      stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
    stm->value.counter += val;
    stm->plus_counter += val;

    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64" %d", stm->name.len,
              stm->name.data, stm->value.counter,getpid());
//    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
 //             stm->name.data, stm->value.counter);
}

void
_stats_server_decr_by(struct context *ctx, struct server *server,
                      stats_server_field_t fidx, int64_t val)
{
    struct stats_metric *stm;

    stm = stats_server_to_metric(ctx, server, fidx);

    ASSERT(stm->type == STATS_GAUGE);
    stm->value.counter -= val;
    stm->minus_counter -= val;

    log_debug(LOG_VVVERB, "decr by field '%.*s' to %"PRId64"", stm->name.len,
              stm->name.data, stm->value.counter);
}




//master stats 
static struct stats_metric *
master_stats_server_to_metric(struct context *ctx, struct stats_server *sts,
                       stats_server_field_t fidx)
{
    struct stats *st;
    //struct stats_pool *stp;
    //struct stats_server *sts;
    struct stats_metric *stm;
    uint32_t pidx, sidx;

    //sidx = server->idx;
    //pidx = server->owner->idx;

    st = ctx->stats;
    //stp = array_get(&st->current, pidx);
    //sts = array_get(&stp->server, sidx);
    stm = sharray_get(&sts->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32" server %"PRIu32"",
              stm->name.len, stm->name.data, pidx, sidx);

    return stm;
}



static void
_master_stats_server_set_by(struct context *ctx, struct stats_server *sts,
                      struct stats_packet *sp)
{
    struct stats_metric *stm;

    stm = master_stats_server_to_metric(ctx, sts, sp->fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
//    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64" %d", stm->name.len,
//              stm->name.data, stm->value.counter,getpid());
    stm->value.counter += sp->plus_counter;
    stm->value.counter += sp->minus_counter;

//    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
 //             stm->name.data, stm->value.counter);
}


static struct stats_metric *
master_stats_pool_to_metric(struct context *ctx, struct stats_pool *pool,
                     stats_pool_field_t fidx)
{
    struct stats *st;
    //struct stats_pool *stp;
    struct stats_metric *stm;
//    uint32_t pidx;

    //pidx = pool->idx;

    st = ctx->stats;
//    stp = array_get(&st->current, pidx);
    stm = sharray_get(&pool->metric, fidx);

    st->updated = 1;

    log_debug(LOG_VVVERB, "metric '%.*s' in pool %"PRIu32"", stm->name.len,
              stm->name.data, pool);

    return stm;
}



static void
_master_stats_pool_set_by(struct context *ctx, struct stats_pool *pool,
                   struct stats_packet *sp)
{
    struct stats_metric *stm;

    stm = master_stats_pool_to_metric(ctx, pool, sp->fidx);

    ASSERT(stm->type == STATS_COUNTER || stm->type == STATS_GAUGE);
//    log_debug(LOG_VVVERB, "incr field '%.*s' to %"PRId64" %d", stm->name.len,
//              stm->name.data, stm->value.counter,getpid());
    stm->value.counter += sp->plus_counter;
    stm->value.counter += sp->minus_counter;

//    log_debug(LOG_VVVERB, "incr by field '%.*s' to %"PRId64"", stm->name.len,
 //             stm->name.data, stm->value.counter);
}



