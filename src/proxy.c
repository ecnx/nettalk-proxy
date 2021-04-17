/* ------------------------------------------------------------------
 * AxProxy - Network Proxy Task
 * ------------------------------------------------------------------ */

#include "axproxy.h"

/**
 * Set socket non-blocking mode
 */
static int socket_set_nonblocking ( int sock )
{
    long mode = 0;

    /* Get current socket mode */
    if ( ( mode = fcntl ( sock, F_GETFL, 0 ) ) < 0 )
    {
        return -1;
    }

    /* Update socket mode */
    if ( fcntl ( sock, F_SETFL, mode | O_NONBLOCK ) < 0 )
    {
        return -1;
    }

    return 0;
}

/**
 * Shutdown and close the socket
 */
static void shutdown_then_close ( int sock )
{
    shutdown ( sock, SHUT_RDWR );
    close ( sock );
}

/**
 * Bind address to listen socket
 */
static int listen_socket ( unsigned int addr, unsigned short port )
{
    int sock;
    int yes = 1;
    struct sockaddr_in saddr;

    /* Prepare socket address */
    memset ( &saddr, '\0', sizeof ( saddr ) );
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = addr;
    saddr.sin_port = htons ( port );

    /* Allocate socket */
    if ( ( sock = socket ( AF_INET, SOCK_STREAM, 0 ) ) < 0 )
    {
        return -1;
    }

    /* Allow reusing socket address */
    if ( setsockopt ( sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof ( yes ) ) < 0 )
    {
        shutdown_then_close ( sock );
        return -1;
    }

    /* Bind socket to address */
    if ( bind ( sock, ( struct sockaddr * ) &saddr, sizeof ( saddr ) ) < 0 )
    {
        shutdown_then_close ( sock );
        return -1;
    }

    /* Put socket into listen mode */
    if ( listen ( sock, LISTEN_BACKLOG ) < 0 )
    {
        shutdown_then_close ( sock );
        return -1;
    }

    return sock;
}

/**
 * Shift bytes to queue
 */
static int queue_shift ( struct queue_t *queue, int fd )
{
    size_t i;
    ssize_t len;

    if ( ( len = send ( fd, queue->arr, queue->len, MSG_NOSIGNAL ) ) < 0 )
    {
        return -1;
    }

    queue->len -= len;

    for ( i = 0; i < queue->len; i++ )
    {
        queue->arr[i] = queue->arr[len + i];
    }

    return 0;
}

/**
 * Allocate stream structure and insert into the list
 */
static struct stream_t *insert_stream ( struct proxy_t *proxy, int sock )
{
    struct stream_t *stream;
    size_t i;

    for ( i = 0, stream = NULL; i < POOL_SIZE; i++ )
    {
        if ( !proxy->stream_pool[i].allocated )
        {
            stream = proxy->stream_pool + i;
            break;
        }
    }

    if ( !stream )
    {
        return NULL;
    }

    memset ( stream, '\0', sizeof ( struct stream_t ) );
    stream->role = S_INVALID;
    stream->fd = sock;
    stream->level = LEVEL_NONE;
    stream->allocated = 1;
    stream->next = proxy->stream_head;

    if ( proxy->stream_head )
    {
        proxy->stream_head->prev = stream;

    } else
    {
        proxy->stream_tail = stream;
    }

    proxy->stream_head = stream;

    return stream;
}

/**
 * Free and remove a single stream from the list
 */
static void remove_stream ( struct proxy_t *proxy, struct stream_t *stream )
{
    if ( stream->fd >= 0 )
    {
        if ( stream->pollref )
        {
            epoll_ctl ( proxy->epoll_fd, EPOLL_CTL_DEL, stream->fd, NULL );
        }

        shutdown_then_close ( stream->fd );
        stream->fd = -1;
    }

    if ( stream == proxy->stream_head )
    {
        proxy->stream_head = stream->next;
    }

    if ( stream == proxy->stream_tail )
    {
        proxy->stream_tail = stream->prev;
    }

    if ( stream->next )
    {
        stream->next->prev = stream->prev;
    }

    if ( stream->prev )
    {
        stream->prev->next = stream->next;
    }

    stream->allocated = 0;
}

/**
 * Show relations statistics
 */
