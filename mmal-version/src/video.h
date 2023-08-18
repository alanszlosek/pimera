#ifndef VIDEO_H
#define VIDEO_H

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_port.h"

void h264_callback(MMAL_PORT_T*, MMAL_BUFFER_HEADER_T*);
void h264_config(MMAL_QUEUE_T*, char*, size_t);
void h264_motion_detected(bool);

#endif