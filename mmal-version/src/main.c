#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
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



typedef struct {
    int detection_sleep;
    int detection_at;
    int stream_sleep;
    uint8_t *previousFrame;

    unsigned int* regions;
    unsigned int regions_length;
    unsigned int changed_pixels_threshold;

    char boundary[81];
    int boundaryLength;

    unsigned int motion_count;
    char filename1[200];
    char filename2[200];
    int fd;

} MOTION_DETECTION_T;
MOTION_DETECTION_T motionDetection;
pthread_mutex_t motionDetectionMutex;

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


bool running = true;
// threading stuff
pthread_mutex_t httpConnectionsMutex;
pthread_mutex_t running_mutex;
unsigned int frame_counter = 0;


// end new processing variables

// HTTP CONNECTIONS
typedef struct connection {
    int fd;
    struct connection *prev, *next;
} connection;
connection *stream_connections = NULL;
connection *motion_connections = NULL;
connection *still_connections = NULL;
pthread_mutex_t stream_connections_mutex;
pthread_mutex_t motion_connections_mutex;
pthread_mutex_t still_connections_mutex;
connection* create_connection(int fd) {
    connection *conn = (connection*) malloc(sizeof(struct connection));
    conn->fd = fd;
    conn->next = conn->prev = NULL;
    return conn;
}
connection* find_connection(connection* head, int fd) {
    while (head) {
        if (head->fd == fd) {
            return head;
        }
        head = head->next;
    }
    return NULL;
}
void clear_connections(connection* head) {
    while (head) {
        connection *h = head;
        head = head->next;
        free(h);
    }
}
#define MAX_POLL_FDS 100
struct pollfd http_fds[MAX_POLL_FDS];
int http_fds_count = 0;



typedef struct HANDLES_S HANDLES;
typedef struct SETTINGS_S SETTINGS;

// https://stackoverflow.com/questions/1675351/typedef-struct-vs-struct-definitions/
typedef struct {
    HANDLES *handles;
    SETTINGS *settings;
    char h264FileKickoff[128];
    uint32_t h264FileKickoffLength;
    int abort;
} CALLBACK_USERDATA;


struct SETTINGS_S {
    int width; // default 1920
    int height; // default 1080
    int vcosWidth;
    int vcosHeight;

    struct {
        unsigned int width;
        unsigned int height;
        unsigned int y_length;

        unsigned int fps; // default 30
        unsigned int keyframePeriod; // default 1000
    } h264;
    struct {
        unsigned int width;
        unsigned int height;
        unsigned int y_length;
    } mjpeg;

    unsigned int region[4];

    
    int motion_check_frequency; // default 3 per second
    unsigned int changed_pixels_threshold;
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

struct HANDLES_S {
    MMAL_COMPONENT_T *camera;    /// Pointer to the camera component
    MMAL_COMPONENT_T *splitter;  /// Pointer to the splitter component
    MMAL_COMPONENT_T *h264_encoder;   /// Pointer to the encoder component
    MMAL_COMPONENT_T *mjpeg_encoder;   /// Pointer to the encoder component
    MMAL_CONNECTION_T *splitterConnection;/// Pointer to the connection from camera to splitter
    MMAL_CONNECTION_T *h264EncoderConnection; /// Pointer to the connection from camera to encoder
    MMAL_CONNECTION_T *mjpegEncoderConnection;

    MMAL_PORT_T* h264EncoderOutputPort;
    MMAL_PORT_T* splitterYuvPort;

    MMAL_POOL_T *h264EncoderPool; /// Pointer to the pool of buffers used by splitter output port 0
    MMAL_POOL_T *mjpegEncoderPool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *yuvPool; /// Pointer to the pool of buffers used by encoder output port