#ifdef VERBOSE_MODE
static void show_stats ( struct proxy_t *proxy )
{
    int c_forwarding = 0;
    int c_total = 0;
    int total = 0;
    struct stream_t *iter;

    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        if ( iter->role == S_CLIENT )
        {
            if ( iter->level == LEVEL_FORWARDING )
            {
                c_forwarding++;
            }
            c_total++;
        }

        total++;
    }

    N ( printf ( "[ntpx] load: C:%i/%i *:%i/%i\n", c_forwarding, c_total, total, POOL_SIZE ) );
}
#endif

/*
 * Abandon associated pair of streams
 */
static void remove_relation ( struct stream_t *stream )
{
    if ( stream->neighbour )
    {
        stream->neighbour->abandoned = 1;
    }
    stream->abandoned = 1;
}

/**
 * Remove all relations
 */
static void remove_all_streams ( struct proxy_t *proxy )
{
    struct stream_t *iter;
    struct stream_t *next;

    for ( iter = proxy->stream_head; iter; iter = next )
    {
        next = iter->next;
        remove_stream ( proxy, iter );
        iter = next;
    }
}

/**
 * Reduce streams count
 */
static void reduce_streams ( struct proxy_t *proxy )
{
    struct stream_t *iter;

    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        if ( iter->role == S_CLIENT && iter->level != LEVEL_FORWARDING )
        {
            remove_relation ( iter );
        }
    }
}

/**
 * Remove abandoned streams
 */
static void cleanup_streams ( struct proxy_t *proxy )
{
    struct stream_t *iter;
    struct stream_t *next;

    for ( iter = proxy->stream_head; iter; iter = next )
    {
        next = iter->next;

        if ( iter->abandoned )
        {
            remove_stream ( proxy, iter );
        }
    }
}

/**
 * Remove oldest forwarding relation
 */
static void force_cleanup ( struct proxy_t *proxy, const struct stream_t *excl )
{
    struct stream_t *iter;

    for ( iter = proxy->stream_tail; iter; iter = iter->prev )
    {
        if ( iter != excl && iter->abandoned )
        {
            remove_relation ( iter );
            remove_stream ( proxy, iter );
            return;
        }
    }

    for ( iter = proxy->stream_tail; iter; iter = iter->prev )
    {
        if ( iter != excl && iter->role == S_CLIENT )
        {
            remove_relation ( iter );
            remove_stream ( proxy, iter );
            return;
        }
    }
}

/**
 * Build stream event list with poll
 */
static int build_poll_list ( struct proxy_t *proxy, struct pollfd *poll_list, size_t *poll_len )
{
    size_t poll_size;
    size_t poll_rlen = 0;
    struct stream_t *iter;
    struct pollfd *pollref;

    poll_size = *poll_len;

    /* Reset poll references */
    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        iter->pollref = NULL;
    }

    /* Append file descriptors to the poll list */
    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        /* Assert poll list index */
        if ( poll_rlen >= poll_size )
        {
            return -1;
        }

        /* Add stream to poll list if applicable */
        if ( iter->events )
        {
            pollref = poll_list + poll_rlen;
            pollref->fd = iter->fd;
            pollref->events = POLLERR | POLLHUP | iter->events;
            iter->pollref = pollref;
            poll_rlen++;
        }
    }

    *poll_len = poll_rlen;

    return 0;
}

/**
 * Update streams revents with poll
 */
static void update_revents_poll ( struct proxy_t *proxy )
{
    struct stream_t *iter;

    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        iter->revents = iter->pollref ? iter->pollref->revents : 0;
    }
}

/**
 * Watch stream events with poll
 */
static int watch_streams_poll ( struct proxy_t *proxy )
{
    int nfds;
    size_t poll_len;
    struct pollfd poll_list[POOL_SIZE];

    /* Set poll list size */
    poll_len = sizeof ( poll_list ) / sizeof ( struct pollfd );

    /* Rebuild poll event list */
    if ( build_poll_list ( proxy, poll_list, &poll_len ) < 0 )
    {
        N ( printf ( "[ntpx] poll build failed: %i\n", errno ) );
        return -1;
    }

    /* Poll events */
    if ( ( nfds = poll ( poll_list, poll_len, POLL_TIMEOUT_MSEC ) ) < 0 )
    {
        N ( printf ( "[ntpx] poll failed: %i\n", errno ) );
        return -1;
    }

    /* Update stream poll revents */
    update_revents_poll ( proxy );

    return nfds;
}

/**
 * Convert poll to epoll events
 */
