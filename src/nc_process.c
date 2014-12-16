#include <stdlib.h>
#include <nc_core.h>
#include <event/nc_event.h>
#include <nc_server.h>
#include <sys/mman.h>
#include <nc_process.h>
#include <sys/epoll.h>

static rstatus_t proxy_each_add_conn(void *elem, void *data)
{
    rstatus_t status;
    struct server_pool *pool = elem;
    struct conn *p;
    struct nc_process *process = data;

    p = pool->p_conn;

    status = event_add_conn(process->evb, p);
    if (status < 0) {
        log_error("event add conn e %d p %d on addr '%.*s' failed: %s",
                  process->evb->ep, p->sd, pool->addrstr.len, pool->addrstr.data,
                  strerror(errno));
        return NC_ERROR;
    }

    status = event_del_out(process->evb, p);
    if (status < 0) {
        log_error("event del out e %d p %d on addr '%.*s' failed: %s",
                  process->evb->ep, p->sd, pool->addrstr.len, pool->addrstr.data,
                  strerror(errno));
        return NC_ERROR;

    }

    return NC_OK;
     

}

static void nc_process_init(struct context *ctx){
    int status;
    log_error("process init %d",ctx->current_process_slot);
    struct nc_process *process = &ctx->processes[ctx->current_process_slot];

    process->evb = event_base_create(EVENT_SIZE, core_core);
    /*ep = epoll_create(EVENT_SIZE);
    if (ep < 0) {
        log_error("epoll create of size %d failed: %s", EVENT_SIZE, strerror(errno));
    }

    event = nc_calloc(EVENT_SIZE, sizeof(*event));
    if (event == NULL) {
        status = close(ep);
        if (status < 0) {
            log_error("close e %d failed, ignored: %s", ep, strerror(errno));
        }
    }

    process->ep = ep;
    process->event = event;

    log_debug(LOG_INFO, "e %d ", process->ep);
    */


    status = array_each(&ctx->pool, proxy_each_add_conn, process); 

    if (pipe(process->channel) < 0 ) {
        log_error("start pipeline error");
    }


}

void
process_loop(struct context *ctx,int process_index)
{
    log_debug(LOG_VVERB, "spawn process process_id = %d ", getpid());
    ctx->processes[process_index].pid = getpid();

    ctx->current_process_slot = process_index;
    nc_process_init(ctx);

    stats_start_child_aggregator(ctx);

    for(;;){
        core_loop(ctx); 
    }
    exit(1);
}


