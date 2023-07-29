#ifndef MJPEG_H
#define MJPEG_H

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_port.h"

void mjpeg_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
void mjpeg_config(MMAL_QUEUE_T *queue);

#endif