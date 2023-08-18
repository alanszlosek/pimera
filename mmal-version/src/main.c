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

#include "camera.h"
#include "detection.h"
#include "handles.h"
#include "http.h"
#include "log.h"
#include "mjpeg.h"
#include "shared.h"
#include "settings.h"
#include "video.h"


#define DEBUG 1

pthread_mutex_t running_mutex;
bool running = 1;
pthread_mutex_t restart_mutex;
int restart = 0;




//MMAL_BUFFER_HEADER_T* h264_buffers[ 60 ];


typedef struct {
    uint8_t fps;
} STATS_T;
STATS_T stats;
pthread_mutex_t statsMutex;


// https://stackoverflow.com/questions/1675351/typedef-struct-vs-struct-definitions/
typedef struct {
    HANDLES *handles;
    SETTINGS *settings;
    char h264FileKickoff[128];
    uint32_t h264FileKickoffLength;
    int abort;
} CALLBACK_USERDATA;


void reconfigureRegion(SETTINGS* settings) {
    unsigned int x_start, x_end, y_start, y_end;
    unsigned int stride = settings->mjpeg.vcosWidth;
    unsigned int rows;
    unsigned int row_length;
    x_start = settings->region[0];
    y_start = settings->region[1];
    x_end = settings->region[2];
    y_end = settings->region[3];
    rows = y_end - y_start;
    row_length = x_end - x_start;

    printf("New detection region: %d, %d, %d, %d\n", x_start, y_start, x_end, y_end);

    if (motionDetection.processing.pointers) {
        free(motionDetection.processing.pointers);
        motionDetection.processing.pointers = NULL;
    }

    motionDetection.processing.pointers = (uint8_t**) malloc(
        // start c, end c, start p
        (sizeof(uint8_t*) * rows * 3)
        +
        // NULL separators between each batch of pointers
        (sizeof(uint8_t*) * motionDetection.processing.batches * 3)
        +
        // NULL terminators
        (sizeof(uint8_t*) * 9)
    );

    unsigned int rows_per_batch = rows / motionDetection.processing.batches;
    unsigned int offset = (y_start * stride) + x_start;
    unsigned int i = 0;
    unsigned int j;

    for (
        unsigned int y = y_start,
        row = 0;
        
        y < y_end;
        
        y++,
        row++
    ) {
        unsigned int row_offset = offset + (stride * row);
        j = i;
        // pointer to start of row in current
        motionDetection.processing.pointers[i++] = 
            motionDetection.currentFrame + row_offset;

        // pointer to end of that row in current
        motionDetection.processing.pointers[i++] = motionDetection.processing.pointers[j] + row_length;

        // pointer to start of corresponding row in previous
        motionDetection.processing.pointers[i++] = 
            motionDetection.previousFrame + row_offset;
        // TODO: verify this is right
        if ((row+1) % rows_per_batch == 0) {
            motionDetection.processing.pointers[i++] = NULL;
            motionDetection.processing.pointers[i++] = NULL;
            motionDetection.processing.pointers[i++] = NULL;
        }
    }
    motionDetection.processing.pointers[i++] = NULL;
    motionDetection.processing.pointers[i++] = NULL;
    motionDetection.processing.pointers[i++] = NULL;

    // OLD
    /*
    motionDetection.region.offset = (y_start * stride) + x_start;
    motionDetection.region.num_rows = y_end - y_start;
    // Split detection into batches so we finish just before the next detection frame
    motionDetection.region.batches = motionDetection.detection_sleep - 1;
    motionDetection.region.row_batch_size = motionDetection.region.num_rows / motionDetection.region.batches;
    motionDetection.region.row_length = x_end - x_start;
    motionDetection.region.stride = stride;
    */
}
void initDetection(SETTINGS* settings) {
    motionDetection.detection_sleep = settings->h264.fps / settings->motion_check_frequency;

    motionDetection.processing.batches = motionDetection.detection_sleep;

    motionDetection.stream_sleep = settings->h264.fps / 3;

    motionDetection.previousFrame = (uint8_t*) malloc( settings->mjpeg.y_length );
    motionDetection.currentFrame = (uint8_t*) malloc( settings->mjpeg.y_length );

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

    motionDetection.processing.pointers = NULL;
    reconfigureRegion(settings);
}

void freeDetection() {
    free(motionDetection.previousFrame);
    free(motionDetection.currentFrame);
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
    settings->threshold = 5000;
    detection_threshold(settings->threshold);

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
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "threshold", 9);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->threshold = atoi(value);
        detection_threshold(settings->threshold);
    }

    fprintf(stdout, "[INFO] SETTINGS. h264 %dx%d @ %d fps. mjpeg %dx%d. motion_check_frequency: %d\n", settings->h264.width, settings->h264.height, settings->h264.fps, settings->mjpeg.width, settings->mjpeg.height, settings->motion_check_frequency);


    ini_destroy(ini);
    return;
}