static int poll_to_epoll_events ( int poll_events )
{
    int epoll_events = 0;

    if ( poll_events & POLLERR )
    {
        epoll_events |= EPOLLERR;
    }

    if ( poll_events & POLLHUP )
    {
        epoll_events |= EPOLLHUP;
    }

    if ( poll_events & POLLIN )
    {
        epoll_events |= EPOLLIN;
    }

    if ( poll_events & POLLOUT )
    {
        epoll_events |= EPOLLOUT;
    }

    return epoll_events;
}

/**
 * Convert epoll to poll events
 */
static int epoll_to_poll_events ( int epoll_events )
{
    int poll_events = 0;

    if ( epoll_events & EPOLLERR )
    {
        poll_events |= POLLERR;
    }

    if ( epoll_events & EPOLLHUP )
    {
        poll_events |= POLLHUP;
    }

    if ( epoll_events & EPOLLIN )
    {
        poll_events |= POLLIN;
    }

    if ( epoll_events & EPOLLOUT )
    {
        poll_events |= POLLOUT;
    }

    return epoll_events;
}

/**
 * Build stream event list with epoll
 */
static int build_epoll_list ( struct proxy_t *proxy )
{
    int operation;
    struct stream_t *iter;
    struct epoll_event event;

    /* Append file descriptors to the poll list */
    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        if ( iter->events )
        {
            if ( !iter->pollref || iter->events != iter->levents )
            {
                event.data.ptr = iter;
                event.events = poll_to_epoll_events ( iter->events | POLLERR | POLLHUP );
                operation = iter->pollref ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

                if ( epoll_ctl ( proxy->epoll_fd, operation, iter->fd, &event ) < 0 )
                {
                    return -1;
                }

                iter->levents = iter->events;
                iter->pollref = EPOLLREF;
            }

        } else if ( iter->pollref )
        {
            if ( epoll_ctl ( proxy->epoll_fd, EPOLL_CTL_DEL, iter->fd, NULL ) < 0 )
            {
                return -1;
            }

            iter->pollref = NULL;
        }
    }

    return 0;
}

/**
 * Update streams revents with poll
 */
static void update_revents_epoll ( struct proxy_t *proxy, int nfds, struct epoll_event *events )
{
    int i;
    struct stream_t *stream;

    for ( stream = proxy->stream_head; stream; stream = stream->next )
    {
        stream->revents = 0;
    }

    for ( i = 0; i < nfds; i++ )
    {
        if ( ( stream = events[i].data.ptr ) )
        {
            stream->revents = epoll_to_poll_events ( events[i].events );
        }
    }
}

/**
 * Watch stream events with epoll
 */
static int watch_streams_epoll ( struct proxy_t *proxy )
{
    int nfds;
    struct epoll_event events[POOL_SIZE];

    /* Rebuild epoll event list */
    if ( build_epoll_list ( proxy ) < 0 )
    {
        N ( printf ( "[ntpx] poll build failed: %i\n", errno ) );
        return -1;
    }

    /* E-Poll events */
    if ( ( nfds = epoll_wait ( proxy->epoll_fd, events, POOL_SIZE, POLL_TIMEOUT_MSEC ) ) < 0 )
    {
        N ( printf ( "[ntpx] poll failed: %i\n", errno ) );
        return -1;
    }

    /* Update stream epoll revents */
    update_revents_epoll ( proxy, nfds, events );

    return nfds;
}

/**
 * Watch stream events
 */
static int watch_streams ( struct proxy_t *proxy )
{
    if ( proxy->epoll_fd >= 0 )
    {
        return watch_streams_epoll ( proxy );
    }

    return watch_streams_poll ( proxy );
}

/**
 * Create a new stream
 */
static struct stream_t *accept_new_stream ( struct proxy_t *proxy, int lfd )
{
    int sock;
    struct stream_t *stream;

    /* Accept incoming connection */
    if ( ( sock = accept ( lfd, NULL, NULL ) ) < 0 )
    {
        return NULL;
    }

    /* Set non-blocking mode on socket */
    if ( socket_set_nonblocking ( sock ) < 0 )
    {
        shutdown_then_close ( sock );
        return NULL;
    }

    /* Try allocating new stream */
    if ( !( stream = insert_stream ( proxy, sock ) ) )
    {
        force_cleanup ( proxy, NULL );
        stream = insert_stream ( proxy, sock );
    }

    /* Check if stream was allocated */
    if ( !stream )
    {
        shutdown_then_close ( sock );
        return NULL;
    }

