#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
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


#define CAMERA_VIDEO_PORT 1
#define SPLITTER_H264_PORT 1
//#define SPLITTER_MJPEG_PORT 1
#define SPLITTER_RESIZER_PORT 0

#define EX_ERROR 1

#define DESIRED_OUTPUT_BUFFERS 3
#define H264_BITRATE  25000000 // 62.5Mbit/s OR 25000000 25Mbits/s

int verbose = 1;


typedef struct {
    int width; // default 1920
    int height; // default 1080
    int vcosWidth;
    int vcosHeight;

    struct {
        unsigned int width;
        unsigned int height;
        int vcosWidth;
        int vcosHeight;
        unsigned int y_length;

        unsigned int fps; // default 30
        unsigned int keyframePeriod; // default 1000
    } h264;
    struct {
        unsigned int width;
        unsigned int height;
        int vcosWidth;
        int vcosHeight;
        unsigned int y_length;
    } mjpeg;

    unsigned int region[4];

    
    int motionCheckFrequency; // default 3 per second
    // TODO: rename this using vaues from libcamera version
    unsigned int changed_pixels_threshold;
    char objectDetectionEndpoint[128];
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
} SETTINGS;

// this is global, right?
typedef struct {
    MMAL_COMPONENT_T *camera;    /// Pointer to the camera component
    MMAL_COMPONENT_T *splitter;  /// Pointer to the splitter component
    MMAL_COMPONENT_T *h264_encoder;   /// Pointer to the encoder component
    MMAL_COMPONENT_T *mjpeg_encoder;   /// Pointer to the encoder component
    MMAL_COMPONENT_T *resizer;
    MMAL_CONNECTION_T *splitter_connection;/// Pointer to the connection from camera to splitter
    MMAL_CONNECTION_T *h264_encoder_connection; /// Pointer to the connection from camera to encoder
    MMAL_CONNECTION_T *mjpegEncoderConnection;
    MMAL_CONNECTION_T *resizer_connection;


    MMAL_PORT_T* h264EncoderOutputPort;
    MMAL_PORT_T* splitterBgrPort;

    MMAL_POOL_T *h264_encoder_pool; /// Pointer to the pool of buffers used by splitter output port 0
    MMAL_POOL_T *mjpegEncoderPool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *bgrPool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *resizer_pool;

    SETTINGS* settings;
} HANDLES;


typedef struct {
    HANDLES *handles;
    SETTINGS *settings;
} CALLBACK_USERDATA;

typedef struct {
    uint8_t yuv_fps;
    uint8_t h264_fps;
} STATS_T;
STATS_T stats;
pthread_mutex_t statsMutex;


bool running = true;
pthread_mutex_t running_mutex;


void logError(char const* msg, const char *func) {
    // time prefix
    char t[40];
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);
    localtime_r(&rawtime, &timeinfo);
    strftime(t, 40, "%Y-%m-%d %H:%M:%S", &timeinfo);

    fprintf(stdout, "%s [ERR] in %s: %s\n", t, func, msg);
    fflush(stdout);
}
void logInfo(char const* fmt, ...) {
    // time prefix
    char t[40];
    time_t rawtime;
    struct tm timeinfo;
    va_list ap;
    time(&rawtime);
    localtime_r(&rawtime, &timeinfo);
    strftime(t, 40, "%Y-%m-%d %H:%M:%S", &timeinfo);

    fprintf(stdout, "%s [INFO] ", t);

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}

