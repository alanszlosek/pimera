#ifndef DETECTION_H
#define DETECTION_H

#include <inttypes.h>
#include <pthread.h>

typedef struct {
    int detection_sleep;
    int detection_at;
    int stream_sleep;
    uint8_t *previousFrame;

    // holds yuv data so we can spread detection across multiple yuvCallback() calls
    uint8_t* yuvBuffer;
    unsigned int changed_pixels_threshold;
    unsigned int pixel_delta;
    struct {
        unsigned int offset;
        unsigned int row_length;
        unsigned int stride;
        unsigned int batches;
        unsigned int row_batch_size;
        unsigned int num_rows;
    } region;

    char boundary[81];
    int boundaryLength;

    unsigned int motion_count;
    char filename1[200];
    char filename2[200];
    int fd;

} MOTION_DETECTION_T;
extern MOTION_DETECTION_T motionDetection;
extern pthread_mutex_t motionDetectionMutex;

#endif