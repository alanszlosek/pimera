#ifndef DETECTION_H
#define DETECTION_H

#include <inttypes.h>
#include <pthread.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_port.h"

#include "settings.h"

// TODO: just need fps and y_length locally

typedef struct {
    int64_t detection_sleep;
    int stream_sleep;
    uint8_t *previous_frame;

    // holds yuv data so we can spread detection across multiple yuvCallback() calls
    uint8_t* current_frame;
    unsigned int changed_pixels_threshold;
    // This is only used for stats reporting in the API endpoint
    unsigned int pixel_delta;
    /*
    struct {
        unsigned int offset;
        unsigned int row_length;
        unsigned int stride;
        unsigned int batches;
        unsigned int row_batch_size;
        unsigned int num_rows;
    } region;
    */
    struct {
        unsigned int batches;
        uint8_t** pointers;
    } processing;

    char boundary[81];
    int boundary_length;

    unsigned int motion_count;
} MOTION_DETECTION_T;
extern MOTION_DETECTION_T motion_detection;
extern pthread_mutex_t motion_detection_mutex;

void detection_config(unsigned int fps, unsigned int y_length, MMAL_QUEUE_T* queue);
void detection_threshold(unsigned int threshold);
void yuv_callback(MMAL_PORT_T*, MMAL_BUFFER_HEADER_T*);


#endif
