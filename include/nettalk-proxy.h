/* ------------------------------------------------------------------
 * Net Talk Proxy - Shared Project Header
 * ------------------------------------------------------------------ */

#ifndef NETTALK_PROXY_H
#define NETTALK_PROXY_H

#include "defs.h"
#include "config.h"

#define S_INVALID                   -1
#define S_SERVER                    0
#define S_CLIENT                    1

#define LEVEL_PENDING               1
#define LEVEL_UNBOUND               2
#define LEVEL_ACK                   3

#define EPOLLREF                    ((struct pollfd*) -1)

/**
 * Utility data queue
 */
struct queue_t
{
    size_t len;
    uint8_t arr[DATA_QUEUE_CAPACITY];
};

/**
 * Channel ID
 */
struct channel_t
{
    uint8_t bytes[16];
};

/**
 * IP/TCP connection stream
 */
struct stream_t
{
    int role;
    int fd;
    int level;
    int allocated;
    int abandoned;
    short events;
    short levents;
    short revents;

    struct pollfd *pollref;
    struct stream_t *neighbour;
    struct stream_t *prev;
    struct stream_t *next;
    struct queue_t queue;
    struct channel_t channel;
};

/**
 * NetTalk Proxy task context
 */
struct proxy_t
{
    size_t stream_size;
    int verbose;
    int epoll_fd;
    struct stream_t *stream_head;
    struct stream_t *stream_tail;
    struct stream_t stream_pool[POOL_SIZE];

    struct sockaddr_storage entrance;
};

/**
 * Proxy task entry point
 */
extern int proxy_task ( struct proxy_t *proxy );

#include "util.h"

#endif
