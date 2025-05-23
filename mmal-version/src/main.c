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
pthread_mutex_t stats_mutex;


void region_reconfigure(SETTINGS* settings) {
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

    if (motion_detection.processing.pointers) {
        free(motion_detection.processing.pointers);
        motion_detection.processing.pointers = NULL;
    }

    motion_detection.processing.pointers = (uint8_t**) malloc(
        // start c, end c, start p
        (sizeof(uint8_t*) * rows * 3)
        +
        // NULL separators between each batch of pointers
        (sizeof(uint8_t*) * motion_detection.processing.batches * 3)
        +
        // NULL terminators
        (sizeof(uint8_t*) * 9)
    );

    unsigned int rows_per_batch = rows / motion_detection.processing.batches;
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
        motion_detection.processing.pointers[i++] = 
            motion_detection.current_frame + row_offset;

        // pointer to end of that row in current
        motion_detection.processing.pointers[i++] = motion_detection.processing.pointers[j] + row_length;

        // pointer to start of corresponding row in previous
        motion_detection.processing.pointers[i++] = 
            motion_detection.previous_frame + row_offset;
        // TODO: verify this is right
        if ((row+1) % rows_per_batch == 0) {
            motion_detection.processing.pointers[i++] = NULL;
            motion_detection.processing.pointers[i++] = NULL;
            motion_detection.processing.pointers[i++] = NULL;
        }
    }
    // One final row to signal end of pointer data
    motion_detection.processing.pointers[i++] = NULL;
    motion_detection.processing.pointers[i++] = NULL;
    motion_detection.processing.pointers[i++] = NULL;

    // OLD
    /*
    motion_detection.region.offset = (y_start * stride) + x_start;
    motion_detection.region.num_rows = y_end - y_start;
    // Split detection into batches so we finish just before the next detection frame
    motion_detection.region.batches = motion_detection.detection_sleep - 1;
    motion_detection.region.row_batch_size = motion_detection.region.num_rows / motion_detection.region.batches;
    motion_detection.region.row_length = x_end - x_start;
    motion_detection.region.stride = stride;
    */
}
void detection_init(SETTINGS* settings) {
    // Frames have pts timestamp in microseconds, so calculate sleep based on pts
    motion_detection.detection_sleep = 1000000 / settings->motion_check_frequency;

    motion_detection.processing.batches = settings->h264.fps / settings->motion_check_frequency;

    motion_detection.stream_sleep = settings->h264.fps / settings->mjpeg.fps;

    motion_detection.previous_frame = (uint8_t*) malloc( settings->mjpeg.y_length );
    motion_detection.current_frame = (uint8_t*) malloc( settings->mjpeg.y_length );

    motion_detection.motion_count = 0;

    strncpy(motion_detection.boundary, "--HATCHA\r\nContent-Type: image/jpeg\r\nContent-length: ", 80);
    motion_detection.boundary_length = strlen(motion_detection.boundary);

    /*
    int start_x = 100;
    int start_y = 100;
    int width = 100;
    int height = 100;
    int end_y = start_y + height;
    int end_x = start_x + width;
    */

    motion_detection.processing.pointers = NULL;
    region_reconfigure(settings);
}

void detection_free() {
    free(motion_detection.previous_frame);
    free(motion_detection.current_frame);
}

void set_default_settings(SETTINGS* settings) {
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
    settings->mjpeg.fps = 4;

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
        log_error("Failed to gethostname", __func__);
        snprintf(settings->hostname, sizeof(settings->hostname), "INVALID");
    }

    // video path
    snprintf(settings->videoPath, 100, "");

    // TODO: handle this in read_settings()
    /*
    settings->mjpeg.width = 640;
    settings->mjpeg.height = 480;
    */

    settings->heartbeat_host[0] = 0;
    settings->heartbeat_port[0] = 0;
    settings->metrics_host[0] = 0;
    settings->metrics_port[0] = 0;
    
    c = getenv("DEBUG");
    if (c) {
        log_info("Enabling debug");
        settings->debug = true;
    }

}

