#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>


#include "bcm_host.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_parameters_camera.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/vcos/vcos.h"


#define INI_IMPLEMENTATION
#include "ini.h"
#include "utlist.h"

#include "detection.h"
#include "http.h"
#include "log.h"
#include "shared.h"
#include "settings.h"


#define CAMERA_VIDEO_PORT 1
#define SPLITTER_H264_PORT 0
#define SPLITTER_MJPEG_PORT 1
#define SPLITTER_YUV_PORT 2

#define ERROR_CHECK_INTERVAL 10 //100
#define DESIRED_OUTPUT_BUFFERS 3
#define EX_ERROR 1
#define MJPEG_BITRATE 25000000 // 25Mbits/s
#define H264_BITRATE  25000000 // 62.5Mbit/s OR 25000000 25Mbits/s
#define FRAME_ANNOTATION_TEXT_SIZE 24

#define DEBUG 1

pthread_mutex_t running_mutex;
bool running = 1;
pthread_mutex_t restart_mutex;
int restart = 0;

// Used by yuvCallback()
unsigned int pixel_delta_threshold = 4000;

// TODO: change this to fixed list of MMAL buffers
uint8_t* h264_buffer;
size_t h264_buffer_length = 0;
size_t h264_buffer_size = 0;

//MMAL_BUFFER_HEADER_T* h264_buffers[ 60 ];


typedef struct {
    uint8_t fps;
} STATS_T;
STATS_T stats;
pthread_mutex_t statsMutex;


unsigned int frame_counter = 0;


// end new processing variables

// HTTP CONNECTIONS


// Restart camera


typedef struct HANDLES_S HANDLES;
typedef struct SETTINGS_S SETTINGS;
typedef struct H264_SETTINGS_S H264_SETTINGS;
typedef struct MJPEG_SETTINGS_S MJPEG_SETTINGS;

// https://stackoverflow.com/questions/1675351/typedef-struct-vs-struct-definitions/
typedef struct {
    HANDLES *handles;
    SETTINGS *settings;
    char h264FileKickoff[128];
    uint32_t h264FileKickoffLength;
    int abort;
} CALLBACK_USERDATA;



struct HANDLES_S {
    MMAL_COMPONENT_T *camera;    /// Pointer to the camera component

    MMAL_COMPONENT_T *splitter;  /// Pointer to the splitter component
    MMAL_COMPONENT_T *full_splitter; // Pointer to splitter
    MMAL_COMPONENT_T *resized_splitter; // Pointer to splitter
    MMAL_CONNECTION_T *full_splitter_connection;/// Pointer to the connection from camera to splitter
    MMAL_CONNECTION_T *resized_splitter_connection;/// Pointer to the connection from camera to splitter

    MMAL_COMPONENT_T *resizer;
    MMAL_CONNECTION_T *resizer_connection;

    MMAL_PORT_T* splitterYuvPort;

    MMAL_COMPONENT_T *h264_encoder;   /// Pointer to the encoder component
    MMAL_COMPONENT_T *mjpeg_encoder;   /// Pointer to the encoder component
    MMAL_CONNECTION_T *h264_encoder_connection; /// Pointer to the connection from camera to encoder
    MMAL_CONNECTION_T *mjpeg_encoder_connection;

    MMAL_PORT_T* h264EncoderOutputPort;

    MMAL_POOL_T *h264_encoder_pool; /// Pointer to the pool of buffers used by splitter output port 0
    MMAL_POOL_T *mjpeg_encoder_pool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *yuvPool; /// Pointer to the pool of buffers used by encoder output port

    SETTINGS* settings;
    CALLBACK_USERDATA* h264CallbackUserdata;        /// Used to move data to the encoder callback
};




void reconfigureRegion(SETTINGS* settings) {
    unsigned int x_start, x_end, y_start, y_end;
    x_start = settings->region[0];
    y_start = settings->region[1];
    x_end = settings->region[2];
    y_end = settings->region[3];
    printf("New detection region: %d, %d, %d, %d\n", x_start, y_start, x_end, y_end);
    unsigned int stride = settings->mjpeg.width;

    motionDetection.region.offset = (y_start * stride) + x_start;
    motionDetection.region.num_rows = y_end - y_start;
    motionDetection.region.batches = 2;
    motionDetection.region.row_batch_size = motionDetection.region.num_rows / motionDetection.region.batches;
    motionDetection.region.row_length = x_end - x_start;
    motionDetection.region.stride = settings->mjpeg.vcosWidth;
}
void initDetection(SETTINGS* settings) {
    motionDetection.detection_sleep = settings->h264.fps / settings->motion_check_frequency;
    motionDetection.detection_at = motionDetection.detection_sleep;

    motionDetection.stream_sleep = settings->h264.fps / 3;

    motionDetection.previousFrame = (uint8_t*) malloc( settings->mjpeg.y_length );
    motionDetection.yuvBuffer = (uint8_t*) malloc( settings->mjpeg.y_length );

    motionDetection.motion_count = 0;

    strncpy(motionDetection.boundary, "--HATCHA\r\nContent-Type: image/jpeg\r\nContent-length: ", 80);
    motionDetection.boundaryLength = strlen(motionDetection.boundary);

    /*
    int start_x = 100;
    int start_y = 100;
    int width = 100;
    int height = 100;
    int end_y = start_y + height;
    int end_x = start_x + width;
    */

    reconfigureRegion(settings);
}

void freeDetection() {
    free(motionDetection.previousFrame);
    free(motionDetection.yuvBuffer);
}

void setDefaultSettings(SETTINGS* settings) {
    char* c;

    settings->streamDenominator = 3;

    //settings->width = 1920;
    //settings->height = 1080;
    //settings->h264.fps = 30;
    // lower settings keeps temperatures happy too
    // 1640x922 uses full sensor field-of-view, which is helpful
    settings->width = 1640;
    settings->height = 922;

    settings->region[0] = 0;
    settings->region[1] = 0;
    settings->region[2] = 100;
    settings->region[3] = 100;
    settings->changed_pixels_threshold = 500;
    settings->pixel_delta_threshold = 4000;
    pixel_delta_threshold = settings->pixel_delta_threshold;

    settings->vcosWidth = ALIGN_UP(settings->width, 32);
    settings->vcosHeight = ALIGN_UP(settings->height, 16);

    settings->h264.width = settings->width;
    settings->h264.height = settings->height;
    settings->h264.vcosWidth = settings->vcosWidth;
    settings->h264.vcosHeight = settings->vcosHeight;
    settings->h264.fps = 10;
    settings->h264.keyframePeriod = 1000;

    settings->mjpeg.width = settings->width / settings->streamDenominator;
    settings->mjpeg.height = settings->height / settings->streamDenominator;
    settings->mjpeg.vcosWidth = ALIGN_UP(settings->mjpeg.width, 32);
    settings->mjpeg.vcosHeight = ALIGN_UP(settings->mjpeg.height, 16);
    settings->mjpeg.y_length = settings->mjpeg.vcosWidth * settings->mjpeg.vcosHeight * 1.5;

    // TODO: rename this
    settings->motion_check_frequency = 3;
    settings->objectDetectionEndpoint[0] = 0;
    settings->verbose = 1;
    settings->debug = false;

    settings->sharpness = 0;
    settings->contrast = 0;
    settings->brightness = 50;
    settings->saturation = 0;
    settings->iso = 0;
    settings->videoStabilisation = 0;
    settings->exposureCompensation = 0;

    /*
    MMAL_PARAM_EXPOSUREMODE_OFF,
    MMAL_PARAM_EXPOSUREMODE_AUTO,
    MMAL_PARAM_EXPOSUREMODE_NIGHT,
    MMAL_PARAM_EXPOSUREMODE_NIGHTPREVIEW,
    MMAL_PARAM_EXPOSUREMODE_BACKLIGHT,
    MMAL_PARAM_EXPOSUREMODE_SPOTLIGHT,
    MMAL_PARAM_EXPOSUREMODE_SPORTS,
    MMAL_PARAM_EXPOSUREMODE_SNOW,
    MMAL_PARAM_EXPOSUREMODE_BEACH,
    MMAL_PARAM_EXPOSUREMODE_VERYLONG,
    MMAL_PARAM_EXPOSUREMODE_FIXEDFPS,
    MMAL_PARAM_EXPOSUREMODE_ANTISHAKE,
    MMAL_PARAM_EXPOSUREMODE_FIREWORKS,
   */
    settings->exposureMode = MMAL_PARAM_EXPOSUREMODE_AUTO;

    settings->flickerAvoidMode = MMAL_PARAM_FLICKERAVOID_OFF;
    /*
    MMAL_PARAM_FLICKERAVOID_OFF,
    MMAL_PARAM_FLICKERAVOID_AUTO,
    MMAL_PARAM_FLICKERAVOID_50HZ,
    MMAL_PARAM_FLICKERAVOID_60HZ,
    MMAL_PARAM_FLICKERAVOID_MAX = 0x7FFFFFFF
   */

    settings->awbMode = MMAL_PARAM_AWBMODE_SUNLIGHT;
    /*
    MMAL_PARAM_AWBMODE_OFF,
    MMAL_PARAM_AWBMODE_AUTO,
    MMAL_PARAM_AWBMODE_SUNLIGHT,
    MMAL_PARAM_AWBMODE_CLOUDY,
    MMAL_PARAM_AWBMODE_SHADE,
    MMAL_PARAM_AWBMODE_TUNGSTEN,
    MMAL_PARAM_AWBMODE_FLUORESCENT,
    MMAL_PARAM_AWBMODE_INCANDESCENT,
    MMAL_PARAM_AWBMODE_FLASH,
    MMAL_PARAM_AWBMODE_HORIZON,
    MMAL_PARAM_AWBMODE_GREYWORLD,
    */

    // get hostname
    if (gethostname(settings->hostname, sizeof(settings->hostname)) != 0) {
        logError("Failed to gethostname", __func__);
        snprintf(settings->hostname, sizeof(settings->hostname), "INVALID");
    }

    // video path
    snprintf(settings->videoPath, 100, "");

    // TODO: handle this in readSettings()
    /*
    settings->mjpeg.width = 640;
    settings->mjpeg.height = 480;
    */
    
    
    c = getenv("DEBUG");
    if (c) {
        logInfo("Enabling debug");
        settings->debug = true;
    }

}

