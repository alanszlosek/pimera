#include <stdio.h>

#include "camera.h"
#include "detection.h"

MOTION_DETECTION_T motionDetection;
pthread_mutex_t motionDetectionMutex;

// local variables to reduce need to dereference into settings struct
unsigned int settings_buffer_length;
unsigned int settings_fps;
MMAL_QUEUE_T *yuv_queue;
// currently we take pixel deltas between frames if they're above 50,
// and add them all together.
// if total is above yuv_threshold we consider that to be motion
unsigned int yuv_threshold = 900000;

// for first iteration, this just copies into previous buffer
int8_t detection_row_batch = -1;
unsigned int threshold_tally;
unsigned int yuv_frame_counter = 0;

time_t previous_time = 0;
unsigned int fps = 0; // count frames per second to track hiccups
unsigned int fps_rate = 0;
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
    if (yuv_frame_counter >= motionDetection.detection_at) {
        // Perform motion detection
        begin = clock();

        mmal_buffer_header_mem_lock(buffer);
        memcpy(motionDetection.yuvBuffer, buffer->data, settings_buffer_length);
        mmal_buffer_header_mem_unlock(buffer);

        uint8_t* p = motionDetection.previousFrame;
        uint8_t* c = motionDetection.yuvBuffer;

        // Save to file
        /*
        char filename[255];
        FILE* f;
        snprintf(filename, 200, "/home/pi/yuv/%d.yuv", yuv_frame_counter);
        printf("Saving YUV to %s\n", filename);
        f = fopen(filename, "w+");
        fwrite(c, 1, settings->mjpeg.y_length, f);
        fclose(f);
        */

        detection_row_batch = 1;
        threshold_tally = 0;
        /*
        for regions we really just need:
        start offset
        length of row
        stride to next row from offset
        how many rows
        */
        // TODO: i think i'm looping over these wrong
        // seems data might be in different order
        for (int row = 0; row < motionDetection.region.row_batch_size; row++) {
            unsigned int offset = motionDetection.region.offset + (motionDetection.region.stride * row);
            uint8_t *c_start = c + offset;
            uint8_t *p_start = p + offset;
            uint8_t *c_end = c_start + motionDetection.region.row_length;

            // TODO: add SIMD ops and stride
            for (; c_start < c_end; c_start++, p_start++) {
                uint8_t delta = abs(*c_start - *p_start);
                if (delta > 50) {
                    threshold_tally++;
                }
            }
        }
        mmal_buffer_header_mem_unlock(buffer);
        end = clock();

        //printf("DETECTION1. Pixel delta: %d threshold: %d Time: %f FPS: %d\n", threshold_tally, yuv_threshold, (double)(end - begin) / CLOCKS_PER_SEC, fps_rate);

        if (threshold_tally > yuv_threshold) {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: can still lock around here, but don't need lock above
            motionDetection.motion_count++;
            motionDetection.pixel_delta = threshold_tally;
            // if motion is detected, check again in 2 seconds
            motionDetection.detection_at = yuv_frame_counter + (settings_fps * 2);
            pthread_mutex_unlock(&motionDetectionMutex);

            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, buffer->data, settings_buffer_length);

            // skip second half
            detection_row_batch = 0;
        } else {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: if not reach threshold, set detection_at to yuv_frame_counter+1
            // so we process rest of lines in next callback
            motionDetection.motion_count = 0;
            motionDetection.pixel_delta = threshold_tally;
            motionDetection.detection_at = yuv_frame_counter + motionDetection.detection_sleep;
            pthread_mutex_unlock(&motionDetectionMutex);
        }


    // TODO: verify that detection_row_batch and the loop below is correct
    } else if (detection_row_batch > 0) {
        detection_row_batch++;
        begin = clock();
        
        uint8_t* p = motionDetection.previousFrame;
        uint8_t* c = motionDetection.yuvBuffer;
        // TODO: i'm so tired, hard to think about this
        for (
            int row = 0,
            row_number = motionDetection.region.row_batch_size * detection_row_batch;

            row < motionDetection.region.row_batch_size &&
            row_number < motionDetection.region.num_rows;
            
            row++,
            row_number++
        ) {
            unsigned int offset = motionDetection.region.offset + (motionDetection.region.stride * row_number);
            uint8_t *c_start = c + offset;
            uint8_t *p_start = p + offset;
            uint8_t *c_end = c_start + motionDetection.region.row_length;

            for (; c_start < c_end; c_start++, p_start++) {
                uint8_t delta = abs(*c_start - *p_start);
                if (delta > 50) {
                    threshold_tally++;
                }
            }
        }
        if (detection_row_batch == motionDetection.region.batches) {
            // no more batches
            detection_row_batch = 0;
        }
        end = clock();

        //printf("DETECTION2. Pixel delta: %d threshold: %d Time: %f FPS: %d\n", threshold_tally, yuv_threshold, (double)(end - begin) / CLOCKS_PER_SEC, fps_rate);

        // TODO: refactor this to compare delta against non-mutex threshold
        if (threshold_tally > yuv_threshold) {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: can still lock around here, but don't need lock above
            motionDetection.motion_count++;
            motionDetection.pixel_delta = threshold_tally;
            // if motion is detected, check again in 2 seconds
            motionDetection.detection_at = yuv_frame_counter - 1 + (settings_fps * 2);
            pthread_mutex_unlock(&motionDetectionMutex);
            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, motionDetection.yuvBuffer, settings_buffer_length);

        } else {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: if not reach threshold, set detection_at to yuv_frame_counter+1
            // so we process rest of lines in next callback
            motionDetection.motion_count = 0;
            motionDetection.pixel_delta = threshold_tally;
            // this should already be set correctly
            //motionDetection.detection_at = yuv_frame_counter + motionDetection.detection_sleep;
            pthread_mutex_unlock(&motionDetectionMutex);
        }

    } else if (detection_row_batch == -1) {
        mmal_buffer_header_mem_lock(buffer);
        // First run, copy into previous
        memcpy(motionDetection.previousFrame, buffer->data, settings_buffer_length);
        mmal_buffer_header_mem_unlock(buffer);
        mmal_buffer_header_release(buffer);
        send_buffers_to_port(port, yuv_queue);
        // next time will compare rows
        detection_row_batch = 1;
        return;
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