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
#include <unistd.h>
#include <nc_core.h>
#include <nc_conf.h>
#include <nc_server.h>
#include <nc_proxy.h>
#include <sys/mman.h>

static uint32_t ctx_id; /* context generation */

static rstatus_t
core_calc_connections(struct context *ctx)
{
    int status;
    struct rlimit limit;

    status = getrlimit(RLIMIT_NOFILE, &limit);
    if (status < 0) {
        log_error("getrlimit failed: %s", strerror(errno));
        return NC_ERROR;
    }

    ctx->max_nfd = (uint32_t)limit.rlim_cur;
    ctx->max_ncconn = ctx->max_nfd - ctx->max_nsconn - RESERVED_FDS;
    log_debug(LOG_NOTICE, "max fds %"PRIu32" max client conns %"PRIu32" "
              "max server conns %"PRIu32"", ctx->max_nfd, ctx->max_ncconn,
              ctx->max_nsconn);

    return NC_OK;
}

static void print_array(struct array *arr)
{
    int i;
    struct conf_change_item *item = NULL;

    for(i = 0; i<array_n(arr); ++i){
        item = array_get(arr,i); 
        log_error("id=%d,item->start=%ld, end=%ld",i,item->start,item->end);
    }
}

static void 
conf_change_to_conf(struct conf *ccf, struct conf *cf)
{
    struct array *cfp_pool = &ccf->pool;
    uint32_t i, j, k, nelem;
    /* O(N^2) complicated */
    for(i = 0; i< array_n(cfp_pool); ++i){
        struct conf_pool * cfp = array_get(cfp_pool,i);

        for(j = 0; j < array_n(&cf->pool); ++j){
            struct conf_pool *p = array_get(&cf->pool,j);
            if (string_compare(&p->name, &cfp->name)!=0) {
               continue; 
            }

            if (cfp->change_list.nelem!=0){
                //print_array(&cfp->change_list);
                //p->change_list = cfp->change_list;
                for (k = 0, nelem = array_n(&cfp->change_list); k < nelem; ++k) {
                    struct conf_change_item *src_field = array_get(&cfp->change_list, k);
                    struct conf_change_item *dst_field = array_push(&p->change_list);
                    dst_field->start = src_field->start;
                    dst_field->end = src_field->end;
                    dst_field->from = src_field->from;
                    dst_field->to = src_field->to;
                    dst_field->valid = src_field->valid;
                
                }

                p->migrating = 1;
                log_debug(LOG_NOTICE, "p %.*s running in migrating mode",
                  p->name.len,p->name.data);

            }
            
        }

    }
}