void readSettings(SETTINGS* settings) {
    FILE* fp = fopen("settings.ini", "r");
    if (fp == NULL) {
        // no settings
        return;
    }
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* data = (char*) malloc(size + 1);
    if (!data) {
        logError("Failed to allocate data to read settings.ini. Bailing\n", __func__);
        return;
    }
    fread(data, 1, size, fp);
    data[ size ] = '\0';
    fclose(fp);

    ini_t* ini = ini_load(data, NULL);
    free(data);

    int i;
    char const *value;
    char valueCopy[128];
    
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "width", 5);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->width = atoi(value);
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "height", 6);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->height = atoi(value);
    }
    settings->vcosWidth = ALIGN_UP(settings->width, 32);
    settings->vcosHeight = ALIGN_UP(settings->height, 16);
    settings->h264.width = settings->width;
    settings->h264.height = settings->height;
    settings->h264.vcosWidth = settings->vcosWidth;
    settings->h264.vcosHeight = settings->vcosHeight;

    settings->mjpeg.width = settings->width / settings->streamDenominator;
    settings->mjpeg.height = settings->height / settings->streamDenominator;
    settings->mjpeg.vcosWidth = ALIGN_UP(settings->mjpeg.width, 32);
    settings->mjpeg.vcosHeight = ALIGN_UP(settings->mjpeg.height, 16);
    settings->mjpeg.y_length = settings->mjpeg.vcosWidth * settings->mjpeg.vcosHeight * 1.5;

    printf("MJPEG VCOS %d x %d\n", settings->mjpeg.vcosWidth, settings->mjpeg.vcosHeight);

    i = ini_find_property(ini, INI_GLOBAL_SECTION, "fps", 3);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->h264.fps = atoi(value);
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "videoPath", 9);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->videoPath, value, sizeof(settings->videoPath));
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "changedPixelsThreshold", 15);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->changed_pixels_threshold = atoi(value);
    }
    // TODO: rename this with units
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "motion_check_frequency", 20);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->motion_check_frequency = atoi(value);
    }

    // motion detection region
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "region", 6);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);

        strcpy(valueCopy, value);

        char *p = strtok(valueCopy, ",");
        int j = 0;
        while (p) {
            settings->region[ j ] = atoi(p);
            j++;
            p = strtok(NULL, ",");
        }
        logInfo("Region: %d %d %d %d", settings->region[0], settings->region[1], settings->region[2], settings->region[3]);
    }
    // motion detection threshold
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "changedPixelsThreshold", 22);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->changed_pixels_threshold = atoi(value);
    }

    fprintf(stdout, "[INFO] SETTINGS. h264 %dx%d @ %d fps. mjpeg %dx%d. motion_check_frequency: %d\n", settings->h264.width, settings->h264.height, settings->h264.fps, settings->mjpeg.width, settings->mjpeg.height, settings->motion_check_frequency);


    ini_destroy(ini);
    return;
}


// HELPER FUNCTIONS




void send_buffers_to_port(MMAL_PORT_T* port, MMAL_QUEUE_T* queue) {
    // TODO: move this to main thread
    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T* new_buffer;
        while ( (new_buffer = mmal_queue_get(queue)) ) {
            status = mmal_port_send_buffer(port, new_buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed, no buffer to return to port\n", __func__);
                break;
            }
        }
    }
}

MMAL_STATUS_T connectEnable(MMAL_CONNECTION_T **conn, MMAL_PORT_T *output, MMAL_PORT_T *input) {
    MMAL_STATUS_T status;

    status =  mmal_connection_create(conn, output, input, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);
    if (status != MMAL_SUCCESS) {
        return status;
    }

    status = mmal_connection_enable(*conn);
    if (status != MMAL_SUCCESS) {
        mmal_connection_destroy(*conn);
    }

    return status;
}

// TODO: better param order
void disable_port(MMAL_PORT_T *port, char const* description) {
    if (DEBUG) {
        fprintf(stdout, "[INFO] Disabling %s port\n", description);
    }
    if (!port) {
        if (DEBUG) {
            fprintf(stdout, "[INFO] Nothing to disable. %s port is NULL\n", description);
        }
        return;
    }
    if (!port->is_enabled) {
        if (DEBUG) {
            fprintf(stdout, "[INFO] Nothing to disable. %s port not enabled\n", description);
        }
        return;
    }

    mmal_port_disable(port);
}
void destroyConnection(SETTINGS *settings, MMAL_CONNECTION_T *connection, char const* description) {
    if (!connection) {
        if (settings->verbose) {
            fprintf(stdout, "[INFO] Nothing to destroy. %s connection is NULL\n", description);
        }
        return;
    }
    if (settings->verbose) {
        fprintf(stdout, "[INFO] Destroying %s connection\n", description);
    }
    mmal_connection_destroy(connection);
}

void disableComponent(SETTINGS *settings, MMAL_COMPONENT_T *component, char const* description) {
    if (!component) {
        if (settings->verbose) {
            fprintf(stdout, "[INFO] Nothing to disable. %s component is NULL\n", description);
        }
        return;
    }
    if (settings->verbose) {
        fprintf(stdout, "[INFO] Disabling %s component\n", description);
    }
    mmal_component_disable(component);
}
void destroyComponent(SETTINGS *settings, MMAL_COMPONENT_T *component, char const* description) {
    if (!component) {
        if (settings->verbose) {
            fprintf(stdout, "[INFO] Nothing to destroy. %s component is NULL\n", description);
        }
        return;
    }
    if (settings->verbose) {
        fprintf(stdout, "[INFO] Destroying %s component\n", description);
    }
    mmal_component_destroy(component);
}