void signalHandler(int signal_number) {
    logError("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = false;
    pthread_mutex_unlock(&running_mutex);
    logInfo("Signaled");
}



void setDefaultSettings(SETTINGS* settings) {
    char* c;

    // 1640x922 uses full sensor field-of-view, which is helpful
    settings->width = 1640;
    settings->height = 922;

    settings->region[0] = 0;
    settings->region[1] = 0;
    settings->region[2] = 100;
    settings->region[3] = 100;
    settings->changed_pixels_threshold = 500;

    settings->vcosWidth = ALIGN_UP(settings->width, 16);
    settings->vcosHeight = ALIGN_UP(settings->height, 16);
    settings->h264.fps = 30;
    settings->h264.keyframePeriod = 1000;
    // TODO: rename this
    settings->motionCheckFrequency = 3;
    settings->objectDetectionEndpoint[0] = 0;
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
    settings->mjpeg.width = 820;
    settings->mjpeg.height = 461;
    settings->mjpeg.vcosWidth = ALIGN_UP(settings->mjpeg.width, 16);
    settings->mjpeg.vcosHeight = ALIGN_UP(settings->mjpeg.height, 16);

    settings->mjpeg.y_length = settings->mjpeg.vcosWidth * settings->mjpeg.vcosHeight * 1.5;
    
    c = getenv("DEBUG");
    if (c) {
        logInfo("Enabling debug");
        settings->debug = true;
    }

}


// MMAL HELPER FUNCTIONS

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


void destroyComponent(SETTINGS *settings, MMAL_COMPONENT_T *component, char const* description) {
    if (!component) {
        if (verbose) {
            fprintf(stdout, "[INFO] Nothing to destroy. %s component is NULL\n", description);
        }
        return;
    }
    if (verbose) {
        fprintf(stdout, "[INFO] Destroying %s component\n", description);
    }
    mmal_component_destroy(component);
}


void cameraControlCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    fprintf(stdout, " HEY cameraControlCallback, buffer->cmd: %d\n", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

static void destroy_camera(HANDLES *handles) {
    if (handles->camera) {
        if (verbose) {
            logInfo("Destroying camera");
        }
        mmal_component_destroy(handles->camera);
        handles->camera = NULL;
    }
}

static MMAL_STATUS_T create_camera(HANDLES *handles) {
    SETTINGS* settings = handles->settings;
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
        destroy_camera(handles);
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
    format->encoding_variant = MMAL_ENCODING_I420;

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

    handles->camera = camera;


    if (verbose) {
        logInfo("Camera created");
    }

    return status;
}
// END CAMERA FUNCTIONS



// SPLITTER FUNCTIONS
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
    if (verbose) {
        logInfo("Splitter created");
    }

    if (verbose) {
        logInfo("Connecting camera video port to splitter input port");
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
        logError("connectEnable failed for camera video port to splitter input", __func__);
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
        if (verbose) {
            logInfo("Destroying splitter connection");
        }
    }
    mmal_connection_destroy(connection);


    if (splitter) {
        if (verbose) {
            logInfo("Destroying splitter");
        }
        mmal_component_destroy(splitter);
    }
}


static MMAL_STATUS_T create_resizer(MMAL_COMPONENT_T **handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port) {
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

    // Tell resizer which format it'll be receiving from the camera video output
    mmal_format_copy(resizer->input[0]->format, output_port->format);

    status = mmal_port_format_commit(resizer->input[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on resizer input", __func__);
        mmal_component_destroy(resizer);
        return status;
    }

    status = mmal_component_enable(resizer);
    if (status != MMAL_SUCCESS) {
        logError("mmal_component_enable failed on resizer", __func__);
        mmal_component_destroy(resizer);
        return status;
    }


    if (verbose) {
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
    if (verbose) {
        logInfo("Resizer created");
    }
    return status;
}

static void destroy_resizer(MMAL_COMPONENT_T *splitter, MMAL_CONNECTION_T *connection) {
    if (connection) {
        if (verbose) {
            logInfo("Destroying resizer connection");
        }
    }
    mmal_connection_destroy(connection);


    if (splitter) {
        if (verbose) {
            logInfo("Destroying resizer");
        }
        mmal_component_destroy(splitter);
    }
}


void h264_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata = (CALLBACK_USERDATA *)port->userdata;

    pthread_mutex_lock(&statsMutex);
    stats.h264_fps++;
    pthread_mutex_unlock(&statsMutex);

    mmal_buffer_header_release(buffer);

    send_buffers_to_port(port, userdata->handles->h264_encoder_pool->queue);
}

static void destroy_h264_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool) {
    if (connection) {
        mmal_connection_destroy(connection);
    }

    // Get rid of any port buffers first
    if (pool) {
        if (verbose) {
            logInfo("Destroying h264 encoder pool");
        }
        mmal_port_pool_destroy(component->output[0], pool);
    }

    if (component) {
        if (verbose) {
            logInfo("Destroying h264 encoder component");
        }
        mmal_component_destroy(component);
    }
}

static MMAL_STATUS_T create_h264_encoder(MMAL_COMPONENT_T **handle, MMAL_POOL_T **pool_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, SETTINGS *settings) {
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


    if (verbose) {
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

        if (verbose) {
        logInfo("Enabling h264 encoder output port");
    }

    status = mmal_port_enable(encoder_output, h264_callback);
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

    if (verbose) {
        logInfo("Created H264 encoder");
    }

    return status;
}










void resizer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata;
    SETTINGS *settings;

    userdata = (CALLBACK_USERDATA *)port->userdata;
    settings = userdata->settings;

    //logInfo("Got buffer. Length: %d", buffer->length);

    pthread_mutex_lock(&statsMutex);
    stats.yuv_fps++;
    pthread_mutex_unlock(&statsMutex);

    mmal_buffer_header_release(buffer);


    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T* new_buffer;
        while ( (new_buffer = mmal_queue_get(userdata->handles->resizer_pool->queue)) ) {
            status = mmal_port_send_buffer(port, new_buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed, no buffer to return to resizer port\n", __func__);
                break;
            }
        }
    }
}


