#ifndef HTTP_H
#define HTTP_H

typedef struct connection {
    int fd;
    struct connection *prev, *next;
} connection;
extern connection *stream_connections;
extern connection *motion_connections;
extern connection *still_connections;
extern pthread_mutex_t stream_connections_mutex;
extern pthread_mutex_t motion_connections_mutex;
extern pthread_mutex_t still_connections_mutex;
extern pthread_mutex_t http_connections_mutex;

void http_server_thread_cleanup(void *arg);
void *http_server(void *v);
int socket_send(int socket, char* data, int length);

#endif
