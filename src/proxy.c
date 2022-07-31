/* ------------------------------------------------------------------
 * Net Talk Proxy - Network Proxy Task
 * ------------------------------------------------------------------ */

#include "nettalk-proxy.h"


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
            verbose ( "bound connection with socket:%i and socket:%i\n", stream->fd, iter->fd );
            break;
        }
    }
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
int handle_stream_events ( struct proxy_t *proxy, struct stream_t *stream )
{
    int status;

    if ( handle_forward_data ( proxy, stream ) >= 0 )
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
        show_stats ( proxy );
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
 * Proxy task entry point
 */
int proxy_task ( struct proxy_t *proxy )
{
    int sock;
    struct stream_t *stream;
    int status = 0;

    /* Set stream size */
    proxy->stream_size = sizeof ( struct stream_t );

    /* Reset current state */
    proxy->stream_head = NULL;
    proxy->stream_tail = NULL;
    memset ( proxy->stream_pool, '\0', sizeof ( proxy->stream_pool ) );

    /* Proxy events setup */
    if ( proxy_events_setup ( proxy ) < 0 )
    {
        return -1;
    }

    /* Setup listen socket */
    if ( ( sock = listen_socket ( proxy, &proxy->entrance ) ) < 0 )
    {
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
        shutdown_then_close ( proxy, sock );
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

    verbose ( "proxy setup was successful\n" );

    /* Run forward loop */
    while ( ( status = handle_streams_cycle ( proxy ) ) >= 0 );

    /* Remove all streams */
    remove_all_streams ( proxy );

    /* Close epoll fd if created */
    if ( proxy->epoll_fd >= 0 )
    {
        close ( proxy->epoll_fd );
    }

    verbose ( "done proxy uninitializing\n" );

    return status;
}
