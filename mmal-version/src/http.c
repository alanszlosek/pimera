#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "http.h"
#include "detection.h"
#include "log.h"
#include "shared.h"
#include "settings.h"
#include "utlist.h"

#define MAX_POLL_FDS 100
#define REQUEST_MAX_LENGTH 8192
struct pollfd http_fds[MAX_POLL_FDS];
int http_fds_count = 0;

connection *stream_connections;
connection *motion_connections;
connection *still_connections;
pthread_mutex_t stream_connections_mutex;
pthread_mutex_t motion_connections_mutex;
pthread_mutex_t still_connections_mutex;
pthread_mutex_t http_connections_mutex;


connection* create_connection(int fd) {
    connection *conn = (connection*) malloc(sizeof(struct connection));
    conn->fd = fd;
    conn->next = conn->prev = NULL;
    return conn;
}
connection* find_connection(connection* head, int fd) {
    while (head) {
        if (head->fd == fd) {
            return head;
        }
        head = head->next;
    }
    return NULL;
}
void clear_connections(connection* head) {
    while (head) {
        connection *h = head;
        head = head->next;
        free(h);
    }
}

int socket_send(int socket, char* data, int length) {
    int totalWritten = 0;
    int written = 0;
    int remaining = length;
    int attempts = 0;
    
    // TODO: think through the attempts more
    while (totalWritten < length) {
        written = send(socket, data, remaining, MSG_NOSIGNAL);
        // if fail to write 3 times, bail

        attempts++;
        if (attempts >= 3) {
            log_error("Three failed attempts to write to socket", __func__);
            return -1;
        }
        remaining -= written;
        data += written;
        totalWritten += written;
    }
    return totalWritten;
}


int http_server_cleanup_arg = 0;
void http_server_thread_cleanup(void *arg) {
    pthread_mutex_unlock(&running_mutex);

    log_info("HTTP Server thread closing down");
    for (int i = 0; i < http_fds_count; i++) {
        log_info("Closing %d", http_fds[i].fd);
        close(http_fds[i].fd);
    }

    pthread_mutex_lock(&stream_connections_mutex);
    clear_connections(stream_connections);
    pthread_mutex_unlock(&stream_connections_mutex);
}

