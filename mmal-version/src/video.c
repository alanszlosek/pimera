#include "video.h"


void sendH264Buffers(MMAL_PORT_T* port, CALLBACK_USERDATA* userdata) {
    // TODO: move this to main thread
    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T* new_buffer;
        while ( (new_buffer = mmal_queue_get(userdata->handles->h264_encoder_pool->queue)) ) {
            status = mmal_port_send_buffer(port, new_buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed, no buffer to return to h264 encoder port\n", __func__);
                break;
            }
        }
    }
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

bool saving = 0;
char h264FileKickoff[128];
uint32_t h264FileKickoffLength;
void h264Callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata;
    SETTINGS *settings;
    struct timespec ts;
    bool debug;


    if (!port->userdata) {
        logError("Did not find userdata in h264 callback", __func__);
        debug = false;
    } else {
        userdata = (CALLBACK_USERDATA *)port->userdata;
        settings = userdata->settings;
        debug = settings->debug;
    }

    //h264BufferDebug(buffer);

    if (buffer->cmd) {
        logInfo("Found cmd in h264 buffer. Releasing");
        mmal_buffer_header_release(buffer);
        sendH264Buffers(port, userdata);
        return;
    }
    // is this necessary during shutdown to prevent a bunch of callbacks with messed up data?
    if (!buffer->length) {
        mmal_buffer_header_release(buffer);
        sendH264Buffers(port, userdata);
        return;
    }
    
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

        sendH264Buffers(port, userdata);
        return;
    }

    if (saving) {
        mmal_buffer_header_mem_lock(buffer);
        write(motionDetection.fd, buffer->data, buffer->length);
        mmal_buffer_header_mem_unlock(buffer);

        // still have motion?
        pthread_mutex_lock(&motionDetectionMutex);
        if (motionDetection.motion_count) {
            // leave open

        // only close once we see an end of the frame of data,
        // otherwise may have corrupt video files. sadly, still seeing
        // this from ffmpeg: "[h264 @ 0x5583838594c0] error while decoding MB 6 20, bytestream -22"
        } else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
            // TODO: do i need to wait for a certain type of data before closing?

            // save then close
            logInfo("CLOSING %s\n", motionDetection.filename1);
            close(motionDetection.fd);
            motionDetection.fd = 0;

            rename(motionDetection.filename1, motionDetection.filename2);
            saving = 0;
        }
        // TODO: don't need mutex around file descriptor
        
        pthread_mutex_unlock(&motionDetectionMutex);
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
        pthread_mutex_lock(&motionDetectionMutex);
        if (motionDetection.motion_count) {
            // open file
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            struct tm *timeinfo = localtime(&ts.tv_sec);
            char datetime[16];
            strftime(datetime, sizeof(datetime), "%Y%m%d%H%M%S", timeinfo);

            // prepare temporary and final filenames
    
            // temporary so our h264 processing pipeline doesn't grab
            // a file that's still being written to
            snprintf(motionDetection.filename1, sizeof(motionDetection.filename1), "%s/%s_%s_%dx%dx%d.", settings->videoPath, datetime, settings->hostname, settings->width, settings->height, settings->h264.fps);
            strncpy(motionDetection.filename2, motionDetection.filename1, sizeof(motionDetection.filename1));
            // now append differing extensions
            strcat(motionDetection.filename1, "_h264");
            strcat(motionDetection.filename2, "h264");

            logInfo("OPENING %s\n", motionDetection.filename1);

            motionDetection.fd = open(motionDetection.filename1, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);

            write(motionDetection.fd, h264FileKickoff, h264FileKickoffLength);
            /*
            for (int i = 0; i < h264_buffer_length; i++) {
                MMAL_BUFFER_HEADER_T *b = h264_buffers[i];
                mmal_buffer_header_mem_lock(b);
                write(motionDetection.fd, b->data, b->length);
                mmal_buffer_header_mem_unlock(b);
                mmal_buffer_header_release( b );
            }
            */
            write(motionDetection.fd, h264_buffer, h264_buffer_length);
            h264_buffer_length = 0;

            saving = 1;
        }
        pthread_mutex_unlock(&motionDetectionMutex);
        
    }

    sendH264Buffers(port, userdata);
}

// END H264 FUNCTIONS