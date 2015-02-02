#ifndef _NC_PROCESS_H_
#define _NC_PROCESS_H_

#include <sys/signal.h>

struct nc_process {        
    pid_t                 pid;
    struct event_base      *evb;                 /* event base */
    int                   pair_channel[2];  /* socket pair for watcher process and child process */ 
    int                   channel[2];  /* pipeline for stat child and master */
    struct conn           dummy_conn;      /* dummy connection for socket pair communication */

};

#define NC_PROCESSES         1
#define NC_MAX_PROCESSES         1024

struct nc_process *processes;
extern sig_atomic_t nc_reconfiging;
extern sig_atomic_t nc_reconfigure;
extern uint8_t nc_exit;

rstatus_t process_spawn(struct context *ctx, int process_index);
void process_loop(struct context *ctx, int process_index);
void signal_processes(struct context *ctx,uint8_t command );
rstatus_t
process_read_channel(void *arg, uint32_t events);


#endif
