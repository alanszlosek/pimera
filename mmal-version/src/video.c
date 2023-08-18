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
uint32_t h264_file_kickoffLength;
bool saving = false;
unsigned int h264_frame_counter = 0;
unsigned int save_until = 0;
unsigned int h264_settings_fps = 0;

// TODO: change this to fixed list of MMAL buffers
uint8_t* h264_buffer;
size_t h264_buffer_length = 0;
//size_t h264_buffer_size = 0;

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
    h264_frame_counter++;
    
    // TODO: concat this buffer with previous one containing timestamp
    // see here: https://forums.raspberrypi.com/viewtopic.php?t=220074


    if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) {
        logInfo("Keeping h264 config data for later");

        if (buffer->length > 128) {
            logError("Not enough space in h264_file_kickoff", __func__);
        } else {
            mmal_buffer_header_mem_lock(buffer);
            memcpy(h264_file_kickoff, buffer->data, buffer->length);
            h264_file_kickoffLength = buffer->length;
            mmal_buffer_header_mem_unlock(buffer);
        }
        mmal_buffer_header_release(buffer);

        send_buffers_to_port(port, h264_queue);
        return;
    }

    if (saving) {
        mmal_buffer_header_mem_lock(buffer);
        write(video_fd, buffer->data, buffer->length);
        mmal_buffer_header_mem_unlock(buffer);

        // should we still be saving?
        // save up to 1 second past the last time motion was detected
        if (h264_frame_counter < save_until) {
            // leave open

        // only close once we see an end of the frame of data,
        // otherwise may have corrupt video files. sadly, still seeing
        // this from ffmpeg: "[h264 @ 0x5583838594c0] error while decoding MB 6 20, bytestream -22"
        } else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
            // TODO: do i need to wait for a certain type of data before closing?

            // save then close
            logInfo("CLOSING %s\n", video_file_temp);
            close(video_fd);
            video_fd = 0;

            rename(video_file_temp, video_file_final);
            saving = false;
        }
        

    } else {

        // TODO: refactor this a bit ... we end up doing unnecessary copying if there's motion

        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) {
            // keyframe might have invalid pts if frame data is split across more than 1 buffer
            // but first keyframe of data will have pts
            if (buffer->pts != MMAL_TIME_UNKNOWN) {
                // start at beginning of buffer ... we want to cache from the start of the keyframe
                h264_buffer_length = 0;
            }
        }


        // Do we need to open new file?
        //if (h264_motion) {
        // motion detected, we have a frame to record until
        if (h264_frame_counter < save_until) {
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

            logInfo("OPENING %s\n", video_file_temp);

            // TODO: handle failed open
            video_fd = open(video_file_temp, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
            write(video_fd, h264_file_kickoff, h264_file_kickoffLength);

            write(video_fd, h264_buffer, h264_buffer_length);
            h264_buffer_length = 0;

            mmal_buffer_header_mem_lock(buffer);
            write(video_fd, buffer->data, buffer->length);
            mmal_buffer_header_mem_unlock(buffer);

            saving = true;

        } else {
            // Since we haven't seen motion yet, buffer data starting from first keyframe of data
            mmal_buffer_header_mem_lock(buffer);
            memcpy(h264_buffer + h264_buffer_length, buffer->data, buffer->length);
            h264_buffer_length += buffer->length;
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

    h264_buffer = (uint8_t*) malloc(h264_buffer_size);
    if (!h264_buffer) {
        logError("FAILED TO ALLOCATE H264 BUFFER", __func__);
        // OF SIZE %u\n", h264_buffer_size);
    }
}

void h264_motion_detected() {
    save_until = h264_frame_counter + h264_settings_fps;

    // would like to set frame/recording cutoffs here instead
}
