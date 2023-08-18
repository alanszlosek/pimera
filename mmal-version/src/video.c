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
char video_file1[256];
char video_file2[256];
int video_fd;
bool h264_motion = false;

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

bool saving = 0;
char h264FileKickoff[128];
uint32_t h264FileKickoffLength;
void h264_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    struct timespec ts;
    
    // TODO: concat this buffer with previous one containing timestamp
    // see here: https://forums.raspberrypi.com/viewtopic.php?t=220074


    if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) {
        logInfo("Keeping h264 config data for later");

        if (buffer->length > 128) {
            logError("Not enough space in h264_file_kickoff", __func__);
        } else {
            //pthread_mutex_lock(&userdataMutex);
            mmal_buffer_header_mem_lock(buffer);
            memcpy(h264FileKickoff, buffer->data, buffer->length);
            h264FileKickoffLength = buffer->length;
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

        // still have motion?
        if (h264_motion) {
            // leave open

        // only close once we see an end of the frame of data,
        // otherwise may have corrupt video files. sadly, still seeing
        // this from ffmpeg: "[h264 @ 0x5583838594c0] error while decoding MB 6 20, bytestream -22"
        } else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
            // TODO: do i need to wait for a certain type of data before closing?

            // save then close
            logInfo("CLOSING %s\n", video_file1);
            close(video_fd);
            video_fd = 0;

            rename(video_file1, video_file2);
            saving = 0;
        }
        // TODO: don't need mutex around file descriptor
        
        mmal_buffer_header_release(buffer);

    } else {

        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) {
            // if first keyframe buffer, which should have pts
            if (buffer->pts != MMAL_TIME_UNKNOWN) {
                // keyframe might have invalid pts if frame data is split across more than 1 buffer
                // but will have pts for first frame

                // clear out and start fresh;
                /*
                for (int i = 0; i < h264_buffer_length; i++) {
                    mmal_buffer_header_release( h264_buffers[i] );
                }
                */
                h264_buffer_length = 0;
            }

            mmal_buffer_header_mem_lock(buffer);
            //h264_buffers[ h264_buffer_length ] = buffer;
            //h264_buffer_length++;
            memcpy(h264_buffer + h264_buffer_length, buffer->data, buffer->length);
            h264_buffer_length += buffer->length;
            mmal_buffer_header_mem_unlock(buffer);

        } else {
            mmal_buffer_header_mem_lock(buffer);
            //h264_buffers[ h264_buffer_length ] = buffer;
            //h264_buffer_length++;
            memcpy(h264_buffer + h264_buffer_length, buffer->data, buffer->length);
            h264_buffer_length += buffer->length;
            mmal_buffer_header_mem_unlock(buffer);
        }
        mmal_buffer_header_release(buffer);

        // Do we need to open new file?
        if (h264_motion) {
            // open file
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm *timeinfo = localtime(&ts.tv_sec);
            char datetime[16];
            strftime(datetime, sizeof(datetime), "%Y%m%d%H%M%S", timeinfo);

            // prepare temporary and final filenames
    
            // temporary so our h264 processing pipeline doesn't grab
            // a file that's still being written to
            snprintf(video_file1, 255, video_file_pattern, datetime);
            strncpy(video_file2, video_file1, sizeof(video_file1));
            // now append differing extensions
            strcat(video_file1, "_h264");
            strcat(video_file2, "h264");

            logInfo("OPENING %s\n", video_file1);

            video_fd = open(video_file1, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);

            write(video_fd, h264FileKickoff, h264FileKickoffLength);
            /*
            for (int i = 0; i < h264_buffer_length; i++) {
                MMAL_BUFFER_HEADER_T *b = h264_buffers[i];
                mmal_buffer_header_mem_lock(b);
                write(video_fd, b->data, b->length);
                mmal_buffer_header_mem_unlock(b);
                mmal_buffer_header_release( b );
            }
            */
            write(video_fd, h264_buffer, h264_buffer_length);
            h264_buffer_length = 0;

            saving = 1;
        }
        
    }

    send_buffers_to_port(port, h264_queue);
}

void h264_config(MMAL_QUEUE_T *queue, char* path, size_t h264_buffer_size) {
    h264_queue = queue;
    video_file_pattern = path;

    h264_buffer = (uint8_t*) malloc(h264_buffer_size);
    if (!h264_buffer) {
        logError("FAILED TO ALLOCATE H264 BUFFER", __func__);
        // OF SIZE %u\n", h264_buffer_size);
    }
}

void h264_motion_detected(bool yes) {
    h264_motion = yes;

    // would like to set frame/recording cutoffs here instead
}