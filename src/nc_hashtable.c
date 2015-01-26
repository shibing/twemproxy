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

#include <nc_core.h>
#include <nc_hashtable.h>

void hash_table_init(struct hash_table *ht, uint16_t size) {
    ht->item_count = 0;
    ht->size = size;
    array_init(&ht->head, size, sizeof(struct hash_node));
    uint32_t i;
    for(i=0; i<size; ++i){
        struct hash_node *node = array_push(&ht->head);
        node->valid = 0;
    }
    
}

void hash_table_deinit(struct hash_table *ht) {
   //TODO destroy list

    array_destroy(&ht->head); 
}

void hash_table_set(struct hash_table *ht, uint32_t key, int32_t value) {
    uint16_t    pos;
    pos = key % ht->size;
    struct hash_node *head = array_get(&ht->head, pos);
    struct hash_node *old = NULL;

    log_error("hash_table_set key=%u, value=%d",key,value);

    if (head->valid == 0) {
        head->key = key;
        head->value = value;
        head->valid = 1;
        head->next = NULL;
        return;
    }

    while (head!=NULL && head->valid) {
        if (head->key == key ) {
            head->value = value; 
            return;

        }  else {
            old = head;
            head = head->next;
        }
    }

    if (old!=NULL) {
        struct hash_node *node;
        node = nc_alloc(sizeof(struct hash_node));
        node->next = NULL;
        node->key = key;
        node->value = value;
        node->valid = 1;
        old->next = node;
    }

    return;
}

int32_t hash_table_get(struct hash_table *ht, uint32_t key) {
    uint16_t    pos;
    pos = key % ht->size;
    struct hash_node *head = array_get(&ht->head, pos);
    log_error("hash_table_get key=%u",key);

    while (head!=NULL && head->valid) {
        if (head->key == key) {
            return head->value;
        } else {
            head = head->next;
        }
    }

    if (head == NULL || head->valid==0){
        return -1;
    }
}

rstatus_t
write_ht_channel(int channel, struct hash_cmd *cmd, size_t size)
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
    log_error("wwwwwwwww rite n=%d channel=%d cmd.cmd=%d,cmd.key=%u cmd.value=%d",n,channel,cmd->cmd,cmd->key,cmd->value);
    if (n == -1) {
        log_error("write channel error %s",strerror(errno));
        return NC_ERROR;
    }
    return NC_OK;

}

rstatus_t
read_ht_channel(uint8_t channel, struct hash_cmd *cmd)
{
    ssize_t             n;
    struct iovec        iov[1];
    struct msghdr       msg;
    size_t              size;
    int                 s;
    s = channel;

    union {
        struct cmsghdr  cm;
        char            space[CMSG_SPACE(sizeof(int))];
    } cmsg;

    size = sizeof(struct hash_cmd);

    iov[0].iov_base = (char *) cmd;
    iov[0].iov_len = size;
                      
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    msg.msg_control = (caddr_t) &cmsg;
    msg.msg_controllen = sizeof(cmsg);

    n = recvmsg(channel, &msg, 0);

    log_error("socket pair %d",s);

    if (n == -1) {
        log_error("recvmsg() failed %s",strerror(errno));
        return NC_ERROR;
    }

    if (n == 0) {
        log_error("recvmsg() returned zero");
        return NC_ERROR;
    }

    if ((size_t) n < sizeof(struct hash_cmd)) {
        log_error(
                      "recvmsg() returned not enough data: %uz", n);
        return NC_ERROR;
    }


    return NC_OK; 

}

void remote_get(int channel, uint32_t key, struct conn *conn, struct msg *msg ,struct conn *conn1) {
    struct hash_cmd *cmd;
    struct hash_cmd reply;
    cmd = nc_alloc(sizeof(struct hash_cmd));
    memset(cmd,0,sizeof(struct hash_cmd));
    cmd->cmd = 0;
    cmd->key = key;
    cmd->value = 0;
    cmd->conn = conn;
    cmd->msg = msg;
    TAILQ_INSERT_TAIL(&conn1->ht_cmd_q, cmd, ht_cmd_tqe);
    struct context *ctx;
    ctx = conn1->owner;
    event_add_out(ctx->processes[ctx->current_process_slot].evb, conn1);
    log_error("remote_get");
    log_error("event add out evb=%p,conn=%p, sd=%d pid=%d", ctx->processes[ctx->current_process_slot].evb, conn1,conn1->sd,getpid());
}


void remote_set(int channel, uint32_t key, int32_t value, struct conn* conn1) {
    struct hash_cmd *cmd;
    struct hash_cmd *reply;
    cmd = nc_alloc(sizeof(struct hash_cmd));

    memset(cmd,0,sizeof(struct hash_cmd));
    cmd->cmd = 1;
    cmd->key = key;
    cmd->value = value;
    TAILQ_INSERT_TAIL(&conn1->ht_cmd_q, cmd, ht_cmd_tqe);
    struct context *ctx;
    ctx = conn1->owner;
    event_add_out(ctx->processes[ctx->current_process_slot].evb, conn1);
    log_error("event add out evb=%p,conn=%p, sd=%d", ctx->processes[ctx->current_process_slot].evb, conn1,conn1->sd);

    //write_ht_channel(channel, &cmd, sizeof(struct hash_cmd));
}