void read_settings(SETTINGS* settings) {
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
        log_error("Failed to allocate data to read settings.ini. Bailing\n", __func__);
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
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "stream_fps", 10);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->mjpeg.fps = atoi(value);
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "video_path", 10);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->videoPath, value, sizeof(settings->videoPath));
    }
    // TODO: rename this with units
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "motion_check_frequency", 22);
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
        log_info("Region: %d %d %d %d", settings->region[0], settings->region[1], settings->region[2], settings->region[3]);
    }
    // motion detection threshold
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "threshold", 9);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        settings->threshold = atoi(value);
        detection_threshold(settings->threshold);
    }

    // heartbeat and metrics hosts
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "heartbeat_host", 14);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->heartbeat_host, value, SETTINGS_HOST_SIZE);
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "heartbeat_port", 14);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->heartbeat_port, value, sizeof(settings->heartbeat_port));
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "metrics_host", 12);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->metrics_host, value, SETTINGS_HOST_SIZE);
    }
    i = ini_find_property(ini, INI_GLOBAL_SECTION, "metrics_port", 12);
    if (i != INI_NOT_FOUND) {
        value = ini_property_value(ini, INI_GLOBAL_SECTION, i);
        strncpy(settings->metrics_port, value, sizeof(settings->metrics_port));
    }

    fprintf(stdout, "[INFO] SETTINGS. h264 %dx%d @ %d fps. mjpeg %dx%d. motion_check_frequency: %d\n", settings->h264.width, settings->h264.height, settings->h264.fps, settings->mjpeg.width, settings->mjpeg.height, settings->motion_check_frequency);
    fprintf(stdout, "[INFO] SETTINGS. heartbeat host %s\n", settings->heartbeat_host);


    ini_destroy(ini);
    return;
}