void signalHandler(int signal_number) {
    logError("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = false;
    pthread_mutex_unlock(&running_mutex);
    logInfo("Signaled");
}

// END HELPERS



// CAMERA FUNCTIONS

void cameraControlCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    fprintf(stdout, " HEY cameraControlCallback, buffer->cmd: %d\n", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

static void destroy_camera(MMAL_COMPONENT_T *camera) {
    if (camera) {
        if (DEBUG) {
            logInfo("Destroying camera");
        }
        mmal_component_destroy(camera);
    }
}

static MMAL_STATUS_T create_camera(MMAL_COMPONENT_T **camera_handle, SETTINGS *settings) {
    MMAL_COMPONENT_T *camera = 0;
    MMAL_PARAMETER_CAMERA_CONFIG_T camera_config;
    MMAL_ES_FORMAT_T *format;
    MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
    MMAL_STATUS_T status;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed", __func__);
        return status;
    }

    status = mmal_port_enable(camera->control, cameraControlCallback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed", __func__);
        destroyComponent(settings, camera, "camera");
        return status;
    }

    // All this is necessary to enable timestamps on frames
    camera_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
    camera_config.hdr.size = sizeof(MMAL_PARAMETER_CAMERA_CONFIG_T);
    camera_config.max_stills_w = settings->width;
    camera_config.max_stills_h = settings->height;
    camera_config.stills_yuv422 = 0;
    camera_config.one_shot_stills = 0;
    camera_config.max_preview_video_w = settings->width;
    camera_config.max_preview_video_h = settings->height;
    camera_config.num_preview_video_frames = 3;
    camera_config.stills_capture_circular_buffer_height = 0;
    camera_config.fast_preview_resume = 0;

    // this param affects buffer->pts
    camera_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC;
    mmal_port_parameter_set(camera->control, &camera_config.hdr);

    // Sensor mode 0 for auto
    status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, 0);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set_uint32 failed to set sensor config to 0", __func__);
        //destroyComponent(settings, camera, "camera");
        destroy_camera(camera);
        return status;
    }


    // TODO: report if setting these params fails
    MMAL_RATIONAL_T value;
    value.num = settings->saturation;
    value.den = 100;
    mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_SATURATION, value);

    value.num = settings->sharpness;
    mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_SHARPNESS, value);
    
    value.num = settings->contrast;
    mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_CONTRAST, value);

    value.num = settings->brightness;
    mmal_port_parameter_set_rational(camera->control, MMAL_PARAMETER_BRIGHTNESS, value);

    mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_ISO, settings->iso);

    mmal_port_parameter_set_boolean(camera->control, MMAL_PARAMETER_VIDEO_STABILISATION, settings->videoStabilisation);

    mmal_port_parameter_set_int32(camera->control, MMAL_PARAMETER_EXPOSURE_COMP, settings->exposureCompensation);

    MMAL_PARAMETER_EXPOSUREMODE_T exposureMode;
    exposureMode.hdr.id = MMAL_PARAMETER_EXPOSURE_MODE;
    exposureMode.hdr.size = sizeof(MMAL_PARAMETER_EXPOSUREMODE_T);
    exposureMode.value = settings->exposureMode;
    mmal_port_parameter_set(camera->control, &exposureMode.hdr);

    MMAL_PARAMETER_FLICKERAVOID_T flickerAvoidMode;
    flickerAvoidMode.hdr.id = MMAL_PARAMETER_FLICKER_AVOID;
    flickerAvoidMode.hdr.size = sizeof(MMAL_PARAMETER_FLICKERAVOID_T);
    flickerAvoidMode.value = settings->flickerAvoidMode;
    mmal_port_parameter_set(camera->control, &flickerAvoidMode.hdr);

    MMAL_PARAMETER_AWBMODE_T awbMode;
    awbMode.hdr.id = MMAL_PARAMETER_AWB_MODE;
    awbMode.hdr.size = sizeof(MMAL_PARAMETER_AWBMODE_T);
    awbMode.value = settings->awbMode;
    mmal_port_parameter_set(camera->control, &awbMode.hdr);


    // port enable was here

    video_port = camera->output[CAMERA_VIDEO_PORT];

    // this doesn't work
    /*
    status = mmal_port_parameter_set_boolean(video_port, MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, 1);
    if (status) {
        logError("Failed to enable timestamp interpolation", __func__);
    }
    */

    format = video_port->format;
    //format->encoding_variant = MMAL_ENCODING_I420;

    fprintf(stdout, "[INFO] vcos w: %d h: %d\n", settings->vcosWidth, settings->vcosHeight);

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->es->video.width = settings->width; // TODO: do we need vcosWidth here?
    format->es->video.height = settings->height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = settings->width;
    format->es->video.crop.height = settings->height;
    format->es->video.frame_rate.num = settings->h264.fps;
    format->es->video.frame_rate.den = 1; // i think this is what we want

    status = mmal_port_format_commit(video_port);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed\n", __func__);
        destroyComponent(settings, camera, "camera");
        return status;
    }

    status = mmal_component_enable(camera);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed", __func__);
        destroyComponent(settings, camera, "camera");
        return status;
    }

    /*
    MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T change_event_request;
    change_event_request.change_id = MMAL_PARAMETER_CAMERA_SETTINGS;
    change_event_request.enable = 1;

    status = mmal_port_parameter_set(camera->control, &change_event_request.hdr);
    if (status != MMAL_SUCCESS) {
        logError("Failed to set camera settings", __func__);
    }
    */

    *camera_handle = camera;


    if (DEBUG) {
        logInfo("Camera created");
    }

    return status;
}

// END CAMERA FUNCTIONS



// SPLITTER FUNCTIONS
static MMAL_STATUS_T createSplitter(HANDLES *handles, SETTINGS* settings) {
    MMAL_COMPONENT_T *splitter = 0;
    MMAL_PORT_T *splitter_output = NULL;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;
    int i;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &splitter);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for splitter", __func__);
        return status;
    }

    mmal_format_copy(splitter->input[0]->format, handles->camera->output[CAMERA_VIDEO_PORT]->format);

    // TODO: not sure this is right
    /*
    if (splitter->input[0]->buffer_num < DESIRED_OUTPUT_BUFFERS) {
        splitter->input[0]->buffer_num = DESIRED_OUTPUT_BUFFERS;
    }
    */


    status = mmal_port_format_commit(splitter->input[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on splitter input", __func__);
        destroyComponent(settings, splitter, "splitter");
        return status;
    }

    // KINDA DOUBT I NEED TO BE CONCERNED WITH SPLITTER BUFFERS
    //splitter->input[0]->buffer_num < splitter->input[0]->buffer_num_recommended;

    // Pass through the same input format to outputs
    for (i = 0; i < splitter->output_num; i++) {
        mmal_format_copy(splitter->output[i]->format, splitter->input[0]->format);

        if (i == SPLITTER_YUV_PORT) {
            // need both of these to move from opaque format
            splitter->output[i]->format->encoding = MMAL_ENCODING_I420;
            splitter->output[i]->format->encoding_variant = MMAL_ENCODING_I420;

            splitter->output[i]->buffer_size = settings->vcosWidth * settings->vcosHeight * 1.5;
            //splitter->output[i]->buffer_size = splitter->output[i]->buffer_size_recommended;
            splitter->output[i]->buffer_num = DESIRED_OUTPUT_BUFFERS;
        }

        status = mmal_port_format_commit(splitter->output[i]);
        if (status != MMAL_SUCCESS) {
            logError("mmal_port_format_commit failed on a splitter output", __func__);
            destroyComponent(settings, splitter, "splitter");
            return status;
        }
        /*
        if (i == SPLITTER_YUV_PORT) {
            fprintf(
                stdout,
                "bgr buf. num rec: %d size rec: %d\n",
                splitter->output[i]->buffer_num_recommended,
                splitter->output[i]->buffer_size_recommended
            );
            // docs say to adjust these after changes to port's format
            // crossing fingers
            // hope this incorporates vcos dims given stride
            splitter->output[i]->buffer_size = settings->vcosWidth * settings->vcosHeight * 1.5;
            //splitter->output[i]->buffer_size = splitter->output[i]->buffer_size_recommended;
            splitter->output[i]->buffer_num = DESIRED_OUTPUT_BUFFERS;
            //splitter->output[i]->buffer_num = splitter->output[i]->buffer_num_recommended;
        }
        */
    }

    status = mmal_component_enable(splitter);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on splitter", __func__);
        destroyComponent(settings, splitter, "splitter");
        return status;
    }

    // create yuv pool
    //settings.vcosWidth * settings.vcosHeight * 3;
    int yuv_buffer_size = splitter->output[SPLITTER_YUV_PORT]->buffer_size;
    int yuv_num_buffers = splitter->output[SPLITTER_YUV_PORT]->buffer_num;
    fprintf(stdout, "Creating yuv buffer pool with %d bufs of size: %d\n", yuv_num_buffers, yuv_buffer_size);
    handles->yuvPool = mmal_port_pool_create(splitter->output[SPLITTER_YUV_PORT], yuv_num_buffers, yuv_buffer_size);
    if (!handles->yuvPool) {
        logError("mmal_port_pool_create failed for yuv output", __func__);
        // TODO: what error code for this?
    } else {
        if (settings->verbose) {
            logInfo("Created yuv pool");
        }
    }

    handles->splitter = splitter;
    if (settings->verbose) {
        logInfo("Splitter created");
    }

    return status;
}

static void destroySplitter(HANDLES *handles, SETTINGS *settings) {
    if (handles->splitter) {
        if (settings->verbose) {
            logInfo("Destroying splitter");
        }
        mmal_component_destroy(handles->splitter);
        handles->splitter = NULL;
    }
}
// END OLD SPLITTER FUNCTIONS

