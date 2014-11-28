#ifndef _NC_PROCESS_H_
#define _NC_PROCESS_H_
struct nc_process {        
    pid_t                 pid;
    struct event_base      *evb;                 /* event base */
    int                   channel[2];  /* socket pair for watcher process and child process */ 
};

#define NC_PROCESSES         1
#define NC_MAX_PROCESSES         1024

struct nc_process *processes;

void process_loop(struct context *ctx, int process_index);

#endif