    return stream;
}

/**
 * Handle new stream creation
 */
static int handle_new_stream ( struct proxy_t *proxy, struct stream_t *stream, int role )
{
    struct stream_t *util;

    if ( ~stream->revents & POLLIN )
    {
        return -1;
    }

    /* Accept incoming connection */
    if ( !( util = accept_new_stream ( proxy, stream->fd ) ) )
    {
        return -2;
    }

    /* Setup new stream */
    util->role = role;
    util->level = LEVEL_PENDING;
    if ( role == S_CLIENT )
    {
        util->events = POLLIN;
    }

    return 0;
}

/**
 * Bind stream into a relation
 */
static void bind_neighbour ( struct proxy_t *proxy, struct stream_t *stream )
{
    struct stream_t *iter;

    /* Find peer on the same channel */
    for ( iter = proxy->stream_head; iter; iter = iter->next )
    {
        if ( iter != stream && !iter->abandoned && iter->role == S_CLIENT
            && iter->level == LEVEL_UNBOUND && !iter->neighbour
            && !memcmp ( iter->channel.bytes, stream->channel.bytes,
                sizeof ( iter->channel.bytes ) ) )
        {
            stream->neighbour = iter;
            iter->neighbour = stream;
            stream->level = LEVEL_ACK;
            iter->level = LEVEL_ACK;
            stream->events = POLLOUT;
            iter->events = POLLOUT;
            return;
        }
    }
}


/**
 * Forward data between sockets
 */
static int socket_forward_data ( int srcfd, int dstfd )
{
    int len = FORWARD_CHUNK_LEN;
    int recvlim;
    int sendlim;
    int sendwip;
    socklen_t optlen;
    unsigned char buffer[FORWARD_CHUNK_LEN];

    if ( ioctl ( srcfd, FIONREAD, &recvlim ) < 0 )
    {
        return -1;
    }

    if ( !recvlim )
    {
        return -1;
    }

    if ( recvlim < len )
    {
        len = recvlim;
    }

    if ( ioctl ( dstfd, TIOCOUTQ, &sendwip ) < 0 )
    {
        return -1;
    }

    optlen = sizeof ( sendlim );

    if ( getsockopt ( dstfd, SOL_SOCKET, SO_SNDBUF, &sendlim, &optlen ) < 0 )
    {
        return -1;
    }

    if ( optlen != sizeof ( sendlim ) )
    {
        return -1;
    }

    if ( sendwip > sendlim )
    {
        return -1;
    }

    sendlim -= sendwip;

    if ( !sendlim )
    {
        return -1;
    }

    if ( sendlim < len )
    {
        len = sendlim;
    }

    if ( !len )
    {
        return -1;
    }

    if ( recv ( srcfd, buffer, len, MSG_PEEK ) < len )
    {
        return -1;
    }

    if ( ( len = send ( dstfd, buffer, len, MSG_NOSIGNAL ) ) < 0 )
    {
        return -1;
    }

    if ( recv ( srcfd, buffer, len, 0 ) < len )
    {
        return -1;
    }

    return len;
}

/**
 * Handle stream data forward
 */
static int handle_forward_data ( struct stream_t *stream )
{
    if ( !stream->neighbour || stream->level != LEVEL_FORWARDING )
    {
        return -1;
    }

    if ( stream->revents & POLLOUT )
    {
        if ( socket_forward_data ( stream->neighbour->fd, stream->fd ) < 0 )
        {
            return -1;
        }

        stream->events &= ~POLLOUT;
        stream->neighbour->events |= POLLIN;

    } else if ( stream->revents & POLLIN )
    {
        stream->events &= ~POLLIN;
        stream->neighbour->events |= POLLOUT;
    }

    return 0;
}

/**
 * Handle stream channel assignation
 */
static int handle_assign_channel ( struct proxy_t *proxy, struct stream_t *stream )
{
    ssize_t len;

    if ( stream->level == LEVEL_PENDING && stream->revents & POLLIN )
    {
        if ( ( len = recv ( stream->fd, stream->queue.arr + stream->queue.len,
                    sizeof ( stream->queue.arr ) - stream->queue.len, 0 ) ) > 0 )
        {
            stream->queue.len += len;
            if ( stream->queue.len == sizeof ( stream->channel.bytes ) )
            {
                memcpy ( stream->channel.bytes, stream->queue.arr, stream->queue.len );
                stream->level = LEVEL_UNBOUND;
                stream->events = POLLIN;
                bind_neighbour ( proxy, stream );
            }

            return 0;
        }
    }

    return -1;
}

