#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_parameters_camera.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"

typedef struct HANDLES_S HANDLES;
typedef struct SETTINGS_S SETTINGS;
typedef struct H264_SETTINGS_S H264_SETTINGS;
typedef struct MJPEG_SETTINGS_S MJPEG_SETTINGS;

struct H264_SETTINGS_S {
    unsigned int width;
    unsigned int height;
    int vcosWidth;
    int vcosHeight;
    unsigned int y_length;

    unsigned int fps; // default 30
    unsigned int keyframePeriod; // default 1000
};
struct MJPEG_SETTINGS_S {
    unsigned int width;
    unsigned int height;
    int vcosWidth;
    int vcosHeight;
    unsigned int y_length;
};
struct SETTINGS_S {
    int width; // default 1920
    int height; // default 1080
    int vcosWidth;
    int vcosHeight;
    unsigned int streamDenominator;

    H264_SETTINGS h264;
    MJPEG_SETTINGS mjpeg;

    unsigned int region[4];

    
    int motion_check_frequency; // default 3 per second
    unsigned int pixel_delta_threshold;
    unsigned int threshold;
    char objectDetectionEndpoint[128];
    int verbose;
    bool debug;

    int sharpness;
    int contrast;
    int brightness;
    int saturation;
    int iso;
    int videoStabilisation;
    int exposureCompensation;
    MMAL_PARAM_EXPOSUREMODE_T exposureMode;
    MMAL_PARAM_FLICKERAVOID_T flickerAvoidMode;
    MMAL_PARAM_AWBMODE_T awbMode;

    char hostname[100];
    char videoPath[100];
};

#endif