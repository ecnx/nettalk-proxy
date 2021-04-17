/* ------------------------------------------------------------------
 * AxProxy - Shared Project Header
 * ------------------------------------------------------------------ */

#ifndef AXPROXY_H
#define AXPROXY_H

#include "defs.h"
#include "config.h"

#define S_INVALID                   -1
#define S_SERVER                    0
#define S_CLIENT                    1

#define LEVEL_NONE                  0
#define LEVEL_PENDING               1
#define LEVEL_UNBOUND               2
#define LEVEL_ACK                   3
#define LEVEL_FORWARDING            4

#define EPOLLREF                    ((struct pollfd*) -1)

/**
 * Utility data queue
 */
struct queue_t
{
    size_t len;
    unsigned char arr[16];
};

/**
 * Channel ID
 */
struct channel_t
{
    unsigned char bytes[16];
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
 * AxProxy task context
 */
struct proxy_t
{
    int epoll_fd;
    unsigned int laddr;
    unsigned short lport;

    struct stream_t *stream_head;
    struct stream_t *stream_tail;
    struct stream_t stream_pool[POOL_SIZE];
};

/**
 * Proxy task entry point
 */
extern int proxy_task ( struct proxy_t *proxy );

#endif