// BEGIN NEW SPLITTER FUNCTIONS
static MMAL_STATUS_T create_splitter(MMAL_COMPONENT_T **splitter_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, int num_outputs) {
    MMAL_COMPONENT_T *splitter = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;
    int i;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &splitter);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for splitter", __func__);
        return status;
    }

    // Tell splitter which format it'll be receiving from the camera video output
    mmal_format_copy(splitter->input[0]->format, output_port->format);

    status = mmal_port_format_commit(splitter->input[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on splitter input", __func__);
        mmal_component_destroy(splitter);
        return status;
    }

    splitter->output_num = num_outputs;
    // Pass through the same input format to outputs
    for (i = 0; i < splitter->output_num; i++) {
        mmal_format_copy(splitter->output[i]->format, splitter->input[0]->format);

        status = mmal_port_format_commit(splitter->output[i]);
        if (status != MMAL_SUCCESS) {
            logError("mmal_port_format_commit failed on a splitter output", __func__);
            mmal_component_destroy(splitter);
            return status;
        }
    }

    status = mmal_component_enable(splitter);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on splitter", __func__);
        mmal_component_destroy(splitter);
        return status;
    }

    *splitter_handle = splitter;
    if (DEBUG) {
        logInfo("Splitter created");
    }

    if (DEBUG) {
        logInfo("Connecting specified port to splitter input port");
    }

    status = mmal_connection_create(
        connection_handle,
        output_port,
        splitter->input[0],
        MMAL_CONNECTION_FLAG_TUNNELLING | 
        MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    );
    if (status != MMAL_SUCCESS) {
        *connection_handle = NULL;
        logError("connectEnable failed for specified port to splitter input", __func__);
        mmal_component_destroy(splitter);
        return EX_ERROR;
    }

    status = mmal_connection_enable(*connection_handle);
    if (status != MMAL_SUCCESS) {
        mmal_connection_destroy(*connection_handle);
        *connection_handle = NULL;
    }

    return status;
}

static void destroy_splitter(MMAL_COMPONENT_T *splitter, MMAL_CONNECTION_T *connection) {
    if (connection) {
        if (DEBUG) {
            logInfo("Destroying splitter connection");
        }
    }
    mmal_connection_destroy(connection);


    if (splitter) {
        if (DEBUG) {
            logInfo("Destroying splitter");
        }
        mmal_component_destroy(splitter);
    }
}
// END NEW SPLITTER FUNCTIONS




static MMAL_STATUS_T create_resizer(MMAL_COMPONENT_T **handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MJPEG_SETTINGS *settings) {
    MMAL_COMPONENT_T *resizer = 0;
    MMAL_PORT_T *splitter_output = NULL;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;
    int i;

    status = mmal_component_create("vc.ril.isp", &resizer);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for resizer", __func__);
        return status;
    }

    // Tell resizer which format it'll be receiving
    mmal_format_copy(resizer->input[0]->format, output_port->format);

    status = mmal_port_format_commit(resizer->input[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on resizer input", __func__);
        mmal_component_destroy(resizer);
        return status;
    }

    // configure output format
    // need botfh of these to move from opaque format
    mmal_format_copy(resizer->output[0]->format, resizer->input[0]->format);
    format = resizer->output[0]->format;
    format->encoding = MMAL_ENCODING_I420;
    format->encoding_variant = MMAL_ENCODING_I420;

    format->es->video.width = settings->vcosWidth;
    format->es->video.height = settings->vcosHeight;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = settings->width;
    format->es->video.crop.height = settings->height;

    status = mmal_port_format_commit(resizer->output[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on resizer output port", __func__);
        mmal_component_destroy(resizer);
        return status;
    }

    status = mmal_component_enable(resizer);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on resizer", __func__);
        mmal_component_destroy(resizer);
        return status;
    }


    if (DEBUG) {
        logInfo("Connecting splitter YUV port to resizer input port");
    }

    status = mmal_connection_create(
        connection_handle,
        output_port,
        resizer->input[0],
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    );
    if (status != MMAL_SUCCESS) {
        *connection_handle = NULL;
        logError("connectEnable failed for splitter YUV to resizer", __func__);
        mmal_component_destroy(resizer);
        return EX_ERROR;
    }

    status = mmal_connection_enable(*connection_handle);
    if (status != MMAL_SUCCESS) {
        mmal_connection_destroy(*connection_handle);
        *connection_handle = NULL;
    }

    *handle = resizer;
    if (DEBUG) {
        logInfo("Resizer created");
    }
    return status;
}

static void destroy_resizer(MMAL_COMPONENT_T *splitter, MMAL_CONNECTION_T *connection) {
    if (connection) {
        if (DEBUG) {
            logInfo("Destroying resizer connection");
        }
    }
    mmal_connection_destroy(connection);


    if (splitter) {
        if (DEBUG) {
            logInfo("Destroying resizer");
        }
        mmal_component_destroy(splitter);
    }
}


// MJPEG ENCODER FUNCTIONS
static void destroy_mjpeg_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool) {
    if (connection) {
        mmal_connection_destroy(connection);
    }

    // Get rid of any port buffers first
    if (pool) {
        if (DEBUG) {
            logInfo("Destroying mjpeg encoder pool");
        }
        mmal_port_pool_destroy(component->output[0], pool);
    }

    if (component) {
        if (DEBUG) {
            logInfo("Destroying mjpeg encoder component");
        }
        mmal_component_destroy(component);
    }
}

static MMAL_STATUS_T create_mjpeg_encoder(MMAL_COMPONENT_T **handle, MMAL_POOL_T **pool_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MMAL_PORT_BH_CB_T callback, MJPEG_SETTINGS *settings) {
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for mjpeg encoder", __func__);
        return status;
    }

    mmal_format_copy(encoder->input[0]->format, output_port->format);

    encoder_output = encoder->output[0];
    encoder_output->format->encoding = MMAL_ENCODING_MJPEG;
    // TODO: what should this be? should it vary based on resolution?
    encoder_output->format->bitrate = MJPEG_BITRATE;

    // WTF docs say these shoudl come after format commit, but that seems
    // wrong, and causes weird behavior

    // TODO: figure this out and comment why
    // use larger buffer size to hopefully address lockups as mentioned in this:
    // https://github.com/waveform80/picamera/pull/179/commits/405f5ed0b107209cdf3dd27b92fceec8962a77d6
    encoder_output->buffer_num = 3;
    encoder_output->buffer_size = settings->width * settings->height * 1.5;

    status = mmal_port_format_commit(encoder_output);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on mjpeg encoder output port", __func__);
        mmal_component_destroy(encoder);
        return status;
    }




    // setting jpeg quality doesn't seem to impact MJPEG
    //mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_Q_FACTOR, 50);    

    status = mmal_component_enable(encoder);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on mjpeg encoder", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    status = mmal_connection_create(
        connection_handle,
        output_port,
        encoder->input[0],
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    );
    if (status != MMAL_SUCCESS) {
        logInfo("Failed to create connection to mjpeg encoder", __func__);
        return status;
    }

    status = mmal_connection_enable(*connection_handle);
    if (status != MMAL_SUCCESS) {
        logError("Failed to enable connection to mjpeg encoder", __func__);
        return EX_ERROR;
    }

    if (DEBUG) {
        logInfo("Enabling mjpeg encoder output port");
    }

    status = mmal_port_enable(encoder_output, callback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for mjpeg encoder output", __func__);
        destroy_mjpeg_encoder(encoder, *connection_handle, pool);
        return EX_ERROR;
    }


    fprintf(stdout, "Creating mjpeg buffer pool with %d bufs of size: %d\n", encoder_output->buffer_num, encoder_output->buffer_size);

    // TODO: we probably don't need a pool. can likely get by with 1 buffer
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
    if (!pool) {
        logError("mmal_port_pool_create failed for mjpeg encoder output", __func__);
        // TODO: what error code for this?
        return MMAL_ENOMEM;
    }

    *handle = encoder;
    *pool_handle = pool;

    if (DEBUG) {
        logInfo("Created MJPEG encoder");
    }

    return status;
}

