#include <stdlib.h>
#include <nc_core.h>
#include <event/nc_event.h>
#include <nc_server.h>
#include <sys/mman.h>
#include <nc_process.h>
#include <sys/epoll.h>
#include <sys/socket.h>

sig_atomic_t nc_reconfigure;
uint8_t nc_exit;

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


static rstatus_t
write_channel(int channel, struct nc_cmd *cmd, size_t size)
{
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;

    union {
        struct cmsghdr  cm;
        char            space[CMSG_SPACE(sizeof(int))];
    } cmsg;

    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    iov[0].iov_base = (char *) cmd;
    iov[0].iov_len = size;
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    n = sendmsg(channel, &msg, 0);
    if (n == -1) {
        log_error("write channel error %s",strerror(errno));
        return NC_ERROR;
    }
    return NC_OK;
}


rstatus_t
process_read_channel(void *arg, uint32_t events)
{
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;
    struct nc_cmd       ch;
    size_t              size;
    int                 s;
    struct conn         *conn;

    union {
        struct cmsghdr  cm;
        char            space[CMSG_SPACE(sizeof(int))];
    } cmsg;

    conn = arg;
    s = conn->sd;
   
    size = sizeof(struct nc_cmd);

    iov[0].iov_base = (char *) &ch;
    iov[0].iov_len = size;
                      
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_control = (caddr_t) &cmsg;
    msg.msg_controllen = sizeof(cmsg);

    n = recvmsg(s, &msg, 0);

    log_error("socket pair %d",s);

    if (n == -1) {
        log_error("recvmsg() failed %s",strerror(errno));
        return NC_ERROR;
    }

    if (n == 0) {
        log_error("recvmsg() returned zero");
        return NC_ERROR;
    }

    if ((size_t) n < sizeof(struct nc_cmd)) {
        log_error(
                      "recvmsg() returned not enough data: %uz", n);
        return NC_ERROR;
    }


    if (ch.command == NC_RELOAD) {
        log_error("get reconfig message");
        nc_exit = 1;
    }


}

void signal_processes(struct context *ctx,uint8_t command ) {
    struct nc_cmd cmd;
    int i;
    cmd.command = command;
    
    for(i=0; i<ctx->worker_num; ++i){
        write_channel(ctx->processes[i].pair_channel[0],&cmd, sizeof(cmd) );

    }
    
    
}

static void nc_process_init(struct context *ctx){
    int status;
    log_error("process init %d",ctx->current_process_slot);
    struct nc_process *process = &ctx->processes[ctx->current_process_slot];
    struct conn dummy_conn;
    struct event_base *evb;

    process->evb = event_base_create(EVENT_SIZE, core_core);


    status = array_each(&ctx->pool, proxy_each_add_conn, process); 
    if (pipe(process->channel) < 0 ) {
        log_error("start pipeline error");
    }


    /* close channel[0] */
    if (close(process->pair_channel[0]) == -1) {
        log_error("close channel 0 failed"); 
    }

    /* add event to channel 1 */
    dummy_conn.dummy = 1;
    dummy_conn.owner = ctx;
    dummy_conn.sd = process->pair_channel[1];

    status = event_add_conn(process->evb, &dummy_conn);
    if (status < 0) {
        log_error("event add stat channel pipe failed");
    }
 




}

rstatus_t process_spawn(struct context *ctx, int i) {

    int status;
    pid_t pid;
    struct nc_process *process = &ctx->processes[i];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctx->processes[i].pair_channel) == -1) {
            log_error("socketpair() failed while spawn");
            return NC_ERROR;
    }


    status = nc_set_nonblocking(process->pair_channel[0]);
    
    if (status < 0) {
        log_error("pair 0 non block error");
        return NC_ERROR;
    }

    status = nc_set_nonblocking(process->pair_channel[1]);
   
    log_error("spawn socket pair %d %d",process->pair_channel[0],process->pair_channel[1]); 
    if (status < 0) {
        log_error("pair 1 non block error");
        return NC_ERROR;
    }

    pid = fork();
    switch (pid) {
    case -1:
        log_error("fork() failed: %s", strerror(errno));
        return NULL;

    case 0:
        process_loop(ctx,i);
        _exit(0);
        break; 
    default:
        break;
    }


}

void
process_loop(struct context *ctx,int process_index)
{
    log_debug(LOG_VVERB, "spawn process process_id = %d ", getpid());
    ctx->processes[process_index].pid = getpid();

    ctx->current_process_slot = process_index;
    nc_process_init(ctx);

    //stats_start_child_aggregator(ctx);

    for(;;){
        core_loop(ctx); 
        if (nc_exit == 1){
            break;
        }
    }
}