void *http_server(void *v) {
    SETTINGS* settings = (SETTINGS*)v;
    bool r;
    int status;
    int ret;

    int port = 8080;
    int listener, socketfd;
    int length;
    static struct sockaddr_in cli_addr; /* static = initialised to zeros */
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */
    char request[REQUEST_MAX_LENGTH];
    int i;
    int yes = 1;

    pthread_cleanup_push(http_server_thread_cleanup, NULL);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    //log_info("Opening socket");
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fprintf(stdout, "Failed to create server socket: %d\n", listener);
        //log_error("Failed to create listening socket", __func__);
        pthread_exit(NULL);
    }

    // REUSEADDR so socket is available again immediately after close()
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    log_info("Binding");
    status = bind(listener, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (status < 0) {
        fprintf(stdout, "Failed to bind: %d\n", errno);
        log_error("Failed to bind", __func__);
        close(listener);
        pthread_exit(NULL);
    }

    log_info("Listening");
    status = listen(listener, SOMAXCONN);
    if (status < 0) {
        fprintf(stdout, "Failed to listen: %d\n", status);
        close(listener);
        pthread_exit(NULL);
    }
    length = sizeof(cli_addr);

    // add listener to poll fds
    http_fds[0].fd = listener;
    http_fds[0].events = POLLIN | POLLERR | POLLNVAL;
    http_fds_count++;

    pthread_mutex_lock(&running_mutex);
    r = running;
    pthread_mutex_unlock(&running_mutex);
    while (r) {
        // this blocks, which is what we want
        log_info("Num streaming FDs: %d\n", http_fds_count);
        ret = poll(http_fds, http_fds_count, -1);
        if (ret < 1) {
            // thread cancelled or other?
            // TODO: handle this
            printf("poll returned <-1. errno: %d\n", errno);
            continue;
        }

        for (int i = 0; i < http_fds_count; ) {
            if (http_fds[i].revents == 0) {
                // nothing to do for this socket
                i++;
                continue;
            }
            // handle new connections
            if (i == 0) {
                if (http_fds[i].revents & POLLIN) {
                    socketfd = accept(listener, NULL, 0); //, (struct sockaddr *)&cli_addr, &length);
                    if (socketfd < 0) {
                        log_info("Failed to accept() connection: %d", errno);
                        pthread_mutex_lock(&running_mutex);
                        r = running;
                        pthread_mutex_unlock(&running_mutex);
                        // TODO: think we should break if accept fails
                        // and perhaps re-listen?
                        break;
                    }
                    if (http_fds_count < MAX_POLL_FDS) {
                        log_info("Accepted: %d", socketfd);

                        // IS THIS FD ALREADY IN OUR LIST?

                        http_fds[http_fds_count].fd = socketfd;
                        http_fds[http_fds_count].events = POLLIN;
                        http_fds_count++;
                    } else {
                        log_error("No space for connection. Closing", __func__);
                        close(socketfd);
                    }
                } else {
                    log_info("Got unexpected revents value on listening socket: %d", http_fds[i].revents);
                }

                // move on to next item in fds
                i++;
                continue;
            } // end handle new connections


            if (http_fds[i].revents & (POLLIN | POLLHUP)) {
                // TODO: we may not receive all the data at once, hmmm
                int bytes = recv(http_fds[i].fd, request, REQUEST_MAX_LENGTH, 0);
                if (bytes > 0) {
                    request[bytes] = 0;

                    fprintf(stdout, "[INFO] Got request: %s\n", request);
                    ret = handle_request(request, http_fds[i].fd, settings);
                    if (ret > 0) {
                        i++;
                        continue;
                    }
                    fprintf(stdout, "[INFO] Got here, how?: %d\n", i);

                    // fall through to close
                } else {
                    // read returned 0 bytes
                    //fprintf(stdout, "[INFO] Read returned 0: %d\n", i);
                    // fall through to close
                    log_info("Socket %d returned 0 bytes", http_fds[i].fd);
                }
            } else {
                log_info("Got other revents value on req socket: %d", http_fds[i].revents);
            }


            // IF GOT HERE, CLOSE SOCKET
            printf("Closing %d\n", http_fds[i].fd);
            connection* el;
            pthread_mutex_lock(&stream_connections_mutex);
            if ((el = find_connection(stream_connections, http_fds[i].fd))) {
                // remove from mjpeg streaming list
                LL_DELETE(stream_connections, el);
            }
            pthread_mutex_unlock(&stream_connections_mutex);

            pthread_mutex_lock(&motion_connections_mutex);
            if ((el = find_connection(motion_connections, http_fds[i].fd))) {
                // remove from mjpeg streaming list
                LL_DELETE(motion_connections, el);
            }
            pthread_mutex_unlock(&motion_connections_mutex);

            ret = close(http_fds[i].fd);
            if (ret != 0) {
                log_info("close on socket %d returned: %d", http_fds[i].fd, ret);
            }

            // move last place into the slot that was just removed
            http_fds[i] = http_fds[http_fds_count - 1];
            http_fds_count--;
            // process the same slot again, which has a new file descriptor
            // don't increment i
        }

        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }
    log_info("HTTP Server thread closing down");
    pthread_cleanup_pop(http_server_cleanup_arg);
    return NULL;
}