int stream_threshold = 0;
int mjpeg_concurrent = 0;
pthread_mutex_t mjpeg_concurrent_mutex;
// callbacks DO run in a separate thread from main
// TODO: convert from camel case
void mjpegCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata = (CALLBACK_USERDATA *)port->userdata;
    int64_t current_time;
    // HTTP response variables
    //char response[8192];
    //int responseLength;

    int ret;
    char contentLength[21];
    int contentLengthLength;

    //logInfo("got mjpeg");

    // TODO: get rid of this
    pthread_mutex_lock(&mjpeg_concurrent_mutex);
    if (mjpeg_concurrent > 0) {
	    printf("mjpegCallback already in process\n");
    }
    mjpeg_concurrent++;
    pthread_mutex_unlock(&mjpeg_concurrent_mutex);

    
    if (!userdata) {
        logError("Expected userdata in mjpeg callback", __func__);
        mmal_buffer_header_release(buffer);
        send_buffers_to_port(port, userdata->handles->mjpeg_encoder_pool->queue);

        // TODO: get rid
        pthread_mutex_lock(&mjpeg_concurrent_mutex);
        mjpeg_concurrent--;
        pthread_mutex_unlock(&mjpeg_concurrent_mutex);
        return;
    }

    if (buffer->length == 0) {
        logError("No data in mjpeg callback buffer", __func__);
        mmal_buffer_header_release(buffer);
        send_buffers_to_port(port, userdata->handles->mjpeg_encoder_pool->queue);

        pthread_mutex_lock(&mjpeg_concurrent_mutex);
        mjpeg_concurrent--;
        pthread_mutex_unlock(&mjpeg_concurrent_mutex);
        return;
    }

    if (frame_counter >= stream_threshold) {
        stream_threshold = frame_counter + motionDetection.stream_sleep;

        // TODO: put connection lists in a circular buffer of linked lists
        // so clients can control their framerate by passing param with HTTP req
        pthread_mutex_lock(&stream_connections_mutex);
        connection* connection = stream_connections;
        if (connection) {

            contentLengthLength = snprintf(contentLength, 20, "%d\r\n\r\n", buffer->length);

            mmal_buffer_header_mem_lock(buffer);
            while (connection) {
                // TODO: wrap each of these in a function, with loops that ensure all 
                // data has been written

                // bail when fail to write
                sendSocket(connection->fd, motionDetection.boundary, motionDetection.boundaryLength) && 
                sendSocket(connection->fd, contentLength, contentLengthLength) && 
                sendSocket(connection->fd, (char*)buffer->data, buffer->length);
                connection = connection->next;
            }
            mmal_buffer_header_mem_unlock(buffer);
        }
        pthread_mutex_unlock(&stream_connections_mutex);
    }

    mmal_buffer_header_release(buffer);

    send_buffers_to_port(port, userdata->handles->mjpeg_encoder_pool->queue);

    // TODO: get rid
    pthread_mutex_lock(&mjpeg_concurrent_mutex);
    mjpeg_concurrent--;
    pthread_mutex_unlock(&mjpeg_concurrent_mutex);
}


// END MJPEG FUNCTIONS



// H264 FUNCTIONS
// TODO: combine this with mjpeg encoeder destroy
static MMAL_STATUS_T createH264Encoder(HANDLES *handles, SETTINGS *settings) {
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *splitter_output;
    MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for h264 encoder", __func__);
        return status;
    }

    // TODO: do i really need this?
    if (!encoder->input_num || !encoder->output_num) {
        logError("Expected h264 encoder to have input/output ports", __func__);
        return MMAL_ENOSYS;
    }

    splitter_output = handles->splitter->output[SPLITTER_H264_PORT];
    encoder_input = encoder->input[0];
    encoder_output = encoder->output[0];

    // Encoder input should match splitter output format
    mmal_format_copy(encoder_input->format, splitter_output->format);

    encoder_output->format->encoding = MMAL_ENCODING_H264;
    // 25Mit or 62.5Mbit
    encoder_output->format->bitrate = H264_BITRATE;

    // this isn't quite working
    encoder_output->format->es->video.width = settings->vcosWidth;
    encoder_output->format->es->video.height = settings->vcosHeight;
    encoder_output->format->es->video.crop.x = 0;
    encoder_output->format->es->video.crop.y = 0;
    encoder_output->format->es->video.crop.width = settings->width;
    encoder_output->format->es->video.crop.height = settings->height;

    // Frame rate will get updated according to input frame rate once connected
    encoder_output->format->es->video.frame_rate.num = 0;
    encoder_output->format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(encoder_output);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on h264 encoder output port", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    /*
    encoder_output->buffer_size = encoder_output->buffer_size_recommended;
    if (encoder_output->buffer_size < encoder_output->buffer_size_min) {
        logInfo("Adjusting buffer_size");
        encoder_output->buffer_size = encoder_output->buffer_size_min;
    }
    */

    encoder_output->buffer_size = settings->width * settings->height * 1.5;

    // TODO: raise this once we get queue set up
    /*
    encoder_output->buffer_num = encoder_output->buffer_num_recommended;
    if (encoder_output->buffer_num < encoder_output->buffer_num_min) {
        logInfo("Adjusting buffer_num");
        encoder_output->buffer_num = encoder_output->buffer_num_min;
    }
    */

    // 3 seconds of buffers
    encoder_output->buffer_num = DESIRED_OUTPUT_BUFFERS; //settings->h264.fps + 10;


    fprintf(stdout, "Creating h264 buffer pool with %d bufs of size: %d\n", encoder_output->buffer_num, encoder_output->buffer_size);

    MMAL_PARAMETER_UINT32_T intraperiod;
    intraperiod.hdr.id = MMAL_PARAMETER_INTRAPERIOD;
    intraperiod.hdr.size = sizeof(intraperiod);
    intraperiod.value = settings->h264.fps;
    status = mmal_port_parameter_set(encoder_output, &intraperiod.hdr);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set failed on h264 encoder", __func__);
        // who cares?
    }

    status = mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, true);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set_boolean failed to set SPS timing on h264 encoder", __func__);
    }


    MMAL_PARAMETER_VIDEO_PROFILE_T video_profile;
    video_profile.hdr.id = MMAL_PARAMETER_PROFILE;
    video_profile.hdr.size = sizeof(video_profile);

    video_profile.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;
    video_profile.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;

    // From RaspiVid.c
    /*
    if((VCOS_ALIGN_UP(settings->width,16) >> 4) * (VCOS_ALIGN_UP(settings->height,16) >> 4) * settings->h264.fps > 245760) {
        logInfo("Here");
        if((VCOS_ALIGN_UP(settings->width,16) >> 4) * (VCOS_ALIGN_UP(settings->height,16) >> 4) * settings->h264.fps <= 522240) {
            logInfo("Too many macroblocks/s: Increasing H264 Level to 4.2\n");
            video_profile.profile[0].level = MMAL_VIDEO_LEVEL_H264_42;
        } else {
            logError("Too many macroblocks/s requested, bailing", __func__);
            mmal_component_destroy(encoder);
            return MMAL_EINVAL;
        }
    }
    */
    
    status = mmal_port_parameter_set(encoder_output, &video_profile.hdr);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set failed on h264 encoder output port profile", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    status = mmal_component_enable(encoder);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on h264 encoder", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
    if (!pool) {
        logError("mmal_port_pool_create failed for h264 encoder output", __func__);
        // TODO: what error code for this?
        return MMAL_ENOMEM;
    }


    handles->h264_encoder_pool = pool;
    handles->h264_encoder = encoder;

    if (settings->verbose) {
        logInfo("Created H264 encoder");
    }

    return status;
}

static void destroy_h264_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool) {
    if (connection) {
        mmal_connection_destroy(connection);
    }

    // Get rid of any port buffers first
    if (pool) {
        if (DEBUG) {
            logInfo("Destroying h264 encoder pool");
        }
        mmal_port_pool_destroy(component->output[0], pool);
    }

    if (component) {
        if (DEBUG) {
            logInfo("Destroying h264 encoder component");
        }
        mmal_component_destroy(component);
    }
}