void heartbeat(HANDLES* handles) {
    SETTINGS* settings = handles->settings;
    bool r;
    char hostname[100];

    struct timespec uptime;
    time_t start;

    if (gethostname(hostname, 100) != 0) {
        logError("Failed to gethostname", __func__);
        snprintf(hostname, 100, "INVALID");
    }


    // get start time
    clock_gettime(CLOCK_REALTIME, &uptime);
    start = uptime.tv_sec;


    // Frame annotations
    MMAL_STATUS_T status;
    struct timespec sleep;
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

    sleep.tv_sec = 0;
    // TODO: sleep until just before next seconds begins
    // sleep up to start of next second
    /*
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long microsecondsRemaining = (1000000 - tv.tv_usec);
    sleep.tv_sec = 0;
    sleep.tv_nsec = (microsecondsRemaining * 1000);
    */
    sleep.tv_nsec = 100 * 1000000;


    pthread_mutex_lock(&running_mutex);
    r = running;
    pthread_mutex_unlock(&running_mutex);
    int tenIterations = 0;
    time_t previousSeconds = 0;
    struct timespec delta;
    while (r) {
        clock_gettime(CLOCK_REALTIME, &delta);

        if (previousSeconds == delta.tv_sec) {
            nanosleep(&sleep, NULL);
            continue;
        }
        previousSeconds = delta.tv_sec;

        pthread_mutex_lock(&statsMutex);
        logInfo("STATS. YUV FPS: %d | H264 FPS: %d", stats.yuv_fps, stats.h264_fps);
        stats.yuv_fps = stats.h264_fps = 0;
        pthread_mutex_unlock(&statsMutex);

        // update time annotation when second has changed
        time(&rawtime);
        localtime_r(&rawtime, &timeinfo);
        strftime(annotate.text, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3, "%Y-%m-%d %H:%M:%S\n", &timeinfo);
        strncat(annotate.text, settings->hostname, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - strlen(settings->hostname) - 1);
        annotate.text[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - 1] = '\0';
        status = mmal_port_parameter_set(handles->camera->control, &annotate.hdr);
        if (status != MMAL_SUCCESS) {
            fprintf(stdout, "FAILED TO SET TIME %d\n", status);
        }


        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }

    return;
}