    SETTINGS* settings;
    CALLBACK_USERDATA* h264CallbackUserdata;        /// Used to move data to the encoder callback
};


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


void initDetection(SETTINGS* settings) {
    motionDetection.detection_sleep = settings->h264.fps / settings->motion_check_frequency;
    motionDetection.detection_at = motionDetection.detection_sleep;

    motionDetection.stream_sleep = settings->h264.fps / 3;

    motionDetection.previousFrame = (uint8_t*) malloc( settings->mjpeg.y_length );

    motionDetection.motion_count = 0;

    strncpy(motionDetection.boundary, "--HATCHA\r\nContent-Type: image/jpeg\r\nContent-length: ", 80);
    motionDetection.boundaryLength = strlen(motionDetection.boundary);


    int start_x = 100;
    int start_y = 100;
    int width = 100;
    int height = 100;
    int end_y = start_y + height;
    int end_x = start_x + width;

    // Based on detection region, make offset pairs (start, stop)
    motionDetection.regions_length = (settings->region[3] - settings->region[1]) * 2;
    logInfo("Allocating %d", motionDetection.regions_length);
    motionDetection.regions = (unsigned int*) calloc(motionDetection.regions_length, sizeof(unsigned int));

    unsigned int x_start, x_end, y_start, y_end;
    x_start = settings->region[0];
    y_start = settings->region[1];
    x_end = settings->region[2];
    y_end = settings->region[3];

    unsigned int i = 0;
    for (unsigned int y = y_start; y < y_end; y++) {
        unsigned int offset = (y * settings->vcosWidth) + x_start;

        motionDetection.regions[ i++ ] = offset;
        motionDetection.regions[ i++ ] = offset + x_end;
    }


    /*
    for (int y = start_y; y < )
    for (x = start)

    i = (y * settings->mjpeg.stride) + x;
    */

}
void freeDetection() {
    free(motionDetection.previousFrame);
    free(motionDetection.regions);
}

void setDefaultSettings(SETTINGS* settings) {
    char* c;

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

    settings->vcosWidth = ALIGN_UP(settings->width, 16);
    settings->vcosHeight = ALIGN_UP(settings->height, 16);
    settings->h264.fps = 10;
    settings->h264.keyframePeriod = 1000;
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
    settings->mjpeg.width = settings->width;
    settings->mjpeg.height = settings->height;
    settings->mjpeg.y_length = settings->width * settings->height;
    
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
    settings->vcosWidth = ALIGN_UP(settings->width, 16);
    settings->vcosHeight = ALIGN_UP(settings->height, 16);
    settings->mjpeg.width = settings->width;
    settings->mjpeg.height = settings->height;
    settings->mjpeg.y_length = settings->width * settings->height;

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

    fprintf(stdout, "[INFO] Settings. %dx%d @ fps: %d motion_check_frequency: %d\n", settings->width, settings->height, settings->h264.fps, settings->motion_check_frequency);


    ini_destroy(ini);
    return;
}


// HELPER FUNCTIONS

int sendSocket(int socket, char* data, int length) {
    int totalWritten = 0;
    int written = 0;
    int remaining = length;
    int attempts = 0;
    
    // TODO: think through the attempts more
    while (totalWritten < length) {
        written = send(socket, data, remaining, MSG_NOSIGNAL);
        // if fail to write 3 times, bail

        attempts++;
        if (attempts >= 3) {
            logError("Three failed attempts to write to socket", __func__);
            return -1;
        }
        remaining -= written;
        data += written;
        totalWritten += written;
    }
    return totalWritten;
}

void sendBuffersToPort(MMAL_PORT_T* port, MMAL_QUEUE_T* queue) {
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
void disablePort(SETTINGS* settings, MMAL_PORT_T *port, char const* description) {
    if (settings->verbose) {
        fprintf(stdout, "[INFO] Disabling %s port\n", description);
    }
    if (!port) {
        if (settings->verbose) {
            fprintf(stdout, "[INFO] Nothing to disable. %s port is NULL\n", description);
        }
        return;
    }
    if (!port->is_enabled) {
        if (settings->verbose) {
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

static void destroyCamera(HANDLES *handles, SETTINGS *settings) {
    if (handles->camera) {
        if (settings->verbose) {
            logInfo("Destroying camera");
        }
        mmal_component_destroy(handles->camera);
        handles->camera = NULL;
    }
}

static MMAL_STATUS_T createCamera(HANDLES *handles, SETTINGS* settings) {
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
        destroyCamera(handles, settings);
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


    if (settings->verbose) {
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
// END SPLITTER FUNCTIONS



// MJPEG ENCODER FUNCTIONS
static MMAL_STATUS_T createMjpegEncoder(HANDLES *handles, SETTINGS *settings) {
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *splitter_output;
    MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

    if (status != MMAL_SUCCESS) {
        logError("mmal_component_create failed for mjpeg encoder", __func__);
        return status;
    }

    splitter_output = handles->splitter->output[SPLITTER_MJPEG_PORT];
    encoder_input = encoder->input[0];
    encoder_output = encoder->output[0];

    // Encoder input should match splitter output format,right?
    // TODO: this seems wrong
    mmal_format_copy(encoder_input->format, splitter_output->format);

    encoder_output->format->encoding = MMAL_ENCODING_MJPEG;
    // TODO: what should this be? should it vary based on resolution?
    encoder_output->format->bitrate = MJPEG_BITRATE;

    // TODO: this doesn't work
    /*
    encoder_output->format->es->video.width = 640;
    encoder_output->format->es->video.height = 480;
    encoder_output->format->es->video.crop.width = 640;
    encoder_output->format->es->video.crop.height = 480;
    */


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

    fprintf(stdout, "Creating mjpeg buffer pool with %d bufs of size: %d\n", encoder_output->buffer_num, encoder_output->buffer_size);

    // TODO: we probably don't need a pool. can likely get by with 1 buffer
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
    if (!pool) {
        logError("mmal_port_pool_create failed for mjpeg encoder output", __func__);
        // TODO: what error code for this?
        return MMAL_ENOMEM;
    }

    handles->mjpegEncoderPool = pool;
    handles->mjpeg_encoder = encoder;

    if (settings->verbose) {
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

    pthread_mutex_lock(&mjpeg_concurrent_mutex);
    if (mjpeg_concurrent > 0) {
	    printf("mjpegCallback already in process\n");
    }
    mjpeg_concurrent++;
    pthread_mutex_unlock(&mjpeg_concurrent_mutex);

    
    if (!userdata) {
        logError("Expected userdata in mjpeg callback", __func__);
        mmal_buffer_header_release(buffer);
        sendBuffersToPort(port, userdata->handles->mjpegEncoderPool->queue);

	pthread_mutex_lock(&mjpeg_concurrent_mutex);
	mjpeg_concurrent--;
	pthread_mutex_unlock(&mjpeg_concurrent_mutex);
        return;
    }

    if (buffer->length == 0) {
        logError("No data in mjpeg callback buffer", __func__);
        mmal_buffer_header_release(buffer);
        sendBuffersToPort(port, userdata->handles->mjpegEncoderPool->queue);

	pthread_mutex_lock(&mjpeg_concurrent_mutex);
	mjpeg_concurrent--;
	pthread_mutex_unlock(&mjpeg_concurrent_mutex);
        return;
    }

    //fprintf(stdout, "mjpeg pts: %" PRId64 "\n", buffer->pts);
    

    //logInfo("Buffer with mjpeg data!");
    //fprintf(stdout, "[INFO] mjpeg buffer length %d\n", buffer->length);

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

    sendBuffersToPort(port, userdata->handles->mjpegEncoderPool->queue);

    pthread_mutex_lock(&mjpeg_concurrent_mutex);
    mjpeg_concurrent--;
    pthread_mutex_unlock(&mjpeg_concurrent_mutex);
}

void destroyMjpegEncoder(HANDLES *handles, SETTINGS *settings) {
    if (handles->mjpegEncoderPool) {
        if (settings->verbose) {
            logInfo("Destroying mjpeg encoder pool");
        }
        mmal_port_pool_destroy(handles->mjpeg_encoder->output[0], handles->mjpegEncoderPool);
        handles->mjpegEncoderPool = NULL;
    }

    if (handles->mjpeg_encoder) {
        if (settings->verbose) {
            logInfo("Destroying mjpeg encoder component");
        }
        mmal_component_destroy(handles->mjpeg_encoder);
        handles->mjpeg_encoder = NULL;
    }
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


    handles->h264EncoderPool = pool;
    handles->h264_encoder = encoder;

    if (settings->verbose) {
        logInfo("Created H264 encoder");
    }

    return status;
}


void sendH264Buffers(MMAL_PORT_T* port, CALLBACK_USERDATA* userdata) {
    // TODO: move this to main thread
    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T* new_buffer;
        while ( (new_buffer = mmal_queue_get(userdata->handles->h264EncoderPool->queue)) ) {
            status = mmal_port_send_buffer(port, new_buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed, no buffer to return to h264 encoder port\n", __func__);
                break;
            }
        }
    }
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

    //logInfo("got h264");

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

    //fprintf(stdout, "h264 pts: %" PRId64 "\n", buffer->pts);
    
    // TODO: concat this buffer with previous one containing timestamp
    // see here: https://forums.raspberrypi.com/viewtopic.php?t=220074


    if (debug) {
        fprintf(
            stdout, "[INFO] frame flags: %8s %8s %8s %8s %10s %10s %10s %10s %10d %10d\n",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS) ? "eos" : "!eos",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_START) ? "start" : "!start",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) ? "end" : "!end",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME) ? "frame" : "!frame",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME) ? "keyframe" : "!keyframe",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) ? "config" : "!config",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) ? "sideinfo" : "!sideinfo",
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_NAL_END) ? "nalend" : "!nalend",
            buffer->offset,
            buffer->length
        );
    }
    

    if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) {
        
        logInfo("Keeping h264 config data for later");

        if (buffer->length > 128) {
            logError("Not enough space in h264_file_kickoff", __func__);
        } else {
            logInfo("Keeping h264 config data for later");
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

        } else {
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
    if (handles->h264EncoderPool) {
        if (settings->verbose) {
            logInfo("Destroying h264 encoder pool");
        }
        mmal_port_pool_destroy(handles->h264_encoder->output[0], handles->h264EncoderPool);
        handles->h264EncoderPool = NULL;
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
void yuvCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    CALLBACK_USERDATA *userdata;
    SETTINGS *settings;

    userdata = (CALLBACK_USERDATA *)port->userdata;
    settings = userdata->settings;

    //logInfo("got yuv");
    frame_counter++;

    pthread_mutex_lock(&statsMutex);
    stats.fps++;
    pthread_mutex_unlock(&statsMutex);

    // fprintf(stdout, "yuv pts: %" PRId64 "\n", buffer->pts);

    // Does buffer have data?
    if (frame_counter == 1) {
        mmal_buffer_header_mem_lock(buffer);
        memcpy( motionDetection.previousFrame, buffer->data, settings->mjpeg.y_length );
        mmal_buffer_header_mem_unlock(buffer);

    } else if (frame_counter >= motionDetection.detection_at) {
        clock_t begin = clock();

        mmal_buffer_header_mem_lock(buffer);
        uint8_t* p = motionDetection.previousFrame;
        uint8_t* c = buffer->data;
        unsigned int changed_pixels = 0;

        /*
        int y_length = 400 * 400; //settings->mjpeg.y_length;
        for (int i = 0; i < y_length; i++) {
            if (abs(c[i] - p[i]) > 50) {
                changed_pixels++;
            }
        }
        */

        //logInfo("YUVlength: %d", buffer->length);

        for (int i = 0; i < motionDetection.regions_length; i += 2) {
            int start = motionDetection.regions[i];
            int end = motionDetection.regions[i + 1];

            for (int j = start; j < end; j++) {
                if (abs(c[j] - p[j]) > 50) {
                    changed_pixels++;
                }
            }
        }



        pthread_mutex_lock(&motionDetectionMutex);
        if (changed_pixels > settings->changed_pixels_threshold) {
            motionDetection.motion_count++;
            // if motion is detected, check again in 2 seconds
            motionDetection.detection_at = frame_counter + (settings->h264.fps * 2);
            // only copy if motion detected ....
            // this lets us detect slow moving items
            memcpy(motionDetection.previousFrame, buffer->data, settings->mjpeg.y_length);
        } else {
            motionDetection.motion_count = 0;
            motionDetection.detection_at = frame_counter + motionDetection.detection_sleep;
        }
        pthread_mutex_unlock(&motionDetectionMutex);

        mmal_buffer_header_mem_unlock(buffer);

        clock_t end = clock();
        //printf("TIME. Motion Detection: %f\n", (double)(end - begin) / CLOCKS_PER_SEC);

        printf("DETECTION. Regions: %d Changed pixels: %d threshold: %d Time: %f\n", motionDetection.regions_length, changed_pixels, settings->changed_pixels_threshold, (double)(end - begin) / CLOCKS_PER_SEC);
    }

    mmal_buffer_header_release(buffer);

    // TODO: refactor this block
    if (port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T* new_buffer;
        while ( (new_buffer = mmal_queue_get(userdata->handles->yuvPool->queue)) ) {
            status = mmal_port_send_buffer(port, new_buffer);
            if (status != MMAL_SUCCESS) {
                //logError("mmal_port_send_buffer failed, no buffer to return to yuv port\n", __func__);
                break;
            }
        }
    }
}
// END YUV FUNCTIONS


// HTTP SERVER STUFF
static int http_server_cleanup_arg = 0;
static void http_server_thread_cleanup(void *arg) {
    pthread_mutex_unlock(&running_mutex);

    logInfo("HTTP Server thread closing down");
    for (int i = 0; i < http_fds_count; i++) {
        printf("Closing %d\n", http_fds[i].fd);
        close(http_fds[i].fd);
    }


    pthread_mutex_lock(&stream_connections_mutex);
    clear_connections(stream_connections);
    pthread_mutex_unlock(&stream_connections_mutex);
}
static void *httpServer(void *v) {
    SETTINGS* settings = (SETTINGS*)v;
    bool r;
    int status;
    int ret;

    int port = 8080;
    int listener, socketfd;
    int length;
    static struct sockaddr_in cli_addr; /* static = initialised to zeros */
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */
    char request[8192];
    int request_length = 8192;
    char* response_header = "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\nCache-control: no-store\r\nContent-Type: multipart/x-mixed-replace; boundary=HATCHA\r\n\r\n";
    int response_header_length = strlen(response_header);
    int response2_max = 4096;
    char response1[1024], response2[4097]; // for index.html
    int response1_length, response2_length;
    int i;
    int yes = 1;

    pthread_cleanup_push(http_server_thread_cleanup, NULL);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    //logInfo("Opening socket");
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fprintf(stdout, "Failed to create server socket: %d\n", listener);
        //logError("Failed to create listening socket", __func__);
        pthread_exit(NULL);
    }

    // REUSEADDR so socket is available again immediately after close()
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    logInfo("Binding");
    status = bind(listener, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (status < 0) {
        fprintf(stdout, "Failed to bind: %d\n", errno);
        logError("Failed to bind", __func__);
        close(listener);
        pthread_exit(NULL);
    }

    logInfo("Listening");
    status = listen(listener, SOMAXCONN);
    if (status < 0) {
        fprintf(stdout, "Failed to listen: %d\n", status);
        close(listener);
        pthread_exit(NULL);
    }
    logInfo("here");
    length = sizeof(cli_addr);

    // add listener to poll fds
    http_fds[0].fd = listener;
    http_fds[0].events = POLLIN;
    http_fds_count++;

    logInfo("here 1");
    pthread_mutex_lock(&running_mutex);
    r = running;
    pthread_mutex_unlock(&running_mutex);
    while (r) {
        // this blocks, which is what we want
        ret = poll(http_fds, http_fds_count, -1);
        if (ret < 1) {
            // thread cancelled or other?
            // TODO: handle this
            printf("poll returned <1 %d\n", ret);
        }

        for (int i = 0; i < http_fds_count; ) {
            if (!(http_fds[i].revents & POLLIN)) {
                i++;
                continue;
            }

            if (i > 0) {
                // TODO: we may not receive all the data at once, hmmm
                int bytes = recv(http_fds[i].fd, request, request_length, 0);
                if (bytes > 0) {
                    request[bytes] = 0;
                    // parse and handle GET request
                    fprintf(stdout, "[INFO] Got request: %s\n", request);

                    // TODO: can we cleanup this in some way?
                    
                    if (strncmp(request, "GET / HTTP", 10) == 0) {
                        logInfo("here3\n");
                        // write index.html
                        FILE* f = fopen("public/index.html", "r");
                        response2_length = fread(response2, 1, response2_max, f);
                        fclose(f);

                        // note the Connection:close header to prevent keep-alive
                        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);
                        ret = sendSocket(http_fds[i].fd, response1, response1_length);
                        ret += sendSocket(http_fds[i].fd, response2, response2_length);

                    } else if (strncmp(request, "GET /stream.mjpeg?ts=", 21) == 0) {
                        ret = sendSocket(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&stream_connections_mutex);
                        LL_APPEND(stream_connections, create_connection( http_fds[i].fd ));
                        pthread_mutex_unlock(&stream_connections_mutex);

                    } else if (strncmp(request, "GET /motion.mjpeg HTTP", 22) == 0) {
                        // send motion pixels as mjpeg
                        ret = sendSocket(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&stream_connections_mutex);
                        LL_APPEND(motion_connections, create_connection( http_fds[i].fd ));
                        pthread_mutex_unlock(&stream_connections_mutex);
                    
                    } else if (strncmp(request, "GET /status.json HTTP", 21) == 0) {
                        response2_length = snprintf(response2, response2_max, "{\"width\": \"%d\", \"height\":\"%d\"}", settings->width, settings->height);

                        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);

                        ret = sendSocket(http_fds[i].fd, response1, response1_length);
                        ret = sendSocket(http_fds[i].fd, response2, response2_length);


                    } else {
                        // request for something we don't handle
                        response1_length = snprintf(response1, 1024, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found");
                        ret = sendSocket(http_fds[i].fd, response1, response1_length);
                    }
                    
                    // Send prelim headers
                    if (ret < 1) {
                        logInfo("Failed to write");
                        close(http_fds[i].fd);
                    }

                    i++;
                } else {
                    connection* el;
                    // Receive returned 0. Socket has been closed.

                    pthread_mutex_lock(&stream_connections_mutex);
                    if ((el = find_connection(stream_connections, http_fds[i].fd))) {
                        // remove from mjpeg streaming list
                        LL_DELETE(stream_connections, el);
                    }
                    pthread_mutex_unlock(&stream_connections_mutex);

                    pthread_mutex_lock(&motion_connections_mutex);
                    if ((el = find_connection(motion_connections, http_fds[i].fd))) {
                        // remove from mjpeg streaming list
                        LL_DELETE(motion_connections, el);
                    }
                    pthread_mutex_unlock(&motion_connections_mutex);

                    printf("Closing %d\n", http_fds[i].fd);
                    close(http_fds[i].fd);

                    http_fds[i] = http_fds[http_fds_count - 1];
                    http_fds_count--;
                    // process the same slot again, which has a new file descriptor
                    // don't increment i
                }
            } else {
                socketfd = accept(listener, NULL, 0); //, (struct sockaddr *)&cli_addr, &length);
                if (http_fds_count < MAX_POLL_FDS) {
                    if (socketfd < 0) {
                        fprintf(stdout, "Failed to accept: %d\n", errno);
                        pthread_mutex_lock(&running_mutex);
                        r = running;
                        pthread_mutex_unlock(&running_mutex);
                        // TODO: think we should break if accept fails
                        // and perhaps re-listen?
                        break;
                    }
                    printf("Accepted: %d\n", socketfd);

                    http_fds[http_fds_count].fd = socketfd;
                    http_fds[http_fds_count].events = POLLIN;
                    http_fds_count++;
                } else {
                    printf("CANNOT ACCEPT CONNECTION, CLOSING\n");
                    close(socketfd);
                }

                // move on to next item in fds
                i++;
            }
        }

        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }
    logInfo("HTTP Server thread closing down");
    pthread_cleanup_pop(http_server_cleanup_arg);
    return NULL;
}


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
    char metric[200];
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
        fprintf(stdout, "[ERR] Failed to getaddrinfo for heartbeat server: %s\n", gai_strerror(rv));
        return;
    }

    if ((sockfd = socket(heartbeatAddr->ai_family, heartbeatAddr->ai_socktype, heartbeatAddr->ai_protocol)) == -1) {
        logError("Failed to create UDP socket. Bailing on heartbeat.", __func__);
        return;
    }

    // Temperature metrics server info
    if ((rv = getaddrinfo("192.168.1.173", "8125", &hints, &temperatureAddr)) != 0) {
        fprintf(stdout, "[ERR] Failed to getaddrinfo for temperature metrics server: %s\n", gai_strerror(rv));
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
        logInfo("STATS. FPS: %d", stats.fps);
        stats.fps = 0;
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


        // send heartbeat once per second
        if ((numbytes = sendto(sockfd, "hi", 2, 0, heartbeatAddr->ai_addr, heartbeatAddr->ai_addrlen)) == -1) {
            fprintf(stderr, "Failed to send udp heartbeat. errno: %d\n", errno);
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
            numbytes = snprintf(metric, 200, "raspi.pimera.seconds,host=%s:%ld|g", hostname, uptime.tv_sec - start);
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

    MMAL_PORT_T *cameraVideoPort = NULL;
    MMAL_PORT_T *splitterInputPort = NULL;
    MMAL_PORT_T *splitterH264Port = NULL;
    MMAL_PORT_T *splitterMjpegPort = NULL;
    MMAL_PORT_T *splitterYuvPort = NULL;
    MMAL_PORT_T *h264EncoderInputPort = NULL;
    MMAL_PORT_T *h264EncoderOutputPort = NULL;
    MMAL_PORT_T *mjpegEncoderInputPort = NULL;
    MMAL_PORT_T *mjpegEncoderOutputPort = NULL;

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

    pthread_mutex_init(&mjpeg_concurrent_mutex, NULL);

    handles.camera = NULL;
    handles.splitter = NULL;
    handles.h264_encoder = NULL;
    handles.mjpeg_encoder = NULL;
    handles.splitterConnection = NULL;
    handles.h264EncoderConnection = NULL;
    handles.mjpegEncoderConnection = NULL;
    handles.h264EncoderPool = NULL;
    handles.mjpegEncoderPool = NULL;
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
        fprintf(stderr, "FAILED TO ALLOCATE H264 BUFFER OF SIZE %u\n", h264_buffer_size);
    }
    


    bcm_host_init();


    signal(SIGINT, signalHandler);

    //pid_t tid = syscall(SYS_gettid);
    //fprintf(stdout, "Main thread id %d\n", tid);


    if ((status = createCamera(&handles, &settings)) != MMAL_SUCCESS) {
        logError("createCamera failed", __func__);
        return EX_ERROR;

    } else if ((status = createSplitter(&handles, &settings)) != MMAL_SUCCESS) {
        logError("createSplitter failed", __func__);
        destroyCamera(&handles, &settings);
        return EX_ERROR;

    } else if ((status = createH264Encoder(&handles, &settings)) != MMAL_SUCCESS) {
        logError("createH264Encoder failed",  __func__);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;

    } else if ((status = createMjpegEncoder(&handles, &settings)) != MMAL_SUCCESS) {
        logError("createMjpegEncoder failed", __func__);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    
    // Major components have been created, let's hook them together
    
    if (settings.verbose) {
        logInfo("Connecting things together now");
    }

    cameraVideoPort = handles.camera->output[CAMERA_VIDEO_PORT];

    splitterInputPort = handles.splitter->input[0];
    splitterH264Port = handles.splitter->output[SPLITTER_H264_PORT];
    splitterMjpegPort = handles.splitter->output[SPLITTER_MJPEG_PORT];
    splitterYuvPort = handles.splitter->output[SPLITTER_YUV_PORT];
    handles.splitterYuvPort = splitterYuvPort;

    h264EncoderInputPort  = handles.h264_encoder->input[0];
    h264EncoderOutputPort = handles.h264_encoder->output[0];
    handles.h264EncoderOutputPort = h264EncoderOutputPort;

    mjpegEncoderInputPort  = handles.mjpeg_encoder->input[0];
    mjpegEncoderOutputPort = handles.mjpeg_encoder->output[0];

    if (settings.verbose)
        logInfo("Connecting camera video port to splitter input port");

    status = connectEnable(&handles.splitterConnection, cameraVideoPort, splitterInputPort);
    if (status != MMAL_SUCCESS) {
        handles.splitterConnection = NULL;
        logError("connectEnable failed for camera video port to splitter input", __func__);
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    // H264 ENCODER SETUP
    if (settings.verbose) {
        logInfo("Connecting splitter h264 port to h264 encoder input port");
    }

    status = connectEnable(&handles.h264EncoderConnection, splitterH264Port, h264EncoderInputPort);
    if (status != MMAL_SUCCESS) {
        logError("connectEnable failed for splitter h264 port to h264 encoder input", __func__);
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }


    h264EncoderOutputPort->userdata = (struct MMAL_PORT_USERDATA_T *)&h264CallbackUserdata;

    if (settings.verbose) {
        logInfo("Enabling mjpeg encoder output port");
    }

    // Enable the h264 encoder output port and tell it its callback function
    status = mmal_port_enable(h264EncoderOutputPort, h264Callback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for h264 encoder output", __func__);
        // disconnect and disable first?
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    // make buffer headers available to encoder output
    if (settings.verbose) {
        logInfo("Providing buffers to h264 encoder output");
    }
    num = mmal_queue_length(handles.h264EncoderPool->queue);
    for (i = 0; i < num; i++) {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(handles.h264EncoderPool->queue);

        if (!buffer) {
            logError("mmal_queue_get failed for h264 encoder pool", __func__);
            break;
        } else {
            status = mmal_port_send_buffer(h264EncoderOutputPort, buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed for h264 encoder output port", __func__);
                break;
            }
        }
    }



    // MJPEG ENCODER SETUP

    if (settings.verbose) {
        logInfo("Connecting splitter mjpeg port to mjpeg encoder input port");
    }

    status = connectEnable(&handles.mjpegEncoderConnection, splitterMjpegPort, mjpegEncoderInputPort);
    if (status != MMAL_SUCCESS) {
        logError("connectEnable failed for splitter mjpeg port to mjpeg encoder input", __func__);
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    mjpegEncoderOutputPort->userdata = (struct MMAL_PORT_USERDATA_T *)&mjpegCallbackUserdata;

    if (settings.verbose) {
        logInfo("Enabling mjpeg encoder output port");
    }

    // Enable the splitter output port and tell it its callback function
    status = mmal_port_enable(mjpegEncoderOutputPort, mjpegCallback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for mjpeg encoder output", __func__);
        // disconnect and disable first?
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    // make buffer headers available to encoder output
    // TODO: i don't understand this
    if (settings.verbose) {
        logInfo("Providing buffers to mjpeg encoder output");
    }
    num = mmal_queue_length(handles.mjpegEncoderPool->queue);
    for (i = 0; i < num; i++) {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(handles.mjpegEncoderPool->queue);

        if (!buffer) {
            logError("mmal_queue_get failed for mjpeg encoder pool", __func__);
        } else {
            status = mmal_port_send_buffer(mjpegEncoderOutputPort, buffer);
            if (status != MMAL_SUCCESS) {
                logError("mmal_port_send_buffer failed for mjpeg encoder output port", __func__);
                break;
            }
        }
    }



    // YUV MOTION STUFF
    splitterYuvPort->userdata = (struct MMAL_PORT_USERDATA_T *)&yuvCallbackUserdata;

    if (settings.verbose) {
        logInfo("Enabling splitter yuv output port");
    }

    // Enable the splitter output port and tell it its callback function
    status = mmal_port_enable(splitterYuvPort, yuvCallback);
    if (status != MMAL_SUCCESS) {
        logError("mmal_port_enable failed for yuv output", __func__);
        // disconnect and disable first?
        destroyMjpegEncoder(&handles, &settings);
        destroyH264Encoder(&handles, &settings);
        destroySplitter(&handles, &settings);
        destroyCamera(&handles, &settings);
        return EX_ERROR;
    }

    if (settings.verbose) {
        logInfo("Providing buffers to yuv output");
    }
    buffer = mmal_queue_get(handles.yuvPool->queue);
    while (buffer) {
        status = mmal_port_send_buffer(splitterYuvPort, buffer);
        if (status != MMAL_SUCCESS) {
            break;
        }
        buffer = mmal_queue_get(handles.yuvPool->queue);
    }


    // TODO: does this really enable capture? necessary?
    status = mmal_port_parameter_set_boolean(cameraVideoPort, MMAL_PARAMETER_CAPTURE, 1);
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


    
    // Disable all our ports that are not handled by connections
    logInfo("hi 4");
    //disablePort(&settings, cameraVideoPort, "camera");
    //disablePort(&settings, splitterH264Port, "splitter h264");
    //disablePort(&settings, splitterMjpegPort, "splitter mjpeg");
    disablePort(&settings, splitterYuvPort, "splitter yuv");
    // disabling this causes a segfault ...
    disablePort(&settings, h264EncoderOutputPort, "h264 encoder output");
    disablePort(&settings, mjpegEncoderOutputPort, "mjpeg encoder output");

    logInfo("hi 5");
    destroyConnection(&settings, handles.h264EncoderConnection, "h264 encoder");
    destroyConnection(&settings, handles.mjpegEncoderConnection, "mjpeg encoder");
    destroyConnection(&settings, handles.splitterConnection, "splitter");

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
    // destroy yuv buffer pool
    if (handles.yuvPool) {
        if (settings.verbose) {
            logInfo("Destroying yuv pool");
        }
        mmal_port_pool_destroy(splitterYuvPort, handles.yuvPool);
        handles.yuvPool = NULL;
    }
    destroySplitter(&handles, &settings);
    destroyCamera(&handles, &settings);


    // clean up the processing queues
    logInfo("Cleaning up processing queues");

    freeDetection();
    //free(h264_buffer);


    if (settings.verbose) {
        logInfo("Shutdown complete");
    }


}
