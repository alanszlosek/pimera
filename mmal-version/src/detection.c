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

MOTION_DETECTION_T motion_detection;
pthread_mutex_t motion_detection_mutex;

// local variables to reduce need to dereference into settings struct
unsigned int settings_buffer_length;
int64_t detection_fps_step = 1000000;
MMAL_QUEUE_T *yuv_queue;

// we take abs of pixels between previous_frame and current_frame
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
int64_t detect_at = 1; // next frame number to test for motion
int64_t detecting_at = 0;

// globals to help iterate over pixel rows
uint8_t** ptr; // pointer to motion_detection.processing.pointers
unsigned int rows_processed;

// frame rate calculation variables
time_t previous_time = 0;
unsigned int fps = 0; // count frames per second to track hiccups
unsigned int fps_rate = 0;


inline void detection() {
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
    }
}

#ifdef PI_ZERO
// SIMD for PI Zero W saves roughly a millisecond
inline void detection_armv6() {
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
    }
}
#endif

#ifdef PI_THREE
inline void detection_neon() {
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
    }
}
#endif



void yuv_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    time_t current_time = time(NULL);
    fps++;

    if (current_time - previous_time > 0) {
        fps_rate = fps;
        fps = 0;
        previous_time = current_time;
    }

    // TODO: log pts with detection buffer so we can have h264 side
    // compare when saving to disk

    // TODO: refactor this without locking
    // perhaps increment local counter and compare buffer timestamp
    /*
    pthread_mutex_lock(&stats_mutex);
    stats.fps++;
    pthread_mutex_unlock(&stats_mutex);
    */

    clock_t begin, end;
    if (buffer->pts >= detect_at) {
        // Perform motion detection
        begin = clock();

        mmal_buffer_header_mem_lock(buffer);
        memcpy(motion_detection.current_frame, buffer->data, settings_buffer_length);
        mmal_buffer_header_mem_unlock(buffer);

        // just copy, then bail
        threshold_tally = 0;
        process = true;

        // schedule next detection
        //detect_at = yuv_frame_counter + motion_detection.detection_sleep;
        detecting_at = buffer->pts;
        detect_at = buffer->pts + motion_detection.detection_sleep;

        // reset pointers
        rows_processed = 0;
        ptr = motion_detection.processing.pointers;
        end = clock();

        //printf("YUV COPY. Time: %f FPS: %d\n", (double)(end - begin) / CLOCKS_PER_SEC, fps_rate);
    }
    
    if (process) {
        begin = clock();

        #ifdef PI_THREE
        detection_neon();
        #else
        #ifdef PI_ZERO
        detection_armv6();
        #else
        detection();
        #endif
        #endif

        end = clock();

        // Detection stopped at NULL separators, advance past them to prepare for next time
        ptr += 3;
        // If we've reached the end of our pointer data, there's nothing more to do
        if (ptr == NULL) {
            printf("Done with batches\n");
            process = false;
        }

        //printf("DETECTION. Pixel delta: %d threshold: %d Time: %f FPS: %d cnt: %d\n", threshold_tally, yuv_threshold, (double)(end - begin) / CLOCKS_PER_SEC, fps_rate, count);

        if (threshold_tally > yuv_threshold) {
            // we exceeded the threshold, don't need to process any more batches
            process = false;

            // if motion is detected, check again in 2 seconds
            // this ensures videos are long enough to be useful, and prevents churn
            // (ie. many short videos are hard to consume, so fewer slightly longer vids are better)
            //detect_at = yuv_frame_counter + detection_fps_step;
            detect_at = buffer->pts + detection_fps_step;

            h264_motion_detected(detecting_at);
            pthread_mutex_lock(&motion_detection_mutex);
            motion_detection.motion_count++;
            motion_detection.pixel_delta = threshold_tally;
            pthread_mutex_unlock(&motion_detection_mutex);
            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motion_detection.previous_frame, motion_detection.current_frame, settings_buffer_length);

        } else {
            pthread_mutex_lock(&motion_detection_mutex);
            // Note: is resetting this here causing video churn?
            motion_detection.motion_count = 0;
            motion_detection.pixel_delta = threshold_tally;
            // next detection is already scheduled so no need to update detect_at here
            pthread_mutex_unlock(&motion_detection_mutex);
        }
    }

    mmal_buffer_header_release(buffer);
    send_buffers_to_port(port, yuv_queue);
}

// TODO: accept settings and handles and extract local copies
void detection_config(unsigned int fps, unsigned int y_length, MMAL_QUEUE_T* queue) {
    // trying to prevent detection during period when we're saving a video that's
    // at least 2 seconds long anyway, but this might be hurting us given that we
    // spread processing out over 1/3 of a second
    detection_fps_step = 1000000;
    settings_buffer_length = y_length;
    // TODO: realloc current_frame and previous_frame buffers here
    yuv_queue = queue;
}
void detection_threshold(unsigned int threshold) {
    yuv_threshold = threshold;
}