int main(int argc, const char **argv) {
    SETTINGS settings;
    HANDLES handles;
    CALLBACK_USERDATA callbackUserdata;

    MMAL_STATUS_T status = MMAL_SUCCESS;

    int i, num;

    MMAL_BUFFER_HEADER_T *buffer; // ??

    callbackUserdata.handles = &handles;
    callbackUserdata.settings = &settings;

    pthread_mutex_init(&statsMutex, NULL);
    pthread_mutex_init(&running_mutex, NULL);


    handles.camera = NULL;
    handles.splitter = NULL;
    handles.h264_encoder = NULL;
    handles.mjpeg_encoder = NULL;
    handles.splitter_connection = NULL;
    handles.h264_encoder_connection = NULL;
    handles.mjpegEncoderConnection = NULL;
    handles.h264_encoder_pool = NULL;
    handles.mjpegEncoderPool = NULL;
    handles.settings = &settings;

    setDefaultSettings(&settings);
    //readSettings(&settings);

    /*
    h264_buffer_size = 
        (settings.width * settings.height)
        *
        (settings.h264.fps * 2);
    h264_buffer = (uint8_t*) malloc(h264_buffer_size);
    if (!h264_buffer) {
        fprintf(stderr, "FAILED TO ALLOCATE H264 BUFFER OF SIZE %u\n", h264_buffer_size);
    }
    */
    


    bcm_host_init();


    signal(SIGINT, signalHandler);


    if ((status = create_camera(&handles)) != MMAL_SUCCESS) {
        logError("create_camera failed", __func__);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.splitter, &handles.splitter_connection, handles.camera->output[CAMERA_VIDEO_PORT], 1)) != MMAL_SUCCESS) {
        logError("create_splitter failed", __func__);
        destroy_camera(&handles);
        return EX_ERROR;
    } else if ((status = create_resizer(&handles.resizer, &handles.resizer_connection, handles.splitter->output[SPLITTER_RESIZER_PORT])) != MMAL_SUCCESS) {
    //else if ((status = create_resizer(&handles.resizer, &handles.resizer_connection, handles.camera->output[CAMERA_VIDEO_PORT])) != MMAL_SUCCESS) {
        logError("create_resizer failed", __func__);
        destroy_splitter(handles.splitter, handles.splitter_connection);
        destroy_camera(&handles);
        return EX_ERROR;
    } else if ((status = create_h264_encoder(&handles.h264_encoder, &handles.h264_encoder_pool, &handles.h264_encoder_connection, handles.splitter->output[SPLITTER_H264_PORT], &settings)) != MMAL_SUCCESS) {
        logError("createH26create_h264_encoder4Encoder failed",  __func__);
        destroy_splitter(handles.splitter, handles.splitter_connection);
        destroy_camera(&handles);
        return EX_ERROR;
    }