static struct context *
core_ctx_create(struct instance *nci)
{
    rstatus_t status;
    struct context *ctx;

    ctx = nc_alloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->id = ++ctx_id;
    ctx->cf = NULL;
    ctx->stats = NULL;
    ctx->evb = NULL;
    array_null(&ctx->pool);
    ctx->max_timeout = nci->stats_interval;
    ctx->timeout = ctx->max_timeout;
    ctx->max_nfd = 0;
    ctx->max_ncconn = 0;
    ctx->max_nsconn = 0;

    ctx->worker_num = nci->worker_num;
    ctx->old_ctx = NULL;

    if (pipe(ctx->channel) < 0 ) {
        log_error("start pipeline error");
    }


//    status = nc_set_nonblocking(ctx->channel[0]);

    //close(ctx->channel[0]);
    //
    //if (status < 0) {
    //    log_error("pair 0 non block error");
    //    return NC_ERROR;
    //}
    //status = nc_set_nonblocking(ctx->channel[1]);
    
//    if (status < 0) {
//        log_error("pair 1 non block error");
//        return NC_ERROR;
//    }


    /* parse and create configuration */
    ctx->cf = conf_create(nci->conf_filename);
    if (ctx->cf == NULL) {
        nc_free(ctx);
        return NULL;
    }

    /* parse change_list config */
    struct conf *change_cf = conf_change_create(nci->conf_change_filename); 
    conf_change_to_conf(change_cf, ctx->cf);
    conf_destroy(change_cf);
     

    /* initialize server pool from configuration */
    status = server_pool_init(&ctx->pool, &ctx->cf->pool, ctx);
    if (status != NC_OK) {
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    /*
     * Get rlimit and calculate max client connections after we have
     * calculated max server connections
     */
    status = core_calc_connections(ctx);
    if (status != NC_OK) {
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    /* create stats per server pool */
    ctx->stats = stats_create(ctx, nci->stats_port, nci->stats_addr, nci->stats_interval,
                              nci->hostname, &ctx->pool);
    if (ctx->stats == NULL) {
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    /* initialize event handling for client, proxy and server */
    ctx->evb = event_base_create(EVENT_SIZE, &core_core);
    if (ctx->evb == NULL) {
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);

        nc_free(ctx);
        return NULL;
    }

    /* preconnect? servers in server pool */
    /*status = server_pool_preconnect(ctx);
    if (status != NC_OK) {
        server_pool_disconnect(ctx);
        event_base_destroy(ctx->evb);
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }*/


    /* initialize proxy per server pool */
    status = proxy_init(ctx);
    if (status != NC_OK) {
        server_pool_disconnect(ctx);
        event_base_destroy(ctx->evb);
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    ctx->current_process_slot = -1;
    ctx->processes = (struct nc_process *) mmap(NULL, ctx->worker_num * sizeof(struct nc_process),
                                PROT_READ|PROT_WRITE,
                                MAP_ANON|MAP_SHARED, -1, (size_t)0); 


    log_debug(LOG_VVERB, "created ctx %p id %"PRIu32"", ctx, ctx->id);

    pid_t pid; 
    int i = 0;
    for(i =0; i< ctx->worker_num; ++i){
        process_spawn(ctx,i);
    } 

    status = stats_start_aggregator(ctx);
    if (status != NC_OK) {
        log_error("error start agg");
    }
    return ctx;
}

static void
core_ctx_destroy(struct context *ctx)
{
    log_debug(LOG_VVERB, "destroy ctx %p id %"PRIu32"", ctx, ctx->id);
    proxy_deinit(ctx);
    server_pool_disconnect(ctx);
    event_base_destroy(ctx->evb);
    stats_destroy(ctx->stats);
    server_pool_deinit(&ctx->pool);
    conf_destroy(ctx->cf);

    munmap(ctx->processes, ctx->worker_num * sizeof(struct nc_process));
    nc_free(ctx);
}

struct context *
core_start(struct instance *nci)
{
    struct context *ctx;

    mbuf_init(nci);
    msg_init();
    conn_init();

    ctx = core_ctx_create(nci);
    if (ctx != NULL) {
        nci->ctx = ctx;
        return ctx;
    }


    conn_deinit();
    msg_deinit();
    mbuf_deinit();

    return NULL;
}

void
core_stop(struct context *ctx)
{
    conn_deinit();
    msg_deinit();
    mbuf_deinit();
    core_ctx_destroy(ctx);
}

static rstatus_t
core_recv(struct context *ctx, struct conn *conn)
{
    rstatus_t status;

    status = conn->recv(ctx, conn);
    if (status != NC_OK) {
        log_debug(LOG_INFO, "recv on %c %d failed: %s",
                  conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd,
                  strerror(errno));
    }

    return status;
}

static rstatus_t
core_send(struct context *ctx, struct conn *conn)
{
    rstatus_t status;

    status = conn->send(ctx, conn);
    if (status != NC_OK) {
        log_debug(LOG_INFO, "send on %c %d failed: status: %d errno: %d %s",
                  conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd,
                  status, errno, strerror(errno));
    }

    return status;
}

static void
core_close(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    char type, *addrstr;

    ASSERT(conn->sd > 0);

    if (conn->client) {
        type = 'c';
        addrstr = nc_unresolve_peer_desc(conn->sd);
    } else {
        type = conn->proxy ? 'p' : 's';
        addrstr = nc_unresolve_addr(conn->addr, conn->addrlen);
    }
    log_debug(LOG_NOTICE, "close %c %d '%s' on event %04"PRIX32" eof %d done "
              "%d rb %zu sb %zu%c %s", type, conn->sd, addrstr, conn->events,
              conn->eof, conn->done, conn->recv_bytes, conn->send_bytes,
              conn->err ? ':' : ' ', conn->err ? strerror(conn->err) : "");

    status = event_del_conn(ctx->processes[ctx->current_process_slot].evb, conn);
    if (status < 0) {
        log_warn("event del conn %c %d failed, ignored: %s",
                 type, conn->sd, strerror(errno));
    }

    conn->close(ctx, conn);
}

static void
core_error(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    char type = conn->client ? 'c' : (conn->proxy ? 'p' : 's');

    status = nc_get_soerror(conn->sd);
    if (status < 0) {
        log_warn("get soerr on %c %d failed, ignored: %s", type, conn->sd,
                  strerror(errno));
    }
    conn->err = errno;

    core_close(ctx, conn);
}

static void
core_timeout(struct context *ctx)
{
    for (;;) {
        struct msg *msg;
        struct conn *conn;
        int64_t now, then;

        msg = msg_tmo_min();
        if (msg == NULL) {
            ctx->timeout = ctx->max_timeout;
            return;
        }

        /* skip over req that are in-error or done */

        if (msg->error || msg->done) {
            msg_tmo_delete(msg);
            continue;
        }

        /*
         * timeout expired req and all the outstanding req on the timing
         * out server
         */

        conn = msg->tmo_rbe.data;
        then = msg->tmo_rbe.key;

        now = nc_msec_now();
        if (now < then) {
            int delta = (int)(then - now);
            ctx->timeout = MIN(delta, ctx->max_timeout);
            return;
        }

        log_debug(LOG_INFO, "req %"PRIu64" on s %d timedout", msg->id, conn->sd);

        msg_tmo_delete(msg);
        conn->err = ETIMEDOUT;

        core_close(ctx, conn);
    }
}

rstatus_t
core_core(void *arg, uint32_t events)
{
    rstatus_t status;
    struct conn *conn = arg;
    struct context *ctx;
    
    if (conn->dummy == 1){
        //log_error("dummpy");
        return process_read_channel(conn,events);
    }

    ctx  = conn_to_ctx(conn);

    log_debug(LOG_VVERB, "event %04"PRIX32" on %c %d", events,
              conn->client ? 'c' : (conn->proxy ? 'p' : 's'), conn->sd);

    conn->events = events;

    /* error takes precedence over read | write */
    if (events & EVENT_ERR) {
        core_error(ctx, conn);
        return NC_ERROR;
    }

    /* read takes precedence over write */
    if (events & EVENT_READ) {
        status = core_recv(ctx, conn);
        if (status != NC_OK || conn->done || conn->err) {
            core_close(ctx, conn);
            return NC_ERROR;
        }
    }

    if (events & EVENT_WRITE) {
        status = core_send(ctx, conn);
        if (status != NC_OK || conn->done || conn->err) {
            core_close(ctx, conn);
            return NC_ERROR;
        }
    }

    return NC_OK;
}

rstatus_t
core_loop(struct context *ctx)
{
    int nsd;

    struct nc_process *process = &ctx->processes[ctx->current_process_slot];
    nsd = event_wait(process->evb, ctx->timeout);
    if (nsd < 0) {
        return nsd;
    }

    core_timeout(ctx);

    stats_swap(ctx->stats);

    log_debug(LOG_NOTICE,"notify to child thread");

    return NC_OK;
}

static rstatus_t print(void *elem, void *data)
{
  log_error("debug print pid=%d, elem=%p ...........",getpid(),elem);
}


struct context *
core_ctx_update(struct context *old_ctx, struct instance *nci)
{
    rstatus_t   status;
    struct context *ctx;

    ctx = nc_alloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->id = ++ctx_id;
    ctx->cf = NULL;
    ctx->stats = NULL;
    ctx->evb = NULL;
    array_null(&ctx->pool);
    ctx->max_timeout = nci->stats_interval;
    ctx->timeout = ctx->max_timeout;
    ctx->max_nfd = 0;
    ctx->max_ncconn = 0;
    ctx->max_nsconn = 0;

    ctx->worker_num = nci->worker_num;

    ctx->old_ctx = old_ctx;

    if (pipe(ctx->channel) < 0 ) {
        log_error("start pipeline error");
    }

//    status = nc_set_nonblocking(ctx->channel[0]);


    ctx->cf = conf_create(nci->conf_filename);
    if (ctx->cf == NULL) {
        nc_free(ctx);
        return NULL;
    }

    /* parse change_list config */
    struct conf *change_cf = conf_change_create(nci->conf_change_filename); 
    conf_change_to_conf(change_cf, ctx->cf);
    conf_destroy(change_cf);

    status = server_pool_init(&ctx->pool, &ctx->cf->pool, ctx);
    if (status != NC_OK) {
        conf_destroy(ctx->cf);
        nc_free(ctx);
        log_error("server_pool_init failed");
        return NULL;
    }

    /*
     * Get rlimit and calculate max client connections after we have
     * calculated max server connections
     */
    status = core_calc_connections(ctx);
    if (status != NC_OK) {
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    /* create stats per server pool */
    ctx->stats = stats_create(ctx, nci->stats_port, nci->stats_addr, nci->stats_interval,
                              nci->hostname, &ctx->pool);
    if (ctx->stats == NULL) {
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }


    ctx->evb = event_base_create(EVENT_SIZE, &core_core);
    if (ctx->evb == NULL) {
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }


    status = proxy_init(ctx);
    if (status != NC_OK) {
        server_pool_disconnect(ctx);
        event_base_destroy(ctx->evb);
        stats_destroy(ctx->stats);
        server_pool_deinit(&ctx->pool);
        conf_destroy(ctx->cf);
        nc_free(ctx);
        return NULL;
    }

    ctx->current_process_slot = -1;
    ctx->processes = (struct nc_process *) mmap(NULL, ctx->worker_num * sizeof(struct nc_process),
                                PROT_READ|PROT_WRITE,
                                MAP_ANON|MAP_SHARED, -1, (size_t)0); 


    log_debug(LOG_VVERB, "created ctx %p id %"PRIu32"", ctx, ctx->id);

    core_ctx_destroy(old_ctx);
    ctx->old_ctx = NULL; 


    return ctx;
}