void signal_handler(int signal_number) {
    log_error("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = false;
    pthread_mutex_unlock(&running_mutex);
    log_info("Signaled");
}






// TODO: clean up these variable names, error messages, etc
void heartbeat(SETTINGS* settings, HANDLES* handles) {
    bool r;

    int sockfd;
    struct addrinfo hints, *heartbeatAddr, *metricsAddr;
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
        log_error("Failed to gethostname", __func__);
        snprintf(hostname, 100, "INVALID");
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // set to AF_INET to use IPv4
    hints.ai_socktype = SOCK_DGRAM;

    // Heartbeat address info
    // TODO: do we need to re-run these two blocks every time?
    // as the os to fill in addrinfo data for the host and port we want to send to
    if (strnlen(settings->heartbeat_host, SETTINGS_HOST_SIZE) && strnlen(settings->heartbeat_port, SETTINGS_PORT_SIZE)) {
        if ((rv = getaddrinfo(settings->heartbeat_host, settings->heartbeat_port, &hints, &heartbeatAddr)) != 0) {
            log_info("Failed to getaddrinfo for heartbeat server: %s", gai_strerror(rv));
            return;
        }

        if ((sockfd = socket(heartbeatAddr->ai_family, heartbeatAddr->ai_socktype, heartbeatAddr->ai_protocol)) == -1) {
            log_error("Failed to create UDP socket. Bailing on heartbeat.", __func__);
            return;
        }
    } else {
        heartbeatAddr = NULL;
    }

    // Temperature metrics server info
    if (strnlen(settings->metrics_host, SETTINGS_HOST_SIZE) && strnlen(settings->metrics_port, SETTINGS_PORT_SIZE)) {
        if ((rv = getaddrinfo(settings->metrics_host, settings->metrics_port, &hints, &metricsAddr)) != 0) {
            log_info("Failed to getaddrinfo for temperature metrics server: %s", gai_strerror(rv));
            return;
        }

        if ((sockfd = socket(metricsAddr->ai_family, metricsAddr->ai_socktype, metricsAddr->ai_protocol)) == -1) {
            log_error("Failed to create UDP socket for temperature metrics. Bailing on heartbeat.", __func__);
            return;
        }
    } else {
        metricsAddr = NULL;
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
    annotate.enable_text_background = MMAL_TRUE;
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
                log_error("Failed to stop camera", __func__);
            }

            // Re-copy settings values
            region_reconfigure(settings);
            detection_threshold(settings->threshold);

            sleep(1);
            status = mmal_port_parameter_set_boolean(handles->camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
            if (status != MMAL_SUCCESS) {
                log_error("Failed to restart camera", __func__);
            }
            restart = false;
        }
        pthread_mutex_unlock(&restart_mutex);

        /*
        pthread_mutex_lock(&stats_mutex);
        fps = stats.fps;
        stats.fps = 0;
        pthread_mutex_unlock(&stats_mutex);
        log_info("STATS. FPS: %d", fps);
        */

        // update time annotation when second has changed
        time(&rawtime);
        localtime_r(&rawtime, &timeinfo);
        strftime(annotate.text, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3, "%Y-%m-%d %H:%M:%S\n", &timeinfo);
        strncat(annotate.text, settings->hostname, MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - strlen(settings->hostname) - 1);
        annotate.text[MMAL_CAMERA_ANNOTATE_MAX_TEXT_LEN_V3 - 1] = '\0';
        status = mmal_port_parameter_set(handles->camera->control, &annotate.hdr);
        if (status != MMAL_SUCCESS) {
            log_error("Failed to set annotation", __func__);
        }


        // send heartbeat once per second
        if (heartbeatAddr) {
            if ((numbytes = sendto(sockfd, "hi", 2, 0, heartbeatAddr->ai_addr, heartbeatAddr->ai_addrlen)) == -1) {
                log_info("Failed to send udp heartbeat. errno: %d", errno);
            }
        }

        // read CPU/GPU temperature every 10 seconds
        // compatible with statsd conventions
        /*
        cpu=$(</sys/class/thermal/thermal_zone0/temp)
        echo "$((cpu/1000)) c"
        */
        if (tenIterations == 0) {
            // Send uptime metric
            if (metricsAddr) {
                clock_gettime(CLOCK_REALTIME, &uptime);
                numbytes = snprintf(
                    metric,
                    500,
                    "raspi.pimera.seconds,host=%s:%ld|g\nraspi.pimera.fps,host=%s:%d|g",
                    hostname, uptime.tv_sec - start,
                    hostname, fps);
                if ((numbytes = sendto(sockfd, metric, numbytes, 0, metricsAddr->ai_addr, metricsAddr->ai_addrlen)) == -1) {
                    log_error("Failed to send pimera uptime", __func__);
                }
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
    log_info("Free 1");
    freeaddrinfo(heartbeatAddr);
    log_info("Free 2");
    freeaddrinfo(metricsAddr);
    return;
}



int main(int argc, const char **argv) {
    SETTINGS settings;
    HANDLES handles;
    MMAL_STATUS_T status = MMAL_SUCCESS;

    // threading
    pthread_t http_server_thread_id;
    void *http_server_thread_status;
    int i, num;

    pthread_mutex_init(&http_connections_mutex, NULL);
    pthread_mutex_init(&running_mutex, NULL);
    pthread_mutex_init(&motion_detection_mutex, NULL);
    pthread_mutex_init(&stats_mutex, NULL);
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

    set_default_settings(&settings);
    read_settings(&settings);

    detection_init(&settings);


    bcm_host_init();

    signal(SIGINT, signal_handler);

    if ((status = create_camera(&handles.camera, &settings)) != MMAL_SUCCESS) {
        log_error("create_camera failed", __func__);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.full_splitter, &handles.full_splitter_connection, handles.camera->output[CAMERA_VIDEO_PORT], 2)) != MMAL_SUCCESS) {
        log_error("create_splitter failed", __func__);
        destroy_camera(handles.camera);
        return EX_ERROR;

    } else if ((status = create_h264_encoder(&handles.h264_encoder, &handles.h264_encoder_pool, &handles.h264_encoder_connection, handles.full_splitter->output[0], h264_callback, &settings.h264)) != MMAL_SUCCESS) {
        log_error("createH26create_h264_encoder4Encoder failed",  __func__);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    
    } else if ((status = create_resizer(&handles.resizer, &handles.resizer_connection, handles.full_splitter->output[1], &settings.mjpeg)) != MMAL_SUCCESS) {
        log_error("create_resizer failed", __func__);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;

    } else if ((status = create_splitter(&handles.resized_splitter, &handles.resized_splitter_connection, handles.resizer->output[0], 2)) != MMAL_SUCCESS) {
        log_error("create_splitter failed", __func__);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;


    } else if ((status = create_mjpeg_encoder(&handles.mjpeg_encoder, &handles.mjpeg_encoder_pool, &handles.mjpeg_encoder_connection, handles.resized_splitter->output[0], mjpeg_callback, &settings.mjpeg)) != MMAL_SUCCESS) {
        log_error("createMjpegEncoder failed", __func__);
        destroy_splitter(handles.resized_splitter, handles.resized_splitter_connection);
        destroy_resizer(handles.resizer, handles.resizer_connection);
        destroy_h264_encoder(handles.h264_encoder, handles.h264_encoder_connection, handles.h264_encoder_pool);
        destroy_splitter(handles.full_splitter, handles.full_splitter_connection);
        destroy_camera(handles.camera);
        return EX_ERROR;
    }
    mjpeg_config(handles.mjpeg_encoder_pool->queue);

    // last mile h264 config
    char video_file_pattern[256];
    snprintf(video_file_pattern, 255, "%s/%%s_%s_%dx%dx%d.", settings.videoPath, settings.hostname, settings.width, settings.height, settings.h264.fps);
    h264_config(
        settings.h264.fps,
        handles.h264_encoder_pool->queue,
        video_file_pattern,
        // h264_buffer_size
        (settings.width * settings.height)
        *
        (settings.h264.fps * 2)
    );

    status = mmal_port_enable(handles.resized_splitter->output[1], yuv_callback);

    if (status != MMAL_SUCCESS) {
        log_error("mmal_port_enable failed for mjpeg encoder output", __func__);
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
        log_error("mmal_port_pool_create failed for mjpeg encoder output", __func__);
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


    handles.h264_encoder->output[0]->userdata = NULL;
    log_info("Sending buffers to h264 port");
    send_buffers_to_port(handles.h264_encoder->output[0], handles.h264_encoder_pool->queue);

    handles.mjpeg_encoder->output[0]->userdata = NULL;
    log_info("Sending buffers to mjpeg port");
    send_buffers_to_port(handles.mjpeg_encoder->output[0], handles.mjpeg_encoder_pool->queue);

    handles.resized_splitter->output[1]->userdata = NULL;
    log_info("Sending buffers to YUV port");
    send_buffers_to_port(handles.resized_splitter->output[1], handles.yuvPool->queue);


    // NOW TURN ON THE CAMERA
    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 1);
    if (status != MMAL_SUCCESS) {
        log_error("Toggling MMAL_PARAMETER_CAPTURE to 1 failed", __func__);
        // TODO: unwind
    }

    // set up http server thread
    pthread_create(&http_server_thread_id, NULL, http_server, &settings);

    // heartbeat loop
    heartbeat(&settings, &handles);

    log_info("Waiting for threads");
    // This bails out of the thread, so we can exit
    pthread_cancel(http_server_thread_id);
    // But we want to gracefully exit the mjpeg thread, instead of bail
    pthread_join(http_server_thread_id, &http_server_thread_status);

    if (settings.verbose) {
        log_info("Shutting down");
    }

    status = mmal_port_parameter_set_boolean(handles.camera->output[CAMERA_VIDEO_PORT], MMAL_PARAMETER_CAPTURE, 0);
    if (status != MMAL_SUCCESS) {
        log_error("Toggling MMAL_PARAMETER_CAPTURE to 0 failed", __func__);
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
    log_info("Cleaning up processing queues");

    detection_free();

    if (settings.verbose) {
        log_info("Shutdown complete");
    }

}
