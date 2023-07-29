#include <stdio.h>

#include "camera.h"
#include "detection.h"
#include "http.h"
#include "mjpeg.h"
#include "log.h"


int stream_threshold = 0;
int mjpeg_concurrent = 0;
pthread_mutex_t mjpeg_concurrent_mutex;

MMAL_QUEUE_T *mjpeg_queue = NULL;
unsigned int mjpeg_frame_counter = 0;

// callbacks DO run in a separate thread from main
void mjpeg_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    clock_t begin, end;
    int64_t current_time;
    // MJPEG streaming variables
    int ret;
    char contentLength[21];
    int contentLengthLength;

    // begin = clock();

    mjpeg_frame_counter++;
    if (mjpeg_frame_counter >= stream_threshold) {
        stream_threshold = mjpeg_frame_counter + motionDetection.stream_sleep;

        // TODO: put connection lists in a circular buffer of linked lists
        // so clients can control their framerate by passing param with HTTP req
        pthread_mutex_lock(&stream_connections_mutex);
        connection* connection = stream_connections;
        if (connection) {

            contentLengthLength = snprintf(contentLength, 20, "%d\r\n\r\n", buffer->length);

            mmal_buffer_header_mem_lock(buffer);
            while (connection) {
                // bail when fail to write
                sendSocket(connection->fd, motionDetection.boundary, motionDetection.boundaryLength) && 
                sendSocket(connection->fd, contentLength, contentLengthLength) && 
                sendSocket(connection->fd, (char*)buffer->data, buffer->length);
                connection = connection->next;
            }
            mmal_buffer_header_mem_unlock(buffer);
        }
        pthread_mutex_unlock(&stream_connections_mutex);
    }

    mmal_buffer_header_release(buffer);

    send_buffers_to_port(port, mjpeg_queue);

    // end = clock();
    // printf("MJPEG CALLBACK. Time: %f\n", (double)(end - begin) / CLOCKS_PER_SEC);
}


void mjpeg_config(MMAL_QUEUE_T *queue) {
    mjpeg_queue = queue;
}
