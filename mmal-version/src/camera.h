#ifndef CAMERA_H
#define CAMERA_H

#define EX_ERROR 1
#define CAMERA_VIDEO_PORT 1
#define SPLITTER_H264_PORT 0
#define SPLITTER_MJPEG_PORT 1
#define SPLITTER_YUV_PORT 2

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_port.h"
#include "settings.h"

void send_buffers_to_port(MMAL_PORT_T*, MMAL_QUEUE_T*);

void cameraControlCallback(MMAL_PORT_T*, MMAL_BUFFER_HEADER_T*);

void destroy_camera(MMAL_COMPONENT_T*);

MMAL_STATUS_T create_camera(MMAL_COMPONENT_T**, SETTINGS*);

MMAL_STATUS_T create_splitter(MMAL_COMPONENT_T**, MMAL_CONNECTION_T**, MMAL_PORT_T*, int);

void destroy_splitter(MMAL_COMPONENT_T*, MMAL_CONNECTION_T*);

MMAL_STATUS_T create_resizer(MMAL_COMPONENT_T**, MMAL_CONNECTION_T**, MMAL_PORT_T*, MJPEG_SETTINGS*);

void destroy_resizer(MMAL_COMPONENT_T *, MMAL_CONNECTION_T *);

void destroy_mjpeg_encoder(MMAL_COMPONENT_T *, MMAL_CONNECTION_T *, MMAL_POOL_T *);

MMAL_STATUS_T create_mjpeg_encoder(MMAL_COMPONENT_T **, MMAL_POOL_T **, MMAL_CONNECTION_T **, MMAL_PORT_T *, MMAL_PORT_BH_CB_T , MJPEG_SETTINGS *);

void destroy_h264_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool);

MMAL_STATUS_T create_h264_encoder(MMAL_COMPONENT_T **, MMAL_POOL_T **, MMAL_CONNECTION_T **, MMAL_PORT_T *, MMAL_PORT_BH_CB_T , H264_SETTINGS *);

void disable_port(MMAL_PORT_T *, char const* );

#endif