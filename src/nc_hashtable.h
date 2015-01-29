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
#ifndef _NC_HASHTABLE_H_
#define _NC_HASHTABLE_H_

#include <nc_core.h>

/*
 * Hashtable 
 */

struct hash_node {
    uint32_t key;
    int32_t value;
    struct hash_node *next;
    int8_t  valid:1;
};

struct hash_table {
    uint16_t        size;
    uint32_t        item_count;
    struct array    head;
};

struct hash_cmd {
    uint8_t                 cmd;                  /* 0 get 1 put 2 reply */
    uint32_t                key;
    int32_t                 value;
    struct conn             *conn;                /* msg need forward to this conn after this cmd*/ 
    struct msg              *msg;                 /* msg need processed after this cmd */
    TAILQ_ENTRY(hash_cmd)   ht_cmd_tqe;           /* link in client q */

};

void remote_set(int channel, uint32_t key, int32_t value, struct context* ctx); 
void remote_get(int channel, uint32_t key, struct conn *next_conn, struct msg *next_msg, struct context *ctx);

rstatus_t read_ht_channel(uint8_t channel, struct hash_cmd *cmd);
rstatus_t write_ht_channel(int channel, struct hash_cmd *cmd, size_t size);

TAILQ_HEAD(ht_cmd_tqh, hash_cmd);

#endif