/*
    

    } else if ((status = createMjpegEncoder(&handles)) != MMAL_SUCCESS) {
        logError("createMjpegEncoder failed", __func__);
        destroyH264Encoder(&handles);
        destroy_splitter(&handles);
        destroy_camera(&handles);
        return EX_ERROR;
    }
*/

    
    // Major components have been created, let's hook them together
    
    if (verbose) {
        logInfo("Connecting things together now");
    }




    // RESIZER SETUP
    MMAL_ES_FORMAT_T *resizer_format = handles.resizer->output[0]->format;
    handles.resizer->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)&callbackUserdata;
    
    // configure output format
    // need both of these to move from opaque format
    resizer_format->encoding = MMAL_ENCODING_I420;
    resizer_format->encoding_variant = MMAL_ENCODING_I420;

    resizer_format->es->video.width = handles.settings->mjpeg.width;
    resizer_format->es->video.height = handles.settings->mjpeg.height;
    resizer_format->es->video.crop.x = 0;
    resizer_format->es->video.crop.y = 0;
    resizer_format->es->video.crop.width = handles.settings->mjpeg.width;
    resizer_format->es->video.crop.height = handles.settings->mjpeg.height;

    status = mmal_port_format_commit(handles.resizer->output[0]);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_format_commit failed on resizer output port", __func__);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_splitter(handles.splitter, handles.splitter_connection);
        destroy_camera(&handles);
        return status;
    }


    /*
    handles.resizer->output[0]->buffer_size = handles.settings->mjpeg.width * handles.settings->mjpeg.height * 1.5;
    //op->buffer_size = op->buffer_size_recommended;
    handles.resizer->output[0]->buffer_num = DESIRED_OUTPUT_BUFFERS;
    */

    handles.resizer->output[0]->buffer_size = 
    handles.resizer->output[0]->buffer_size_recommended;
    handles.resizer->output[0]->buffer_num = 
    handles.resizer->output[0]->buffer_num_recommended;

    if (verbose) {
        logInfo("Enabling resizer output port");
    }

    status = mmal_port_enable(handles.resizer->output[0], resizer_callback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for resizer output", __func__);
        // disconnect and disable first?
        destroy_splitter(handles.splitter, handles.splitter_connection);
        destroy_camera(&handles);
        return EX_ERROR;
    }


    // make a pool
    int bgr_buffer_size = handles.resizer->output[0]->buffer_size;
    int bgr_num_buffers = handles.resizer->output[0]->buffer_num;

    logInfo("Creating YUV buffer pool with %d bufs of size: %d", bgr_num_buffers, bgr_buffer_size);
    handles.resizer_pool = mmal_port_pool_create(handles.resizer->output[0], bgr_num_buffers, bgr_buffer_size);
    if (!handles.resizer_pool) {
        logError("mmal_port_pool_create failed for YUV output", __func__);
        // TODO: what error code for this?
    } else {
        if (verbose) {
            logInfo("Created YUV pool");
        }
    }





    if (verbose) {
        logInfo("Providing buffers to resizer output");
    }
    send_buffers_to_port(handles.resizer->output[0], handles.resizer_pool->queue);




    // H264 ENCODER SETUP
    handles.h264_encoder->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)&callbackUserdata;

    if (verbose) {
        logInfo("Providing buffers to h264 encoder output");
    }
    send_buffers_to_port(handles.h264_encoder->output[0], handles.h264_encoder_pool->queue);





    // TODO: does this really enable capture? necessary?
    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
    if (status != MMAL_SUCCESS) {
        logError("Toggling MMAL_PARAMETER_CAPTURE to 1 failed", __func__);
        // TODO: unwind
    }


    // heartbeat loop
    heartbeat(&handles);


    logInfo("Waiting for threads");
    // This bails out of the thread, so we can exit
    /*
    pthread_cancel(httpServerThreadId);
    pthread_join(httpServerThreadId, &httpServerThreadStatus);
    */


    if (verbose) {
        logInfo("Shutting down");
    }

    // TODO: the below logic is broken
    // do i disable leaf ports first, then connections?

    /*
    
    // Disable all our ports that are not handled by connections
    logInfo("hi 4");
    //disablePort(&settings, cameraVideoPort, "camera");
    //disablePort(&settings, splitterH264Port, "splitter h264");
    //disablePort(&settings, splitterMjpegPort, "splitter mjpeg");
    disablePort(&settings, splitterBgrPort, "splitter bgr");
    // disabling this causes a segfault ...
    disablePort(&settings, h264EncoderOutputPort, "h264 encoder output");
    disablePort(&settings, mjpegEncoderOutputPort, "mjpeg encoder output");

    logInfo("hi 5");
    destroyConnection(&settings, handles.h264EncoderConnection, "h264 encoder");
    destroyConnection(&settings, handles.mjpegEncoderConnection, "mjpeg encoder");
    destroyConnection(&settings, handles.splitter_connection, "splitter");

    logInfo("hi 6");
    disableComponent(&settings, handles.h264_encoder, "h264 encoder");
    disableComponent(&settings, handles.mjpeg_encoder, "mjpeg encoder");
    disableComponent(&settings, handles.splitter, "splitter");
    disableComponent(&settings, handles.camera, "camera");

    logInfo("hi 7");
    destroyH264Encoder(&handles, &settings);
    destroyMjpegEncoder(&handles, &settings);

    logInfo("hi 8");
    // TODO: destroy pools?
    // destroy bgr buffer pool
    if (handles.bgrPool) {
        if (verbose) {
            logInfo("Destroying bgr pool");
        }
        mmal_port_pool_destroy(splitterBgrPort, handles.bgrPool);
        handles.bgrPool = NULL;
    }
    destroy_splitter(handles.splitter, handles.splitter_connection);
    destroy_camera(&handles, &settings);
    */


    // clean up the processing queues
    logInfo("Cleaning up processing queues");


    if (verbose) {
        logInfo("Shutdown complete");
    }


}