/**
 * Handle stream events
 */
static int handle_stream_events ( struct proxy_t *proxy, struct stream_t *stream )
{
    int status;

    if ( handle_forward_data ( stream ) >= 0 )
    {
        return 0;
    }

    if ( stream->role == S_CLIENT && stream->level == LEVEL_ACK
        && stream->queue.len && ( stream->revents & POLLOUT ) )
    {
        if ( queue_shift ( &stream->queue, stream->fd ) < 0 )
        {
            remove_relation ( stream );
            return 0;
        }
        if ( stream->queue.len == 0 )
        {
            stream->level = LEVEL_FORWARDING;
            stream->events = POLLIN;
        }
        return 0;
    }

    switch ( stream->role )
    {
    case S_SERVER:
        N ( show_stats ( proxy ) );
        if ( ( status = handle_new_stream ( proxy, stream, S_CLIENT ) ) == -2 )
        {
            return -1;
        }
        return 0;
    case S_CLIENT:
        if ( handle_assign_channel ( proxy, stream ) >= 0 )
        {
            return 0;
        }
        break;
    }

    remove_relation ( stream );

    return 0;
}

/**
 * Stream event handling cycle
 */
static int handle_streams_cycle ( struct proxy_t *proxy )
{
    int status;
    struct stream_t *iter;
    struct stream_t *next;

    /* Cleanup streams */
    cleanup_streams ( proxy );

    /* Watch streams events */
    if ( ( status = watch_streams ( proxy ) ) < 0 )
    {
        N ( printf ( "[ntpx] event watch failed: %i\n", errno ) );
        return -1;
    }

    /* Reduce streams count */
    if ( !status )
    {
        reduce_streams ( proxy );
        cleanup_streams ( proxy );
        N ( show_stats ( proxy ) );
        return 0;
    }

    /* Process stream list */
    for ( iter = proxy->stream_head; iter; iter = next )
    {
        next = iter->next;

        if ( !iter->abandoned && iter->revents )
        {
            if ( iter->revents & ( POLLERR | POLLHUP ) )
            {
                remove_relation ( iter );

            } else
            {
                if ( handle_stream_events ( proxy, iter ) < 0 )
                {
                    return -1;
                }
            }
        }
    }

    return 0;
}

/**
 * Proxy task entry point
 */
int proxy_task ( struct proxy_t *proxy )
{
    int sock;
    struct stream_t *stream;
    int status = 0;

    /* Reset current state */
    proxy->stream_head = NULL;
    proxy->stream_tail = NULL;
    memset ( proxy->stream_pool, '\0', sizeof ( proxy->stream_pool ) );

    /* Create epoll fd if possible */
    if ( ( proxy->epoll_fd = epoll_create1 ( 0 ) ) >= 0 )
    {
        N ( printf ( "[ntpx] epoll initialized.\n" ) );

    } else
    {
        N ( printf ( "[ntpx] epoll not supported.\n" ) );
    }

    /* Setup listen socket */
    if ( ( sock = listen_socket ( proxy->laddr, proxy->lport ) ) < 0 )
    {
        N ( printf ( "[ntpx] bind socket failed: %i\n", errno ) );
        remove_all_streams ( proxy );
        if ( proxy->epoll_fd >= 0 )
        {
            close ( proxy->epoll_fd );
        }
        return -1;
    }

    /* Allocate new stream */
    if ( !( stream = insert_stream ( proxy, sock ) ) )
    {
        shutdown_then_close ( sock );
        remove_all_streams ( proxy );
        if ( proxy->epoll_fd >= 0 )
        {
            close ( proxy->epoll_fd );
        }
        return -1;
    }

    /* Update listen stream */
    stream->role = S_SERVER;
    stream->events = POLLIN;

    N ( printf ( "[ntpx] setup successful.\n" ) );

    /* Run forward loop */
    while ( ( status = handle_streams_cycle ( proxy ) ) >= 0 );

    /* Remove all streams */
    remove_all_streams ( proxy );

    /* Close epoll fd if created */
    if ( proxy->epoll_fd >= 0 )
    {
        close ( proxy->epoll_fd );
    }

    N ( printf ( "[ntpx] free done.\n" ) );

    return status;
}