static MMAL_STATUS_T create_h264_encoder(MMAL_COMPONENT_T **handle, MMAL_POOL_T **pool_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MMAL_PORT_BH_CB_T callback, H264_SETTINGS *settings) {
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for h264 encoder", __func__);
        return status;
    }

    // TODO: do i really need this?
    if (!encoder->input_num || !encoder->output_num) {
        logError("Expected h264 encoder to have input/output ports", __func__);
        return MMAL_ENOSYS;
    }

    encoder_output = encoder->output[0];

    // Encoder input should match splitter output format
    mmal_format_copy(encoder->input[0]->format, output_port->format);

    encoder_output->format->encoding = MMAL_ENCODING_H264;
    // 25Mit or 62.5Mbit
    encoder_output->format->bitrate = H264_BITRATE;

    // this isn't quite working
    encoder_output->format->es->video.width = settings->vcosWidth;
    encoder_output->format->es->video.height = settings->vcosHeight;
    encoder_output->format->es->video.crop.x = 0;
    encoder_output->format->es->video.crop.y = 0;
    encoder_output->format->es->video.crop.width = settings->width;
    encoder_output->format->es->video.crop.height = settings->height;

    // Frame rate will get updated according to input frame rate once connected
    encoder_output->format->es->video.frame_rate.num = 0;
    encoder_output->format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(encoder_output);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on h264 encoder output port", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    /*
    encoder_output->buffer_size = encoder_output->buffer_size_recommended;
    if (encoder_output->buffer_size < encoder_output->buffer_size_min) {
        logInfo("Adjusting buffer_size");
        encoder_output->buffer_size = encoder_output->buffer_size_min;
    }


    // TODO: raise this once we get queue set up
    encoder_output->buffer_num = encoder_output->buffer_num_recommended;
    if (encoder_output->buffer_num < encoder_output->buffer_num_min) {
        logInfo("Adjusting buffer_num");
        encoder_output->buffer_num = encoder_output->buffer_num_min;
    }
    */


    encoder_output->buffer_size = settings->width * settings->height * 1.5;
    // 3 seconds of buffers
    encoder_output->buffer_num = DESIRED_OUTPUT_BUFFERS; //settings->h264.fps + 10;

    //encoder_output->buffer_size = encoder_output->buffer_size_recommended;
    //encoder_output->buffer_num = encoder_output->buffer_num_recommended;

    fprintf(stdout, "Creating h264 buffer pool with %d bufs of size for %dx%d: %d\n", encoder_output->buffer_num, settings->width, settings->height, encoder_output->buffer_size);

    MMAL_PARAMETER_UINT32_T intraperiod;
    intraperiod.hdr.id = MMAL_PARAMETER_INTRAPERIOD;
    intraperiod.hdr.size = sizeof(intraperiod);
    intraperiod.value = settings->fps;
    status = mmal_port_parameter_set(encoder_output, &intraperiod.hdr);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set failed on h264 encoder", __func__);
        // who cares?
    }

    status = mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, true);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set_boolean failed to set SPS timing on h264 encoder", __func__);
    }


    MMAL_PARAMETER_VIDEO_PROFILE_T video_profile;
    video_profile.hdr.id = MMAL_PARAMETER_PROFILE;
    video_profile.hdr.size = sizeof(video_profile);

    video_profile.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;
    video_profile.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;

    /*
    if((VCOS_ALIGN_UP(settings->width,16) >> 4) * (VCOS_ALIGN_UP(settings->height,16) >> 4) * settings->h264.fps > 245760) {
        logInfo("Here");
        if((VCOS_ALIGN_UP(settings->width,16) >> 4) * (VCOS_ALIGN_UP(settings->height,16) >> 4) * settings->h264.fps <= 522240) {
            logInfo("Too many macroblocks/s: Increasing H264 Level to 4.2\n");
            video_profile.profile[0].level = MMAL_VIDEO_LEVEL_H264_42;
        } else {
            logError("Too many macroblocks/s requested, bailing", __func__);
            mmal_component_destroy(encoder);
            return MMAL_EINVAL;
        }
    }
    */
    
    status = mmal_port_parameter_set(encoder_output, &video_profile.hdr);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_parameter_set failed on h264 encoder output port profile", __func__);
        mmal_component_destroy(encoder);
        return status;
    }

    status = mmal_component_enable(encoder);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on h264 encoder", __func__);
        mmal_component_destroy(encoder);
        return status;
    }


    if (DEBUG) {
        logInfo("Connecting splitter h264 port to h264 encoder input port");
    }

    status = mmal_connection_create(
        connection_handle,
        output_port,
        encoder->input[0],
        MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    );
    if (status != MMAL_SUCCESS) {
        logInfo("Failed to create connection to h264 encoder", __func__);
        return status;
    }

    status = mmal_connection_enable(*connection_handle);
    if (status != MMAL_SUCCESS) {
        logError("Failed to enable connection to h264 encoder", __func__);
        return EX_ERROR;
    }

    if (DEBUG) {
        logInfo("Enabling h264 encoder output port");
    }

    status = mmal_port_enable(encoder_output, callback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for h264 encoder output", __func__);
        destroy_h264_encoder(encoder, *connection_handle, pool);
        return EX_ERROR;
    }

    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
    if (!pool) {
        logError("mmal_port_pool_create failed for h264 encoder output", __func__);
        // TODO: what error code for this?
        return MMAL_ENOMEM;
    }


    *handle = encoder;
    *pool_handle = pool;

    if (DEBUG) {
        logInfo("Created H264 encoder");
    }

    return status;
}




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
int h264_concurrent = 0;
pthread_mutex_t h264_concurrent_mutex;
void h264Callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata;
    SETTINGS *settings;
    struct timespec ts;
    bool debug;

    pthread_mutex_lock(&h264_concurrent_mutex);
    if (h264_concurrent > 0) {
	    printf("h264Callback already in process\n");
    }
    h264_concurrent++;
    pthread_mutex_unlock(&h264_concurrent_mutex);

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

        pthread_mutex_lock(&h264_concurrent_mutex);
        h264_concurrent--;
        pthread_mutex_unlock(&h264_concurrent_mutex);
        return;
    }
    // is this necessary during shutdown to prevent a bunch of callbacks with messed up data?
    if (!buffer->length) {
        mmal_buffer_header_release(buffer);
        sendH264Buffers(port, userdata);

        pthread_mutex_lock(&h264_concurrent_mutex);
        h264_concurrent--;
        pthread_mutex_unlock(&h264_concurrent_mutex);
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

        pthread_mutex_lock(&h264_concurrent_mutex);
        h264_concurrent--;
        pthread_mutex_unlock(&h264_concurrent_mutex);
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

    pthread_mutex_lock(&h264_concurrent_mutex);
    h264_concurrent--;
    pthread_mutex_unlock(&h264_concurrent_mutex);
}


static void destroyH264Encoder(HANDLES *handles, SETTINGS *settings) {
    // Get rid of any port buffers first
    if (handles->h264_encoder_pool) {
        if (settings->verbose) {
            logInfo("Destroying h264 encoder pool");
        }
        mmal_port_pool_destroy(handles->h264_encoder->output[0], handles->h264_encoder_pool);
        handles->h264_encoder_pool = NULL;
    }

    if (handles->h264_encoder) {
        if (settings->verbose) {
            logInfo("Destroying h264 encoder component");
        }
        mmal_component_destroy(handles->h264_encoder);
        handles->h264_encoder = NULL;
    }
}
// END H264 FUNCTIONS


// BEGIN YUV FUNCTIONS
// for first iteration, this just copies into previous buffer
int8_t detection_row_batch = -1;
// TODO: update this according to settings, and when settings change (during camera pause/resume)

// this is declared higher so other functions can use it
//unsigned int pixel_delta_threshold = 100;
unsigned int pixel_delta_temp;
void yuvCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata;
    SETTINGS *settings;

    userdata = (CALLBACK_USERDATA *)port->userdata;
    settings = userdata->settings;

    //logInfo("got yuv");
    frame_counter++;

    // TODO: refactor this without locking
    // perhaps increment local counter and compare buffer timestamp
    pthread_mutex_lock(&statsMutex);
    stats.fps++;
    pthread_mutex_unlock(&statsMutex);

    clock_t begin, end;
    if (frame_counter >= motionDetection.detection_at) {
        // Perform motion detection
        begin = clock();

        mmal_buffer_header_mem_lock(buffer);
        memcpy(motionDetection.yuvBuffer, buffer->data, settings->mjpeg.y_length);
        mmal_buffer_header_mem_unlock(buffer);

        uint8_t* p = motionDetection.previousFrame;
        uint8_t* c = motionDetection.yuvBuffer;

	// Save to file
    /*
	char filename[255];
	FILE* f;
	snprintf(filename, 200, "/home/pi/yuv/%d.yuv", frame_counter);
	printf("Saving YUV to %s\n", filename);
	f = fopen(filename, "w+");
	fwrite(c, 1, settings->mjpeg.y_length, f);
	fclose(f);
    */



        detection_row_batch = 1;
        pixel_delta_temp = 0;
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
                    pixel_delta_temp += delta;
                }
            }
        }
        mmal_buffer_header_mem_unlock(buffer);
        end = clock();

        printf("DETECTION1. Pixel delta: %d threshold: %d Time: %f\n", pixel_delta_temp, pixel_delta_threshold, (double)(end - begin) / CLOCKS_PER_SEC);

        if (pixel_delta_temp > pixel_delta_threshold) {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: can still lock around here, but don't need lock above
            motionDetection.motion_count++;
            motionDetection.pixel_delta = pixel_delta_temp;
            // if motion is detected, check again in 2 seconds
            motionDetection.detection_at = frame_counter + (settings->h264.fps * 2);
            pthread_mutex_unlock(&motionDetectionMutex);

            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, buffer->data, settings->mjpeg.y_length);

            // skip second half
            detection_row_batch = 0;
        } else {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: if not reach threshold, set detection_at to frame_counter+1
            // so we process rest of lines in next callback
            motionDetection.motion_count = 0;
            motionDetection.pixel_delta = pixel_delta_temp;
            motionDetection.detection_at = frame_counter + motionDetection.detection_sleep;
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
                    pixel_delta_temp += delta;
                }
            }
        }
        if (detection_row_batch == motionDetection.region.batches) {
            // no more batches
            detection_row_batch = 0;
        }
        end = clock();

        printf("DETECTION2. Pixel delta: %d threshold: %d Time: %f\n", pixel_delta_temp, pixel_delta_threshold, (double)(end - begin) / CLOCKS_PER_SEC);

        // TODO: refactor this to compare delta against non-mutex threshold
        if (pixel_delta_temp > pixel_delta_threshold) {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: can still lock around here, but don't need lock above
            motionDetection.motion_count++;
            motionDetection.pixel_delta = pixel_delta_temp;
            // if motion is detected, check again in 2 seconds
            motionDetection.detection_at = frame_counter - 1 + (settings->h264.fps * 2);
            pthread_mutex_unlock(&motionDetectionMutex);
            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, buffer->data, settings->mjpeg.y_length);

        } else {
            pthread_mutex_lock(&motionDetectionMutex);
            // TODO: if not reach threshold, set detection_at to frame_counter+1
            // so we process rest of lines in next callback
            motionDetection.motion_count = 0;
            motionDetection.pixel_delta = pixel_delta_temp;
            // this should already be set correctly
            //motionDetection.detection_at = frame_counter + motionDetection.detection_sleep;
            pthread_mutex_unlock(&motionDetectionMutex);
        }

    } else if (detection_row_batch == -1) {
        mmal_buffer_header_mem_lock(buffer);
        // First run, copy into previous
        memcpy(motionDetection.previousFrame, buffer->data, settings->mjpeg.y_length );
        mmal_buffer_header_mem_unlock(buffer);
        mmal_buffer_header_release(buffer);
        send_buffers_to_port(port, userdata->handles->yuvPool->queue);
        // next time will compare rows
        detection_row_batch = 1;
        return;
    }

    mmal_buffer_header_release(buffer);
    send_buffers_to_port(port, userdata->handles->yuvPool->queue);
}
// END YUV FUNCTIONS


