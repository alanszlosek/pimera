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
    int detection_sleep;
    int stream_sleep;
    uint8_t *previousFrame;

    // holds yuv data so we can spread detection across multiple yuvCallback() calls
    uint8_t* currentFrame;
    unsigned int changed_pixels_threshold;
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
    int boundaryLength;

    unsigned int motion_count;
    char filename1[200];
    char filename2[200];
    int fd;

} MOTION_DETECTION_T;
extern MOTION_DETECTION_T motionDetection;
extern pthread_mutex_t motionDetectionMutex;

void detection_config(unsigned int fps, unsigned int y_length, MMAL_QUEUE_T* queue);
void detection_threshold(unsigned int threshold);
void yuv_callback(MMAL_PORT_T*, MMAL_BUFFER_HEADER_T*);


#endif
