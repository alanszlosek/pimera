#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>

#include "camera.h"
#include "log.h"
#include "video.h"


MMAL_QUEUE_T *h264_queue = NULL;
char* video_file_pattern;
char video_file_temp[256];
char video_file_final[256];
int video_fd;
char h264_file_kickoff[128];
uint32_t h264_file_kickoff_length;
bool saving = false;
int64_t save_until = 0;
unsigned int h264_settings_fps = 0;

// TODO: change this to 2 buffers, circular buffer fashion, 
// where we keep the previous keyframe window, and the current one
#define NUM_H264_BUFFERS 3
typedef struct {
    int64_t pts;
    uint8_t* buffer;
    size_t length;
    size_t sz;
} H264_BUFFER;
H264_BUFFER h264_buffers[NUM_H264_BUFFERS];
size_t h264_buffer_offset = 0;
H264_BUFFER* h264_buffer = h264_buffers;

void h264_advance_buffer() {
    H264_BUFFER *b;
    h264_buffer_offset++;
    if (h264_buffer_offset == NUM_H264_BUFFERS) {
        h264_buffer_offset = 0;
    }
    h264_buffer = h264_buffers + h264_buffer_offset;
    h264_buffer->length = 0;
}

void h264BufferDebug(MMAL_BUFFER_HEADER_T* buffer) {
    fprintf(
        stdout, "[INFO] frame flags: %8s %8s %8s %8s %12s %10s %10s %10s %8s %10d %10d\n",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS) ? "eos" : "!eos",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_START) ? "start" : "!start",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) ? "end" : "!end",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME) ? "frame" : "!frame",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) ? "keyframe" : "!keyframe",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) ? "config" : "!config",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) ? "sideinfo" : "!sideinfo",
        (buffer->flags & MMAL_BUFFER_HEADER_FLAG_NAL_END) ? "nalend" : "!nalend",
        (buffer->pts != MMAL_TIME_UNKNOWN) ? "pts" : "!pts",
        buffer->offset,
        buffer->length
    );
}

void h264_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    struct timespec ts;
    
    // TODO: concat this buffer with previous one containing timestamp
    // see here: https://forums.raspberrypi.com/viewtopic.php?t=220074


    if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) {
        log_info("Keeping h264 config data for later");

        if (buffer->length > 128) {
            log_error("Not enough space in h264_file_kickoff", __func__);
        } else {
            mmal_buffer_header_mem_lock(buffer);
            memcpy(h264_file_kickoff, buffer->data, buffer->length);
            h264_file_kickoff_length = buffer->length;
            mmal_buffer_header_mem_unlock(buffer);
        }
        mmal_buffer_header_release(buffer);

        send_buffers_to_port(port, h264_queue);
        return;
    }

    if (saving) {
        // If we are past the save_until time, and we just saw another keyframe, close and stop saving.
        // This ensures we can cleanly start queueing again
        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) {
            if (buffer->pts != MMAL_TIME_UNKNOWN && buffer->pts > save_until) {
                log_info("CLOSING %s\n", video_file_temp);
                close(video_fd);
                video_fd = 0;
        
                rename(video_file_temp, video_file_final);
                saving = false;

                // Put current buffer into new queue
                h264_advance_buffer();
                h264_buffer->pts = buffer->pts;
                mmal_buffer_header_mem_lock(buffer);
                memcpy(h264_buffer->buffer + h264_buffer->length, buffer->data, buffer->length);
                h264_buffer->length += buffer->length;
                mmal_buffer_header_mem_unlock(buffer);


            } else {
                // keep saving
                mmal_buffer_header_mem_lock(buffer);
                write(video_fd, buffer->data, buffer->length);
                mmal_buffer_header_mem_unlock(buffer);
            }
        } else {
            // keep saving
            mmal_buffer_header_mem_lock(buffer);
            write(video_fd, buffer->data, buffer->length);
            mmal_buffer_header_mem_unlock(buffer);
        }
        

    } else { // not saving

        // TODO: refactor this a bit ... we end up doing unnecessary copying if there's motion

        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) {
            // keyframe might have invalid pts if frame data is split across more than 1 buffer
            // but first keyframe of data will have pts
            if (buffer->pts != MMAL_TIME_UNKNOWN) {

                // Go to next circular buffer slot
                h264_advance_buffer();
                h264_buffer->pts = buffer->pts;
            }
        }


        // Do we need to open new file?
        //if (h264_motion) {
        // motion detected, we have a frame to record until
        if (buffer->pts != MMAL_TIME_UNKNOWN && buffer->pts < save_until) {
            // open file
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm *timeinfo = localtime(&ts.tv_sec);
            char datetime[16];
            strftime(datetime, sizeof(datetime), "%Y%m%d%H%M%S", timeinfo);

            // prepare temporary and final filenames
            // temporary so our h264 processing pipeline doesn't grab a file that's still being written to
            snprintf(video_file_temp, 255, video_file_pattern, datetime);
            strncpy(video_file_final, video_file_temp, sizeof(video_file_temp));
            // now append differing extensions
            strcat(video_file_temp, "_h264");
            strcat(video_file_final, "h264");

            log_info("OPENING %s\n", video_file_temp);

            // TODO: handle failed open
            video_fd = open(video_file_temp, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
            write(video_fd, h264_file_kickoff, h264_file_kickoff_length);

            // How do we know which circular buffer to start flushing to disk? 
            write(video_fd, h264_buffer->buffer, h264_buffer->length);

            mmal_buffer_header_mem_lock(buffer);
            write(video_fd, buffer->data, buffer->length);
            mmal_buffer_header_mem_unlock(buffer);

            saving = true;

        } else {

            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) {
                // keyframe might have invalid pts if frame data is split across more than 1 buffer
                // but first keyframe of data will have pts
                if (buffer->pts != MMAL_TIME_UNKNOWN) {
                    h264_advance_buffer();
                    h264_buffer->pts = buffer->pts;
                }
            }


            // Since we haven't seen motion yet, buffer data starting from first keyframe of data
            mmal_buffer_header_mem_lock(buffer);
            memcpy(h264_buffer->buffer + h264_buffer->length, buffer->data, buffer->length);
            h264_buffer->length += buffer->length;
            mmal_buffer_header_mem_unlock(buffer);
        }

    }
    mmal_buffer_header_release(buffer);

    send_buffers_to_port(port, h264_queue);
}

void h264_config(unsigned int fps, MMAL_QUEUE_T *queue, char* path, size_t h264_buffer_size) {
    h264_settings_fps = fps;
    h264_queue = queue;
    video_file_pattern = path;    

    for (size_t i = 0; i < NUM_H264_BUFFERS; i++) {
        h264_buffers[i].pts = 0;
        h264_buffers[i].buffer = (uint8_t*) malloc(h264_buffer_size);
        h264_buffers[i].length = 0;
        h264_buffers[ i ].sz = h264_buffer_size;

        if (!h264_buffers[i].buffer) {
            log_error("FAILED TO ALLOCATE H264 BUFFER", __func__);
            // OF SIZE %u\n", h264_buffer_size);
        }
    }
}

void h264_motion_detected(int64_t pts) {
    // we check for motion in 1 second, so stop recording after 2
    save_until = pts + 2000000;
    //printf("h264_frame_counter: %d. Setting save_until: %d\n", h264_frame_counter, save_until);

    // would like to set frame/recording cutoffs here instead
}