// HTTP SERVER STUFF



// TODO: clean up these variable names, error messages, etc
void heartbeat(SETTINGS* settings, HANDLES* handles) {
    bool r;

    int sockfd;
    struct addrinfo hints, *heartbeatAddr, *temperatureAddr;
    int rv;
    int numbytes;
    // temperature stuff
    int cpuFd;
    char sTemperature[30];
    int iTemperature;
    char metric[501];
    char hostname[100];

    struct timespec uptime;
    time_t start;

    if (gethostname(hostname, 100) != 0) {
        logError("Failed to gethostname", __func__);
        snprintf(hostname, 100, "INVALID");
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // set to AF_INET to use IPv4
    hints.ai_socktype = SOCK_DGRAM;

    // Heartbeat address info
    // TODO: do we need to re-run these two blocks every time?
    // as the os to fill in addrinfo data for the host and port we want to send to
    if ((rv = getaddrinfo("192.168.1.173", "5001", &hints, &heartbeatAddr)) != 0) {
        logInfo("Failed to getaddrinfo for heartbeat server: %s", gai_strerror(rv));
        return;
    }

    if ((sockfd = socket(heartbeatAddr->ai_family, heartbeatAddr->ai_socktype, heartbeatAddr->ai_protocol)) == -1) {
        logError("Failed to create UDP socket. Bailing on heartbeat.", __func__);
        return;
    }

    // Temperature metrics server info
    if ((rv = getaddrinfo("192.168.1.173", "8125", &hints, &temperatureAddr)) != 0) {
        logInfo("Failed to getaddrinfo for temperature metrics server: %s", gai_strerror(rv));
        return;
    }

    if ((sockfd = socket(temperatureAddr->ai_family, temperatureAddr->ai_socktype, temperatureAddr->ai_protocol)) == -1) {
        logError("Failed to create UDP socket for temperature metrics. Bailing on heartbeat.", __func__);
        return;
    }

    // get start time
    clock_gettime(CLOCK_REALTIME, &uptime);
    start = uptime.tv_sec;


    // Frame annotations
    MMAL_STATUS_T status;
    struct timespec sleep_timespec;
    time_t rawtime;
    struct tm timeinfo;
    MMAL_PARAMETER_CAMERA_ANNOTATE_V4_T annotate;
    char text[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3];
    // Prepare annotation info that doesn't change each loop
    annotate.hdr.id = MMAL_PARAMETER_ANNOTATE;
    annotate.hdr.size = sizeof(MMAL_PARAMETER_CAMERA_ANNOTATE_V4_T);
    annotate.enable = MMAL_TRUE;
    annotate.show_shutter = MMAL_FALSE;
    annotate.show_analog_gain = MMAL_FALSE;
    annotate.show_lens = MMAL_FALSE;
    annotate.show_caf = MMAL_FALSE;
    annotate.show_motion = MMAL_FALSE;
    annotate.show_frame_num = MMAL_FALSE;
    annotate.enable_text_background = MMAL_FALSE;
    annotate.custom_background_colour = MMAL_FALSE;
    annotate.custom_text_colour = MMAL_FALSE;
    annotate.text_size = 0; // choose font size automatically
    annotate.justify = 0;
    annotate.x_offset = 0;
    annotate.y_offset = 0;

    sleep_timespec.tv_sec = 0;
    // TODO: sleep until just before next seconds begins
    // sleep up to start of next second
    /*
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long microsecondsRemaining = (1000000 - tv.tv_usec);
    sleep.tv_sec = 0;
    sleep.tv_nsec = (microsecondsRemaining * 1000);
    */
    sleep_timespec.tv_nsec = 100 * 1000000;


    pthread_mutex_lock(&running_mutex);
    r = running;
    pthread_mutex_unlock(&running_mutex);
    int tenIterations = 0;
    time_t previousSeconds = 0;
    struct timespec delta;
    while (r) {
        uint8_t fps;
        clock_gettime(CLOCK_REALTIME, &delta);

        if (previousSeconds == delta.tv_sec) {
            nanosleep(&sleep_timespec, NULL);
            continue;
        }
        previousSeconds = delta.tv_sec;

        pthread_mutex_lock(&restart_mutex);
        if (restart == 1) {
            int status = mmal_port_parameter_set_boolean(handles->camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 0);
            if (status != MMAL_SUCCESS) {
                logError("Failed to stop camera", __func__);
            }

            // Re-copy settings values
            reconfigureRegion(settings);
            pixel_delta_threshold = settings->pixel_delta_threshold;

            sleep(1);
            status = mmal_port_parameter_set_boolean(handles->camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
            if (status != MMAL_SUCCESS) {
                logError("Failed to restart camera", __func__);
            }
            restart = false;
        }
        pthread_mutex_unlock(&restart_mutex);

        pthread_mutex_lock(&statsMutex);
        fps = stats.fps;
        stats.fps = 0;
        pthread_mutex_unlock(&statsMutex);
        logInfo("STATS. FPS: %d", fps);

        // update time annotation when second has changed
        time(&rawtime);
        localtime_r(&rawtime, &timeinfo);
        strftime(annotate.text, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3, "%Y-%m-%d %H:%M:%S\n", &timeinfo);
        strncat(annotate.text, settings->hostname, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - strlen(settings->hostname) - 1);
        annotate.text[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - 1] = '\0';
        status = mmal_port_parameter_set(handles->camera->control, &annotate.hdr);
        if (status != MMAL_SUCCESS) {
            logError("Failed to set annotation", __func__);
        }


        // send heartbeat once per second
        if ((numbytes = sendto(sockfd, "hi", 2, 0, heartbeatAddr->ai_addr, heartbeatAddr->ai_addrlen)) == -1) {
            logInfo("Failed to send udp heartbeat. errno: %d", errno);
        }

        // read CPU/GPU temperature every 10 seconds
        // compatible with statsd conventions
        /*
        cpu=$(</sys/class/thermal/thermal_zone0/temp)
        echo "$((cpu/1000)) c"
        */
        if (tenIterations == 0) {
            // Send uptime metric
            clock_gettime(CLOCK_REALTIME, &uptime);
            numbytes = snprintf(
                metric,
                500,
                "raspi.pimera.seconds,host=%s:%ld|g\nraspi.pimera.fps,host=%s:%d|g",
                hostname, uptime.tv_sec - start,
                hostname, fps);
            if ((numbytes = sendto(sockfd, metric, numbytes, 0, temperatureAddr->ai_addr, temperatureAddr->ai_addrlen)) == -1) {
                logError("Failed to send pimera uptime", __func__);
            }
            
            tenIterations = 9;

        } else {
            tenIterations--;
        }

        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }
    close(sockfd);
    logInfo("Free 1");
    freeaddrinfo(heartbeatAddr);
    logInfo("Free 2");
    freeaddrinfo(temperatureAddr);
    return;
}



int main(int argc, const char **argv) {
    SETTINGS settings;
    HANDLES handles;
    CALLBACK_USERDATA h264CallbackUserdata;
    CALLBACK_USERDATA yuvCallbackUserdata;
    CALLBACK_USERDATA mjpegCallbackUserdata;

    MMAL_STATUS_T status = MMAL_SUCCESS;

    // threading
    pthread_t httpServerThreadId;
    void *httpServerThreadStatus;
    pthread_t motionDetectionThreadId;
    void *motionDetectionThreadStatus;
    int i, num;

    MMAL_BUFFER_HEADER_T *buffer;


    h264CallbackUserdata.handles = &handles;
    h264CallbackUserdata.settings = &settings;
    h264CallbackUserdata.h264FileKickoffLength = 0;
    yuvCallbackUserdata.handles = &handles;
    yuvCallbackUserdata.settings = &settings;
    mjpegCallbackUserdata.handles = &handles;
    mjpegCallbackUserdata.settings = &settings;


    // todo: is there another way we should init? pthread_once? something else?
    pthread_mutex_init(&httpConnectionsMutex, NULL);
    pthread_mutex_init(&running_mutex, NULL);
    pthread_mutex_init(&motionDetectionMutex, NULL);
    pthread_mutex_init(&statsMutex, NULL);
    pthread_mutex_init(&stream_connections_mutex, NULL);
    pthread_mutex_init(&motion_connections_mutex, NULL);
    pthread_mutex_init(&still_connections_mutex, NULL);
    pthread_mutex_init(&restart_mutex, NULL);

    pthread_mutex_init(&mjpeg_concurrent_mutex, NULL);

    handles.camera = NULL;
    handles.full_splitter = NULL;
    handles.resized_splitter = NULL;
    handles.full_splitter_connection = NULL;
    handles.resized_splitter_connection = NULL;
    handles.h264_encoder = NULL;
    handles.mjpeg_encoder = NULL;
    handles.h264_encoder_connection = NULL;
    handles.mjpeg_encoder_connection = NULL;
    handles.h264_encoder_pool = NULL;
    handles.mjpeg_encoder_pool = NULL;
    handles.settings = &settings;
    handles.h264CallbackUserdata = &h264CallbackUserdata;

    setDefaultSettings(&settings);
    readSettings(&settings);

    initDetection(&settings);
    
    h264_buffer_size = 
        (settings.width * settings.height)
        *
        (settings.h264.fps * 2);
    h264_buffer = (uint8_t*) malloc(h264_buffer_size);
    if (!h264_buffer) {
        logError("FAILED TO ALLOCATE H264 BUFFER", __func__);
        // OF SIZE %u\n", h264_buffer_size);
    }


    bcm_host_init();


    signal(SIGINT, signalHandler);

    //pid_t tid = syscall(SYS_gettid);
    //fprintf(stdout, "Main thread id %d\n", tid);


    if ((status = create_camera(&handles.camera, &settings)) != MMAL_SUCCESS) {
        logError("create_camera failed", __func__);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.full_splitter, &handles.full_splitter_connection, handles.camera->output[CAMERA_VIDEO_PORT], 2)) != MMAL_SUCCESS) {
        logError("create_splitter failed", __func__);
        destroy_camera(handles.camera);
        return EX_ERROR;

    } else if ((status = create_h264_encoder(&handles.h264_encoder, &handles.h264_encoder_pool, &handles.h264_encoder_connection, handles.full_splitter->output[0], h264Callback, &settings.h264)) != MMAL_SUCCESS) {
        logError("createH26create_h264_encoder4Encoder failed",  __func__);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    
    } else if ((status = create_resizer(&handles.resizer, &handles.resizer_connection, handles.full_splitter->output[1], &settings.mjpeg)) != MMAL_SUCCESS) {
        logError("create_resizer failed", __func__);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.resized_splitter, &handles.resized_splitter_connection, handles.resizer->output[0], 2)) != MMAL_SUCCESS) {
        logError("create_splitter failed", __func__);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;


    } else if ((status = create_mjpeg_encoder(&handles.mjpeg_encoder, &handles.mjpeg_encoder_pool, &handles.mjpeg_encoder_connection, handles.resized_splitter->output[0], mjpegCallback, &settings.mjpeg)) != MMAL_SUCCESS) {
        logError("createMjpegEncoder failed", __func__);
        destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    }


    // YUV STUFF LAST MILE
    status = mmal_port_enable(handles.resized_splitter->output[1], yuvCallback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for mjpeg encoder output", __func__);
        destroy_mjpeg_encoder(handles.mjpeg_encoder, handles.mjpeg_encoder_connection, handles.mjpeg_encoder_pool);
        destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    }

    // Now pool for YUV output
    handles.resized_splitter->output[1]->buffer_num = 3;
    handles.resized_splitter->output[1]->buffer_size = settings.mjpeg.width * settings.mjpeg.height * 3;
    fprintf(stdout, "Creating YUV buffer pool with %d bufs of size: %d\n", handles.resized_splitter->output[1]->buffer_num, handles.resized_splitter->output[1]->buffer_size);

    // TODO: we probably don't need a pool. can likely get by with 1 buffer
    handles.yuvPool = mmal_port_pool_create(
        handles.resized_splitter->output[1], handles.resized_splitter->output[1]->buffer_num, handles.resized_splitter->output[1]->buffer_size);
    if (!handles.yuvPool) {
        logError("mmal_port_pool_create failed for mjpeg encoder output", __func__);
        // TODO: what error code for this?
        return MMAL_ENOMEM;
    }
    


    handles.h264_encoder->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)&h264CallbackUserdata;
    logInfo("Sending buffers to h264 port");
    send_buffers_to_port(handles.h264_encoder->output[0], handles.h264_encoder_pool->queue);

    handles.mjpeg_encoder->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)&mjpegCallbackUserdata;
    logInfo("Sending buffers to mjpeg port");
    send_buffers_to_port(handles.mjpeg_encoder->output[0], handles.mjpeg_encoder_pool->queue);

    handles.resized_splitter->output[1]->userdata = (struct MMAL_PORT_USERDATA_T *)&yuvCallbackUserdata;
    logInfo("Sending buffers to YUV port");
    send_buffers_to_port(handles.resized_splitter->output[1], handles.yuvPool->queue);


    // NOW TURN ON THE CAMERA

    // TODO: does this really enable capture? necessary?
    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
    if (status != MMAL_SUCCESS) {
        logError("Toggling MMAL_PARAMETER_CAPTURE to 1 failed", __func__);
        // TODO: unwind
    }


    // set up http server thread
    // set up mjpeg writer thread
    pthread_create(&httpServerThreadId, NULL, httpServer, &settings);
    //pthread_create(&motionDetectionThreadId, NULL, motionDetectionThread, (void*)&handles);

    // heartbeat loop
    heartbeat(&settings, &handles);


    logInfo("Waiting for threads");
    logInfo("hi 1");
    // This bails out of the thread, so we can exit
    pthread_cancel(httpServerThreadId);
    //pthread_cancel(motionDetectionThreadId);
    // But we want to gracefully exit the mjpeg thread, instead of bail
    logInfo("hi 2");
    pthread_join(httpServerThreadId, &httpServerThreadStatus);
    logInfo("hi 3");
    //pthread_join(motionDetectionThreadId, &motionDetectionThreadStatus);

    // TODO: flush buffers before we exit?

    if (motionDetection.fd) {
        close(motionDetection.fd);

    }

    if (settings.verbose) {
        logInfo("Shutting down");
    }

    // TODO: the below logic is broken
    // do i disable leaf ports first, then connections?

    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 0);
    if (status != MMAL_SUCCESS) {
        logError("Toggling MMAL_PARAMETER_CAPTURE to 0 failed", __func__);
        // TODO: unwind
    }

    
    // Disable all our ports that are not handled by connections
    logInfo("hi 4");
    //disablePort(&settings, cameraVideoPort, "camera");
    //disablePort(&settings, splitterH264Port, "splitter h264");
    //disablePort(&settings, splitterMjpegPort, "splitter mjpeg");
    disable_port(handles.resized_splitter->output[1], "splitter yuv");
    // disabling this causes a segfault ...
    disable_port(handles.h264_encoder->output[0], "h264 encoder output");
    disable_port(handles.mjpeg_encoder->output[0], "mjpeg encoder output");

    logInfo("hi 5");
    destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
    destroy_resizer(handles.resizer, handles.resizer_connection);
    destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
    destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
    destroy_camera(handles.camera);



    // clean up the processing queues
    logInfo("Cleaning up processing queues");

    freeDetection();
    //free(h264_buffer);


    if (settings.verbose) {
        logInfo("Shutdown complete");
    }


}
