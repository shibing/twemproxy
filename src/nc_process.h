#ifndef _NC_PROCESS_H_
#define _NC_PROCESS_H_
typedef struct {        
    pid_t           pid;
    int                ep;          /* epoll device */
    struct epoll_event *event;      /* epoll event */
    int                 status;
    int        channel[2];                                
    void               *data;   
    struct stats       *stats;      /* stats */
    char               *name;                                      

} nc_process_t;

#define NC_PROCESSES         1
#define NC_MAX_PROCESSES         1024
nc_process_t    *nc_processes;
extern int nc_current_process_slot;

void process_loop(struct context *ctx, int process_index);

#endif