int handle_request(char* request, int poll_fd, SETTINGS* settings) {
    int ret;

    char* response_header = "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\nCache-control: no-store\r\nContent-Type: multipart/x-mixed-replace; boundary=HATCHA\r\n\r\n";
    int response_header_length = strlen(response_header);
    int response2_max = 8192;
    char response1[1024], response2[8193]; // for index.html
    int response1_length, response2_length;


    // parse and handle GET request
    if (strncmp(request, "GET / HTTP", 10) == 0 || strncmp(request, "GET /index.html HTTP", 20) == 0) {
        // write index.html
        FILE* f = fopen("public/index.html", "r");
        response2_length = fread(response2, 1, response2_max, f);
        fclose(f);

        // note the Connection:close header to prevent keep-alive
        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);
        ret = socket_send(poll_fd, response1, response1_length);
        ret += socket_send(poll_fd, response2, response2_length);

    } else if (strncmp(request, "GET /stream.mjpeg?ts=", 21) == 0) {
        ret = socket_send(poll_fd, response_header, response_header_length);

        pthread_mutex_lock(&stream_connections_mutex);
        LL_APPEND(stream_connections, create_connection( poll_fd ));
        pthread_mutex_unlock(&stream_connections_mutex);

    } else if (strncmp(request, "GET /motion.mjpeg HTTP", 22) == 0) {
        // send motion pixels as mjpeg
        ret = socket_send(poll_fd, response_header, response_header_length);

        pthread_mutex_lock(&motion_connections_mutex);
        LL_APPEND(motion_connections, create_connection( poll_fd ));
        pthread_mutex_unlock(&motion_connections_mutex);
    
    } else if (strncmp(request, "GET /status.json HTTP", 21) == 0) {
        pthread_mutex_lock(&motion_detection_mutex);
        int motion_count = motion_detection.motion_count;
        int delta = motion_detection.pixel_delta;
        pthread_mutex_unlock(&motion_detection_mutex);
        response2_length = snprintf(
            response2,
            response2_max,
            "{\"width\": \"%d\", \"height\":\"%d\", \"motion\": %d, \"delta\": %d, \"threshold\": %d, \"region\": \"%d,%d,%d,%d\"}",
            settings->width,
            settings->height,
            motion_count,
            delta,
            settings->threshold,
            settings->region[0],
            settings->region[1],
            settings->region[2],
            settings->region[3]
        );

        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);

        ret = socket_send(poll_fd, response1, response1_length);
        ret += socket_send(poll_fd, response2, response2_length);

    // TODO: add region change GET via query params. stops camera capture, waits half second adjusts settings, restarts capture
    // Expects: /region/123,123,123,123 ... no spaces
    } else if (strncmp(request, "GET /region/", 12) == 0) {
        // Parse region from URL path
        // start at 12
        char *start = request + 12;
        char *c = strtok(start, ",");
        int j = 0;
        while (c != NULL) {
            if (j > 3) {
                break;
            }
            int a = atoi(c);
            settings->region[j] = a;
            j++;
            c = strtok(NULL, ",");
        }
        pthread_mutex_lock(&restart_mutex);
        restart = 1;
        pthread_mutex_unlock(&restart_mutex);
        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
        ret = socket_send(poll_fd, response1, response1_length);
    
    } else if (strncmp(request, "GET /threshold/", 15) == 0) {
        // Parse region from URL path
        // start at 12
        char *start = request + 15;
        char *c = strtok(start, ",");
        settings->threshold = atoi(c);
        
        pthread_mutex_lock(&restart_mutex);
        restart = 1;
        pthread_mutex_unlock(&restart_mutex);
        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
        ret = socket_send(poll_fd, response1, response1_length);

    } else if (strncmp(request, "GET /restart HTTP", 17) == 0) {
        pthread_mutex_lock(&restart_mutex);
        restart = 1;
        pthread_mutex_unlock(&restart_mutex);
        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
        ret = socket_send(poll_fd, response1, response1_length);

    } else {
        // request for something we don't handle
        response1_length = snprintf(response1, 1024, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found");
        ret = socket_send(poll_fd, response1, response1_length);
    }
    log_info("handle_request returned %d bytes on %d\n", ret, poll_fd);
    return ret;
}