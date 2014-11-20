#include <stdlib.h>
#include <nc_core.h>
#include <nc_process.h>
#include <nc_event.h>
#include <nc_server.h>
int nc_current_process_slot = 0;


static rstatus_t proxy_each_add_conn(void *elem, void *data)
{
    rstatus_t status;
    struct server_pool *pool = elem;
    struct conn *p;
    p = pool->p_conn;

    nc_process_t *process = data;

    status = event_add_conn(process->ep, p);
    if (status < 0) {
        log_error("event add conn e %d p %d on addr '%.*s' failed: %s",
                  process->ep, p->sd, pool->addrstr.len, pool->addrstr.data,
                  strerror(errno));
        return NC_ERROR;
    }

    status = event_del_out(process->ep, p);
    if (status < 0) {
        log_error("event del out e %d p %d on addr '%.*s' failed: %s",
                  process->ep, p->sd, pool->addrstr.len, pool->addrstr.data,
                  strerror(errno));
        return NC_ERROR;

    }

    return NC_OK;
     

}

static void nc_process_init(struct context *ctx,nc_process_t *process){
    int status, ep;
    struct epoll_event *event;


    ep = epoll_create(EVENT_SIZE_HINT);
    if (ep < 0) {
        log_error("epoll create of size %d failed: %s", EVENT_SIZE_HINT, strerror(errno));
    }

    event = nc_calloc(EVENT_SIZE_HINT, sizeof(*event));
    if (event == NULL) {
        status = close(ep);
        if (status < 0) {
            log_error("close e %d failed, ignored: %s", ep, strerror(errno));
        }
    }

    process->ep = ep;
    process->event = event;

    log_debug(LOG_INFO, "e %d ", process->ep);


    status = array_each(&ctx->pool, proxy_each_add_conn, process); 



}

void
process_loop(struct context *ctx,int process_index)
{
    //rstatus_t status = pthread_create(&ctx->stats->tid, NULL, stats_loop, ctx->stats);
    //if (status < 0) {
    //    log_error("stats aggregator create failed: %s", strerror(status));
    //    return NC_ERROR;
    //}

    stats_start_child_aggregator(ctx);
    nc_processes[process_index].pid = getpid();
    nc_current_process_slot = process_index;
    nc_process_init(ctx,&nc_processes[process_index]);

    log_debug(LOG_VVERB, "spawn process process_id = %d ", getpid());
    for(;;){
        core_loop(ctx); 
    }
    exit(1);
}