void signalHandler(int signal_number) {
    logError("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = false;
    pthread_mutex_unlock(&running_mutex);
    logInfo("Signaled");
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
            detection_threshold(settings->threshold);

            sleep(1);
            status = mmal_port_parameter_set_boolean(handles->camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
            if (status != MMAL_SUCCESS) {
                logError("Failed to restart camera", __func__);
            }
            restart = false;
        }
        pthread_mutex_unlock(&restart_mutex);

        /*
        pthread_mutex_lock(&statsMutex);
        fps = stats.fps;
        stats.fps = 0;
        pthread_mutex_unlock(&statsMutex);
        logInfo("STATS. FPS: %d", fps);
        */

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


    pthread_mutex_init(&httpConnectionsMutex, NULL);
    pthread_mutex_init(&running_mutex, NULL);
    pthread_mutex_init(&motionDetectionMutex, NULL);
    pthread_mutex_init(&statsMutex, NULL);
    pthread_mutex_init(&stream_connections_mutex, NULL);
    pthread_mutex_init(&motion_connections_mutex, NULL);
    pthread_mutex_init(&still_connections_mutex, NULL);
    pthread_mutex_init(&restart_mutex, NULL);

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

    setDefaultSettings(&settings);
    readSettings(&settings);

    initDetection(&settings);


    bcm_host_init();

    signal(SIGINT, signalHandler);

    if ((status = create_camera(&handles.camera, &settings)) != MMAL_SUCCESS) {
        logError("create_camera failed", __func__);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.full_splitter, &handles.full_splitter_connection, handles.camera->output[CAMERA_VIDEO_PORT], 2)) != MMAL_SUCCESS) {
        logError("create_splitter failed", __func__);
        destroy_camera(handles.camera);
        return EX_ERROR;

    } else if ((status = create_h264_encoder(&handles.h264_encoder, &handles.h264_encoder_pool, &handles.h264_encoder_connection, handles.full_splitter->output[0], h264_callback, &settings.h264)) != MMAL_SUCCESS) {
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


    } else if ((status = create_mjpeg_encoder(&handles.mjpeg_encoder, &handles.mjpeg_encoder_pool, &handles.mjpeg_encoder_connection, handles.resized_splitter->output[0], mjpeg_callback, &settings.mjpeg)) != MMAL_SUCCESS) {
        logError("createMjpegEncoder failed", __func__);
        destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    }
    mjpeg_config(handles.mjpeg_encoder_pool->queue);

    // last mile h264 config
    char videoFilePattern[256];
    snprintf(videoFilePattern, 255, "%s/%%s_%s_%dx%dx%d.", settings.videoPath, settings.hostname, settings.width, settings.height, settings.h264.fps);
    h264_config(
        handles.h264_encoder_pool->queue,
        videoFilePattern,
        // h264_buffer_size
        (settings.width * settings.height)
        *
        (settings.h264.fps * 2)
    );

    status = mmal_port_enable(handles.resized_splitter->output[1], yuv_callback);

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

    // YUV STUFF LAST MILE
    detection_config(
        settings.h264.fps,
        settings.mjpeg.y_length,
        handles.yuvPool->queue
    );
    detection_threshold(settings.threshold);


    handles.h264_encoder->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)&h264CallbackUserdata;
    logInfo("Sending buffers to h264 port");
    send_buffers_to_port(handles.h264_encoder->output[0], handles.h264_encoder_pool->queue);

    handles.mjpeg_encoder->output[0]->userdata = NULL;
    logInfo("Sending buffers to mjpeg port");
    send_buffers_to_port(handles.mjpeg_encoder->output[0], handles.mjpeg_encoder_pool->queue);

    handles.resized_splitter->output[1]->userdata = NULL;
    logInfo("Sending buffers to YUV port");
    send_buffers_to_port(handles.resized_splitter->output[1], handles.yuvPool->queue);


    // NOW TURN ON THE CAMERA
    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
    if (status != MMAL_SUCCESS) {
        logError("Toggling MMAL_PARAMETER_CAPTURE to 1 failed", __func__);
        // TODO: unwind
    }

    // set up http server thread
    pthread_create(&httpServerThreadId, NULL, httpServer, &settings);

    // heartbeat loop
    heartbeat(&settings, &handles);

    logInfo("Waiting for threads");
    // This bails out of the thread, so we can exit
    pthread_cancel(httpServerThreadId);
    // But we want to gracefully exit the mjpeg thread, instead of bail
    pthread_join(httpServerThreadId, &httpServerThreadStatus);

    if (settings.verbose) {
        logInfo("Shutting down");
    }

    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 0);
    if (status != MMAL_SUCCESS) {
        logError("Toggling MMAL_PARAMETER_CAPTURE to 0 failed", __func__);
        // TODO: unwind
    }

    // Disable all our ports that are not handled by connections
    disable_port(handles.resized_splitter->output[1], "splitter yuv");
    disable_port(handles.h264_encoder->output[0], "h264 encoder output");
    disable_port(handles.mjpeg_encoder->output[0], "mjpeg encoder output");

    destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
    destroy_resizer(handles.resizer, handles.resizer_connection);
    destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
    destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
    destroy_camera(handles.camera);

    // clean up the processing queues
    logInfo("Cleaning up processing queues");

    freeDetection();

    if (settings.verbose) {
        logInfo("Shutdown complete");
    }

}
