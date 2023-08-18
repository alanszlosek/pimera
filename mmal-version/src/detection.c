#ifdef PI_THREE
#include <arm_neon.h>
#endif

#ifdef PI_ZERO
#include <arm_acle.h>
#endif

#include <stdbool.h>
#include <stdio.h>

#include "camera.h"
#include "detection.h"
#include "video.h"

MOTION_DETECTION_T motionDetection;
pthread_mutex_t motionDetectionMutex;

// local variables to reduce need to dereference into settings struct
unsigned int settings_buffer_length;
unsigned int settings_fps;
MMAL_QUEUE_T *yuv_queue;

// we take abs of pixels between previousFrame and currentFrame
// but consider 40 of that to be noise
// if it's above 40 we consider that pixel as having changed
unsigned int threshold_tally;
// if total is above yuv_threshold we consider that to be motion
unsigned int yuv_threshold = 900000;
// if abs diff of pixels is above this, the pixel has seen motion
#define ABS_THRESHOLD 40
#define NEON_BATCH 16
#define ARMV6_BATCH 4


bool process = false;
unsigned int yuv_frame_counter = 0;
unsigned int detect_at = 1; // next frame number to test for motion

// globals to help iterate over pixel rows
uint8_t** ptr; // pointer to motionDetection.processing.pointers
unsigned int rows_processed;

// frame rate calculation variables
time_t previous_time = 0;
unsigned int fps = 0; // count frames per second to track hiccups
unsigned int fps_rate = 0;


inline unsigned int detection() {
    unsigned int count = 0;
    for (; *ptr != NULL; ptr += 3) {
        uint8_t* c = *ptr;
        uint8_t* c_end = *(ptr + 1);
        uint8_t* p = *(ptr + 2);

        for (; c < c_end; c++, p++) {
            uint8_t delta = abs(*c - *p);
            if (delta > ABS_THRESHOLD) {
                threshold_tally++;
            }
        }
        count++;
    }
    return count;
}

#ifdef PI_ZERO
// SIMD for PI Zero W saves roughly a millisecond
inline unsigned int detection_armv6() {
    unsigned int count = 0;

    uint8x4_t simd1;
    uint8x4_t simd2;
    uint8x4_t higher;
    uint8x4_t lower;
    uint8x4_t result;
    uint8_t* out;

    for (; *ptr != NULL; ptr += 3) {
        uint8_t* c = *ptr;
        uint8_t* c_end = *(ptr + 1);
        uint8_t* p = *(ptr + 2);

        for (; c < c_end; c += ARMV6_BATCH, p += ARMV6_BATCH) {

            simd1 = *((uint8x4_t*) c);
            simd2 = *((uint8x4_t*) p);

            result = __usub8(simd1, simd2);
            higher = __sel(simd1, simd2);
            lower = __sel(simd2, simd1);
            result = __usub8(higher, lower);

            // TODO: does this work?
            out = (uint8_t*) &result;
            if (out[0] > ABS_THRESHOLD) {
                threshold_tally++;
            }
            if (out[1] > ABS_THRESHOLD) {
                threshold_tally++;
            }
            if (out[2] > ABS_THRESHOLD) {
                threshold_tally++;
            }
            if (out[3] > ABS_THRESHOLD) {
                threshold_tally++;
            }
        }
        count++;
    }
    return count;
}
#endif

#ifdef PI_THREE
inline unsigned int detection_neon() {
    unsigned int count = 0;
    uint8x16_t a, b, absdiff;
    for (; *ptr != NULL; ptr += 3) {
        uint8_t* c = *ptr;
        uint8_t* c_end = *(ptr + 1);
        uint8_t* p = *(ptr + 2);

        for (; c < c_end; c += NEON_BATCH, p += NEON_BATCH) {
            a = vld1q_u8(c);
            b = vld1q_u8(p);
            // int absdiff = abs(*c - *p);
            absdiff = vabdq_u8(a, b);
            uint8_t result[NEON_BATCH];
            vst1q_u8(result, absdiff);
            for (int j = 0; j < NEON_BATCH; j++) {
                if (result[j] > ABS_THRESHOLD) { // & 1) {
                    threshold_tally+=1;
                }
            }
        }
        count++;
    }
    return count;
}
#endif



void yuv_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    time_t current_time = time(NULL);
    yuv_frame_counter++;
    fps++;

    if (current_time - previous_time > 0) {
        fps_rate = fps;
        fps = 0;
        previous_time = current_time;
    }

    // TODO: refactor this without locking
    // perhaps increment local counter and compare buffer timestamp
    /*
    pthread_mutex_lock(&statsMutex);
    stats.fps++;
    pthread_mutex_unlock(&statsMutex);
    */

    clock_t begin, end;
    if (yuv_frame_counter == detect_at) {
        // Perform motion detection
        begin = clock();

        mmal_buffer_header_mem_lock(buffer);
        memcpy(motionDetection.currentFrame, buffer->data, settings_buffer_length);
        mmal_buffer_header_mem_unlock(buffer);

        // just copy, then bail
        threshold_tally = 0;
        process = true;

        // schedule next detection
        detect_at = yuv_frame_counter + motionDetection.detection_sleep;

        // reset pointers
        rows_processed = 0;
        ptr = motionDetection.processing.pointers;
        end = clock();

        //printf("YUV COPY. Time: %f FPS: %d\n", (double)(end - begin) / CLOCKS_PER_SEC, fps_rate);
    }
    
    if (process) {
        begin = clock();
        unsigned int count = 0;

        #ifdef PI_THREE
        count = detection_neon();
        #else
        #ifdef PI_ZERO
        count = detection_armv6();
        #else
        count = detection();
        #endif
        #endif

        // advance past NULL separators
        ptr += 3;
        if (ptr == NULL) {
            // Done
            printf("Done with batches\n");
        }

        end = clock();

        //printf("DETECTION. Pixel delta: %d threshold: %d Time: %f FPS: %d cnt: %d\n", threshold_tally, yuv_threshold, (double)(end - begin) / CLOCKS_PER_SEC, fps_rate, count);

        if (threshold_tally > yuv_threshold) {
            // we exceeded the threshold, don't need to process any more batches
            process = false;

            // if motion is detected, check again in 2 seconds
            detect_at = yuv_frame_counter - 1 + (settings_fps * 2);

            h264_motion_detected(true);
            pthread_mutex_lock(&motionDetectionMutex);
            motionDetection.motion_count++;
            motionDetection.pixel_delta = threshold_tally;
            pthread_mutex_unlock(&motionDetectionMutex);
            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, motionDetection.currentFrame, settings_buffer_length);

        } else {
            h264_motion_detected(false);
            pthread_mutex_lock(&motionDetectionMutex);
            motionDetection.motion_count = 0;
            motionDetection.pixel_delta = threshold_tally;
            // next detection is already scheduled so no need to update detect_at here
            pthread_mutex_unlock(&motionDetectionMutex);
        }

    }

    mmal_buffer_header_release(buffer);
    send_buffers_to_port(port, yuv_queue);
}

// TODO: accept settings and handles and extract local copies
void detection_config(unsigned int fps, unsigned int y_length, MMAL_QUEUE_T* queue) {
    settings_fps = fps;
    settings_buffer_length = y_length;
    yuv_queue = queue;
}
void detection_threshold(unsigned int threshold) {
    yuv_threshold = threshold;
}
