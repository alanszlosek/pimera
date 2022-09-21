#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

#include <arm_neon.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
//#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>
#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

//#include "encoder.hpp"
#include "annotate.hpp"

using namespace libcamera;

static std::shared_ptr<Camera> camera;

// TODO: convert this to vector or array[3] of uint8_t*
// we can pre-calculate length of each YUV data buffer in main
// and store in settings
std::map<FrameBuffer *, std::vector<uint8_t*>> mapped_buffers;
Stream* h264Stream;
Stream* mjpegStream;

pthread_mutex_t processingMutex;
pthread_cond_t processingCondition;
std::list<libcamera::Request*> processingQueue;

#define MAX_POLL_FDS 100
pthread_mutex_t streamConnectionsMutex;
std::list<int> streamConnections;
pthread_mutex_t motionConnectionsMutex;
std::list<int> motionConnections;
struct pollfd http_fds[MAX_POLL_FDS];
int http_fds_count = 0;


pthread_mutex_t runningMutex;
int running = 1;

// Miscellaneous
pthread_mutex_t requestsAtCameraMetricMutex;
pthread_mutex_t requestsAtQueueMetricMutex;
int requestsAtCameraMetric = 0;
int requestsAtQueueMetric = 0;


typedef struct PiMeraSettings {
    struct {
        unsigned int width;
        unsigned int height;
        unsigned int stride;
        unsigned int fps;
        size_t y_length;
        size_t uv_length;
    } h264;

    struct {
        unsigned int width;
        unsigned int height;
        unsigned int stride;
        unsigned int quality = 90;
        // TODO: pre-calculate these in main for YUV plane buffers
        size_t y_length;
        size_t uv_length;
    } mjpeg;
    // if this percentage of pixels change, motion is detected
    float percentage_for_motion = 0.01;
    uint8_t pixel_delta_threshold = 50;


} PiMeraSettings;

PiMeraSettings settings;


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
    pthread_mutex_lock(&runningMutex);
    running = 0;
    pthread_mutex_unlock(&runningMutex);
    logInfo("Signaled");
}


static void* processingThread(void* arg) {
    const PiMeraSettings *settings = (const PiMeraSettings*)arg;
    libcamera::Request* request;

    // BEGIN JPEG STUFF
    // set up jpeg stuff once
    int stride2 = settings->mjpeg.stride / 2;
    uint8_t *Y;
    uint8_t *U;
    uint8_t *V;
    uint8_t *Y_max;
    uint8_t *U_max;
    uint8_t *V_max;
    
    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];
    // for yuv data
    JSAMPARRAY rows_yuv[] = { y_rows, u_rows, v_rows };
    struct jpeg_compress_struct cinfo_yuv;
    struct jpeg_error_mgr jerr_yuv;
    uint8_t* jpeg_buffer_yuv = NULL;
    jpeg_mem_len_t jpeg_len_yuv = 0;
    // for grayscale data
    uint8_t* uv_data;
    // static row data for U and V since we want grayscale
    JSAMPROW grayscale_uv_rows[8];
    // use same data for u and v
    JSAMPARRAY grayscale_rows[] = { y_rows, grayscale_uv_rows, grayscale_uv_rows };
    struct jpeg_compress_struct cinfo_grayscale;
    struct jpeg_error_mgr jerr_grayscale;
    uint8_t* jpeg_buffer_grayscale = NULL;
    jpeg_mem_len_t jpeg_len_grayscale = 0;
    char filename[40];

    // mjpeg stuff
    const char *boundary = "--HATCHA\r\nContent-Type: image/jpeg\r\nContent-length: ";
    int boundaryLength = strlen(boundary);
    char contentLength[20];
    int contentLengthLength;

    // yuv
    // https://stackoverflow.com/questions/16390783/how-to-compress-yuyv-raw-data-to-jpeg-using-libjpeg
    cinfo_yuv.err = jpeg_std_error(&jerr_yuv);
    jpeg_create_compress(&cinfo_yuv);
    // using width here instead of stride to exclude the extra columns of pixels
    cinfo_yuv.image_width = settings->mjpeg.width;
    cinfo_yuv.image_height = settings->mjpeg.height;
    cinfo_yuv.input_components = 3;
    cinfo_yuv.in_color_space = JCS_YCbCr;
    cinfo_yuv.jpeg_color_space = cinfo_yuv.in_color_space;
    cinfo_yuv.restart_interval = 0;
    jpeg_set_defaults(&cinfo_yuv);
    cinfo_yuv.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo_yuv, settings->mjpeg.quality, TRUE);

    // grayscale
    cinfo_grayscale.err = jpeg_std_error(&jerr_grayscale);
    jpeg_create_compress(&cinfo_grayscale);
    // using width here instead of stride to exclude the extra columns of pixels
    cinfo_grayscale.image_width = settings->mjpeg.width;
    cinfo_grayscale.image_height = settings->mjpeg.height;
    //cinfo_grayscale.num_components = 1;
    cinfo_grayscale.input_components = 3;
    cinfo_grayscale.in_color_space = JCS_YCbCr;
    //cinfo_grayscale.jpeg_color_space = JCS_GRAYSCALE;
    cinfo_grayscale.restart_interval = 0;
    jpeg_set_defaults(&cinfo_grayscale);
    // since can't get grayscale color space to work, prepare array of uv data to simulate grayscale from Y plane data
    // prepare 8 rows of data, maybe?
    int uv_length = (settings->mjpeg.stride * 8) / 2;
    uv_data = (uint8_t*) malloc(uv_length);
    memset(uv_data, 128, uv_length);
    // prepare grayscale_uv_rows pointers
    uint8_t* U_row = uv_data;
    for (int i = 0; i < 8; i++, U_row += stride2) {
        grayscale_uv_rows[i] = U_row;
    }

    // even though working with Y plane, raw doesn't work
    // JSAMPARRAY with just Y data and write raw lines ends up generating an image with only half of the scanlines filled in

    // raw lets us do detection with compression in around 60ms, otherwise 250ms
    cinfo_grayscale.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo_grayscale, settings->mjpeg.quality, TRUE);
    

    // END JPEG STUFF


    unsigned int frame_counter = 0;

    // TODO: the threshold variable names need a rethink
    unsigned int mjpeg_sleep = settings->h264.fps / 10; // * 60; // once a second
    unsigned int mjpeg_at = mjpeg_sleep;
    unsigned int num_mjpeg_connections = 0;

    // BEGIN MOTION DETECTION STUFF
    uint8_t* previousFrame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);
    uint8_t* motionFrame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);
    // we'll push pixels with motion to 255, leave others at their regular values,
    // unsure whether this will help us visualize where motion took place
    uint8_t* highlightedMotionFrame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);

    unsigned int detection_sleep = settings->h264.fps / 3;
    unsigned int detection_at = detection_sleep;

    // number of pixels that must be changed to detect motion
    unsigned int changed_pixels_threshold = settings->mjpeg.y_length * settings->percentage_for_motion;
    // END MOTION DETECTION STUFF

    printf("Pixel threshold %d\n", changed_pixels_threshold);

    // Wait for first frame, and store it in previousFrame
    pthread_mutex_lock(&processingMutex);
    while (processingQueue.size() == 0) {
        pthread_cond_wait(&processingCondition, &processingMutex);
    }
    printf("Storing first frame\n");
    request = processingQueue.front();
    processingQueue.pop_front(); // TODO: why both?
    pthread_mutex_unlock(&processingMutex);
    // TODO: hoist up these declarations
    const std::map<const Stream*, FrameBuffer*> &buffers = request->buffers();
    FrameBuffer* fb = buffers.begin()->second;
    Y = mapped_buffers[ fb ][0];
    // TODO: only copy regions we care about, once that data is available
    for (int i = 0; i < (settings->mjpeg.stride * settings->mjpeg.height); i++) {
        previousFrame[i] = Y[i];
    }

    // for testing grayscale
    /*
    for (int y = 0, i = 0; y < settings->mjpeg.height; y++) {
        for (int x = 0; x < settings->mjpeg.width; x++, i++) {
            if (x > 255) {
                motionFrame[i] = 255;
            } else {
                motionFrame[i] = x;
            }
        }
    }
    */

    while (running) {
        pthread_mutex_lock(&processingMutex);
        while (processingQueue.size() == 0) {
            pthread_cond_wait(&processingCondition, &processingMutex);
        }
        request = processingQueue.front();
        processingQueue.pop_front(); // TODO: why both?
        pthread_mutex_unlock(&processingMutex);

        pthread_mutex_lock(&requestsAtQueueMetricMutex);
        requestsAtQueueMetric--;
        pthread_mutex_unlock(&requestsAtQueueMetricMutex);

        // TODO: make this work with graceful shutdowns and HUPs
        if (request->status() == Request::RequestCancelled) {
            // skip?
            printf("CANCEL\n");
            return NULL;
        }

        // Decisions
        /*
        - do we need to convert to jpeg and send to http?
        - do we need to convert to h264
        - do we need to do motion detection?
        */

        frame_counter++;

        // Get FrameBuffer for first stream
        //const std::map<const Stream*, FrameBuffer*> &buffers2 = request->findBuffer(mjpegStream);
        FrameBuffer* fb = request->findBuffer(mjpegStream);




        // TODO: change settings->mjpeg to mjpeg as appropriate below






        // do mjpeg compression?
        // only if we have connections
        // TODO: fix naming on these
        pthread_mutex_lock(&streamConnectionsMutex);
        num_mjpeg_connections = streamConnections.size();
        pthread_mutex_unlock(&streamConnectionsMutex);

        // TODO: how do we add timestamp into the frame?

        // is anyone trying to watch the stream?
        if (num_mjpeg_connections > 0) {
            if (frame_counter >= mjpeg_at) {
                // update threshold for when we should encode next mjpeg frame
                mjpeg_at = frame_counter + mjpeg_sleep;

                Y = mapped_buffers[ fb ][0];
                U = mapped_buffers[ fb ][1];
                V = mapped_buffers[ fb ][2];
                Y_max = (Y + settings->mjpeg.y_length) - settings->mjpeg.stride;
                U_max = (U + settings->mjpeg.uv_length) - stride2;
                V_max = (V + settings->mjpeg.uv_length) - stride2;

                char timestamp[40];
                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                strftime(timestamp, 39, "%Y-%m-%d %H:%M:%S", t);
                annotate(timestamp, strlen(timestamp), Y, 10 + (10 * settings->mjpeg.stride), settings->mjpeg.stride);

                auto start_time = std::chrono::high_resolution_clock::now();
                // use a fresh jpeg_buffer each iteration to avoid OOM:
                // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
                jpeg_mem_dest(&cinfo_yuv, &jpeg_buffer_yuv, &jpeg_len_yuv);
                // this takes 80-130ms, longer than frame at 10fps
                jpeg_start_compress(&cinfo_yuv, TRUE);

                for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; cinfo_yuv.next_scanline < settings->mjpeg.height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->mjpeg.stride) {
                        y_rows[i] = std::min(Y_row, Y_max);
                    }
                    for (int i = 0; i < 8; i++, U_row += stride2, V_row += stride2) {
                        u_rows[i] = std::min(U_row, U_max);
                        v_rows[i] = std::min(V_row, V_max);
                    }

                    //JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
                    jpeg_write_raw_data(&cinfo_yuv, rows_yuv, 16);
                    //jpeg_write_scanlines(&cinfo, rows, 16);
                }
                jpeg_finish_compress(&cinfo_yuv);
                //printf("new len %lu\n", jpeg_len_yuv);
                
                auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);
                printf("    Stream Encode time %ldms\n", ms_int.count());
                // TODO: would love to emit metrics to influx

                // Stream to browsers
                // prepare headers
                contentLengthLength = snprintf(contentLength, 20, "%lu\r\n\r\n", jpeg_len_yuv);
                pthread_mutex_lock(&streamConnectionsMutex);
                auto end = streamConnections.end();
                for (auto fd = streamConnections.begin(); fd != end; fd++) {
                    send(*fd, boundary, boundaryLength, 0);
                    send(*fd, contentLength, contentLengthLength, 0);
                    send(*fd, jpeg_buffer_yuv, jpeg_len_yuv, 0);
                }
                pthread_mutex_unlock(&streamConnectionsMutex);

                /*

                snprintf(filename, 40, "/home/pi/stills/%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer_yuv, jpeg_len_yuv, 1, fp);
                fclose(fp);
                */

                free(jpeg_buffer_yuv);
                jpeg_buffer_yuv = NULL;
            }
        }


        // DO H264 ENCODING?

        // SHOULD WE DO MOTION DETECTION FOR THIS FRAME?
        // let's try assuming the Y channel IS gray
        // TODO: speed this up with vector processing or SIMD
        if (frame_counter >= detection_at) {
            detection_at = frame_counter + detection_sleep;
            //printf("Checking frame for motion\n");

            uint changed_pixels = 0;
            uint compared_pixels = 0;

            auto start_time = std::chrono::high_resolution_clock::now();

            Y = mapped_buffers[ fb ][0];
            // NOTE: copying pixels in bulk ahead of time, instead of using an else below to copy unchanged pixels individually saves 20ms off
            // skipping motionFrame stuff altogether saves 2ms more
            //memcpy(motionFrame, Y, settings->y_length);


            // TODO: only compare regions we care about, once that data is available
            // TODO: this takes 500ms to 1080p, not fast enough
            // 320ms without motionFrame writes
            // 40ms for 640x480
            bool simd = true;
            if (!simd) {
                for (int i = 0; i < settings->mjpeg.y_length; i++) {
                    // compare previous and current
                    int delta = abs(previousFrame[i] - Y[i]);
                    compared_pixels++;
                    if (delta > settings->pixel_delta_threshold) {
                        // highlight pixels that have changed
                        //motionFrame[i] = 255;
                        changed_pixels++;
                    }
                }
            
            } else {
                int batch = 16;
                // constants
                uint8x16_t _threshold = vdupq_n_u8(settings->pixel_delta_threshold);
                uint8x16_t _one = vdupq_n_u8(1);

                uint8x16_t _a, _b, _c;

                uint8_t* current = Y;
                uint8_t* current_max = current + settings->mjpeg.y_length;
                uint8_t* previous = previousFrame;
                uint8_t* previous_max = previous + settings->mjpeg.y_length;
                uint8_t count = 0;

                for (; current < current_max; current += batch, previous += batch) {
                    _a = vld1q_u8(current);
                    _b = vld1q_u8(previous);

                    // subtraction then absolute value
                    _c = vabdq_u8(_b, _a);
                    // compare result against threshold
                    _a = vcgtq_u8(_c, _threshold);
                    // use bitmask from compare to set 1s in matching elements
                    _b = vandq_u8(_a, _one);
                    // increment counter - this overflows!
                    //_count = vaddq_u8(_count, _b);
                    // sum and get result out
                    changed_pixels += vaddvq_u8(_b);
                }
            }


            auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);

            if (changed_pixels > changed_pixels_threshold) {
                // motion was detected!
                memcpy(previousFrame, Y, settings->mjpeg.y_length);

                Y_max = (Y + settings->mjpeg.y_length) - settings->mjpeg.stride;

                // use a fresh jpeg_buffer each iteration to avoid OOM:
                // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
                jpeg_mem_dest(&cinfo_grayscale, &jpeg_buffer_grayscale, &jpeg_len_grayscale);
                // this takes 80-130ms, longer than frame at 10fps
                jpeg_start_compress(&cinfo_grayscale, TRUE);

                for (uint8_t *Y_row = Y; cinfo_grayscale.next_scanline < settings->mjpeg.height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->mjpeg.stride) {
                        y_rows[i] = std::min(Y_row, Y_max);
                    }

                    jpeg_write_raw_data(&cinfo_grayscale, grayscale_rows, 16);
                }
                jpeg_finish_compress(&cinfo_grayscale);

                // Stream to browsers
                // prepare headers
                contentLengthLength = snprintf(contentLength, 20, "%lu\r\n\r\n", jpeg_len_grayscale);
                pthread_mutex_lock(&motionConnectionsMutex);
                auto end = motionConnections.end();
                for (auto fd = motionConnections.begin(); fd != end; fd++) {
                    send(*fd, boundary, boundaryLength, 0);
                    send(*fd, contentLength, contentLengthLength, 0);
                    send(*fd, jpeg_buffer_grayscale, jpeg_len_grayscale, 0);
                }
                pthread_mutex_unlock(&motionConnectionsMutex);
                
                /*
                snprintf(filename, 40, "/home/pi/stills/motion_%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer_grayscale, jpeg_len_grayscale, 1, fp);
                fclose(fp);
                */

                free(jpeg_buffer_grayscale);
                jpeg_buffer_grayscale = NULL;

                printf("  Detection Time: %ldms Changed pixels: %d MOTION DETECTED\n", ms_int.count(), changed_pixels);

            } else {
                printf("  Detection Time: %ldms Changed pixels: %d\n", ms_int.count(), changed_pixels);
            }
        }

        request->reuse(Request::ReuseBuffers);
        camera->queueRequest(request);

        pthread_mutex_lock(&requestsAtCameraMetricMutex);
        requestsAtCameraMetric++;
        pthread_mutex_unlock(&requestsAtCameraMetricMutex);
    }
    //jpeg_destroy_compress(&cinfo_yuv);
    jpeg_destroy_compress(&cinfo_grayscale);
    // Free motion detection buffers
    free(previousFrame);
    free(uv_data);
    
    return NULL;
}


time_t previousSeconds = 0;
int frames = 0;
unsigned int frame_counter = 0;
static void requestComplete(Request *request)
{
    struct timespec delta;
    clock_gettime(CLOCK_REALTIME, &delta);
    if (previousSeconds == delta.tv_sec) {
        frames++;
    } else {
        fprintf(stdout, "Frames: %d\n", frames);
        frames = 1;
        previousSeconds = delta.tv_sec;
    }

    // TODO: can we add a timestamp into the frame here?
    // unfortunately we can't set the Request cookie after the fact

    pthread_mutex_lock(&requestsAtCameraMetricMutex);
    requestsAtCameraMetric--;
    pthread_mutex_unlock(&requestsAtCameraMetricMutex);

    pthread_mutex_lock(&processingMutex);
    processingQueue.push_back(request);
    pthread_cond_signal(&processingCondition);
    pthread_mutex_unlock(&processingMutex);

    pthread_mutex_lock(&requestsAtQueueMetricMutex);
    requestsAtQueueMetric++;
    pthread_mutex_unlock(&requestsAtQueueMetricMutex);
}



// HTTP SERVER STUFF
static int httpServerCleanupArg = 0;
static void httpServerThreadCleanup(void *arg) {
    pthread_mutex_unlock(&runningMutex);

    logInfo("HTTP Server thread closing down");
    for (int i = 0; i < http_fds_count; i++) {
        printf("Closing %d\n", http_fds[i].fd);
        close(http_fds[i].fd);
    }

    pthread_mutex_lock(&streamConnectionsMutex);
    streamConnections.clear();
    pthread_mutex_unlock(&streamConnectionsMutex);
}

static void *httpServerThread(void*) {
    bool r;
    int status;
    int ret;

    int port = 8080;
    int listener, socketfd;
    int length;
    static struct sockaddr_in cli_addr; /* static = initialised to zeros */
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */
    int request_length = 8192;
    char request[8192];
    char response_header[128];
    int response_header_length;
    char response1[1024], response2[1024]; // for index.html
    int response1_length, response2_length;
    int i;
    int yes = 1;

    pthread_cleanup_push(httpServerThreadCleanup, NULL);

    response_header_length = snprintf(response_header, 128, "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\nCache-Control: no-store\r\nContent-Type: multipart/x-mixed-replace; boundary=HATCHA\r\n\r\n");

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
    pthread_mutex_lock(&runningMutex);
    r = running;
    pthread_mutex_unlock(&runningMutex);
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
                        response2_length = fread(response2, 1, 1024, f);
                        fclose(f);

                        // note the Connection:close header to prevent keep-alive
                        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);
                        ret = write(http_fds[i].fd, response1, response1_length);
                        ret += write(http_fds[i].fd, response2, response2_length);
                        
                        pthread_mutex_lock(&runningMutex);
                        r = running;
                        pthread_mutex_unlock(&runningMutex);

                    } else if (strncmp(request, "GET /stream.mjpeg HTTP", 22) == 0) {
                        ret = write(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&streamConnectionsMutex);
                        streamConnections.push_back(http_fds[i].fd);
                        pthread_mutex_unlock(&streamConnectionsMutex);

                    } else if (strncmp(request, "GET /motion.mjpeg HTTP", 22) == 0) {
                        // send motion pixels as mjpeg
                        ret = write(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&streamConnectionsMutex);
                        motionConnections.push_back(http_fds[i].fd);
                        pthread_mutex_unlock(&streamConnectionsMutex);

                    } else {
                        // request for something we don't handle
                        response1_length = snprintf(response1, 1024, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot found");
                        ret = write(http_fds[i].fd, response1, response1_length);
                    }
                    
                    // Send prelim headers
                    if (ret < 1) {
                        logInfo("Failed to write");
                        close(http_fds[i].fd);
                    }

                    i++;
                } else {
                    // Receive returned 0. Socket has been closed.
                    pthread_mutex_lock(&streamConnectionsMutex);
                    // remove from mjpeg streaming list
                    streamConnections.remove( http_fds[i].fd );
                    pthread_mutex_unlock(&streamConnectionsMutex);

                    pthread_mutex_lock(&motionConnectionsMutex);
                    motionConnections.remove( http_fds[i].fd );
                    pthread_mutex_unlock(&motionConnectionsMutex);

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
                        pthread_mutex_lock(&runningMutex);
                        r = running;
                        pthread_mutex_unlock(&runningMutex);
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

        pthread_mutex_lock(&runningMutex);
        r = running;
        pthread_mutex_unlock(&runningMutex);
    }

    pthread_cleanup_pop(httpServerCleanupArg);
    return NULL;
}

int main()
{
    signal(SIGINT, signalHandler);

    //int width = 1640;
    //int height = 922;
    // 1920 1080
    // 640 480

    settings.h264.width = 1920;
    settings.h264.height = 1080;
    settings.h264.stride = 1920;
    settings.h264.fps = 30;
    
    // a third of full-res
    // 640 x 360
    settings.mjpeg.width = settings.h264.width / 3;
    settings.mjpeg.height = settings.h264.height / 3;
    settings.mjpeg.stride = settings.h264.width / 3;
    settings.mjpeg.quality = 95;

    pthread_t processingThreadId;
    void* processingThreadStatus;
    pthread_mutex_init(&processingMutex, NULL);
    pthread_cond_init(&processingCondition, NULL);

    pthread_t httpServerThreadId;
    void* httpServerThreadStatus;
    pthread_mutex_init(&streamConnectionsMutex, NULL);

    pthread_mutex_init(&requestsAtCameraMetricMutex, NULL);
    pthread_mutex_init(&requestsAtQueueMetricMutex, NULL);
    

    std::unique_ptr<CameraManager> cm = std::make_unique<CameraManager>();
    cm->start();

    if (cm->cameras().empty()) {
       std::cout << "No cameras were identified on the system."
                 << std::endl;
       cm->stop();
       return EXIT_FAILURE;
    }

    std::string cameraId = cm->cameras()[0]->id();
    camera = cm->get(cameraId);

    camera->acquire();

    // VideoRecording
    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration( { StreamRole::VideoRecording, StreamRole::VideoRecording } );
    // smaller for MJPEG
    StreamConfiguration &mjpegStreamConfig = config->at(0);
    mjpegStreamConfig.pixelFormat = libcamera::formats::YUV420;
    //mjpegStreamConfig.colorSpace = libcamera::ColorSpace::Jpeg;
    mjpegStreamConfig.size.width = settings.mjpeg.width;
    mjpegStreamConfig.size.height = settings.mjpeg.height;
    mjpegStreamConfig.bufferCount = 10;

    // Full resolution for h264
    StreamConfiguration &h264StreamConfig = config->at(1);
    h264StreamConfig.pixelFormat = libcamera::formats::YUV420;
    //h264StreamConfig.colorSpace = libcamera::ColorSpace::Jpeg; // TODO: is this necessary?
    h264StreamConfig.size.width = settings.h264.width;
    h264StreamConfig.size.height = settings.h264.height;
    // This seems to default to 4, but we want to queue buffers for post
    // processing, so we need to raise it.
    // 10 works but 20 fails and isn't an error we can catch
    h264StreamConfig.bufferCount = 10;

    CameraConfiguration::Status status = config->validate();
    if (status == CameraConfiguration::Invalid) {
        fprintf(stderr, "Camera Configuration is invalid\n");
    } else if (status == CameraConfiguration::Adjusted) {
        fprintf(stderr, "Camera Configuration was invalid and has been adjusted\n");
    }
    settings.mjpeg.stride = mjpegStreamConfig.stride;
    printf("MJPEG Stride after configuring: %d\n", mjpegStreamConfig.stride);
    settings.mjpeg.y_length = settings.mjpeg.stride * settings.mjpeg.height;
    settings.mjpeg.uv_length = settings.mjpeg.y_length / 4; // I think this is the right size

    // Configuration might have set an unexpected stride, use it.
    // Think YUV420 needs 64bit alignment according to:
    // https://github.com/raspberrypi/picamera2/blob/main/picamera2/configuration.py
    settings.h264.stride = h264StreamConfig.stride;
    printf("H264 Stride after configuring: %d\n", h264StreamConfig.stride);
    settings.h264.y_length = settings.h264.stride * settings.h264.height;
    settings.h264.uv_length = settings.h264.y_length / 4; // I think this is the right size


    camera->configure(config.get());
    h264Stream = h264StreamConfig.stream();
    mjpegStream = mjpegStreamConfig.stream();

    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    std::vector<std::unique_ptr<Request>> requests;
    for (StreamConfiguration &cfg : *config) {
        Stream *stream = cfg.stream();
        // TODO: it's possible we'll need our own allocator for raspi,
        // so we can enqueue many frames for processing in other threads
        int ret = allocator->allocate(stream);
        // This error handling doesn't catch a failure to allocate 20 buffers
        if (ret < 0) {
            std::cerr << "Can't allocate buffers" << std::endl;
            return -ENOMEM;
        }

        const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
        size_t allocated = buffers.size();
        std::cout << "Allocated " << allocated << " buffers for stream" << std::endl;

        // now do mmaping
        for (unsigned int i = 0; i < buffers.size(); ++i) {
            const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

            // Unless libcamera or other libs change, we know what kind of planes we're dealing with.
            // For each buffer, planes seem to share the same dmabuf fd.
            // However, libcamera-apps handles this with looping logic.
            // NOTE: At one point I tried more than 1 mmap per dmabuf, but for some reason it didn't work.
            auto planes = buffer->planes();
            size_t buffer_size = planes[0].length + planes[1].length + planes[2].length;
            if (planes[0].fd.get() != planes[1].fd.get() || planes[0].fd.get() != planes[2].fd.get()) {
                // ERROR, unexpected buffer layout
                printf("UNEXPECTED BUFFER AND FD LAYOUT\n");
                return 1;
            }

            uint8_t* memory = (uint8_t*)mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, planes[0].fd.get(), 0);
            if (!memory) {
                printf("UNEXPECTED MMAP\n");
                return 1;
            }
            // Get a handle to each plane's memory region within the mmap/dmabuf
            mapped_buffers[buffer.get()].push_back(memory);
            mapped_buffers[buffer.get()].push_back(memory + planes[0].length);
            mapped_buffers[buffer.get()].push_back(memory + planes[0].length + planes[1].length);
        }
    }


    // Now create requests and add buffers
    Stream *stream1 = mjpegStreamConfig.stream();
    Stream *stream2 = h264StreamConfig.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers1 = allocator->buffers(stream1);
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers2 = allocator->buffers(stream2);
    
    for (unsigned int i = 0; i < buffers1.size(); ++i) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request)
        {
            std::cerr << "Can't create request" << std::endl;
            return -ENOMEM;
        }


        int ret = request->addBuffer(stream1, buffers1[i].get());
        if (ret < 0)
        {
            printf("Can't add buffer1 for request\n");
            return ret;
        }
        ret = request->addBuffer(stream2, buffers2[i].get());
        if (ret < 0)
        {
            printf("Can't add buffer2 for request\n");
            return ret;
        }
        

        requests.push_back(std::move(request));
        
    }

    camera->requestCompleted.connect(requestComplete);

    // sets fps (via frame duration limts)
    // TODO: create ControlList and move to global var
    // TODO: is there a raspi-specific implementation of this?
    libcamera::ControlList controls(libcamera::controls::controls);
    int64_t frame_time = 1000000 / settings.h264.fps; // in microseconds
    controls.set(libcamera::controls::FrameDurationLimits, libcamera::Span<const int64_t, 2>({ frame_time, frame_time }));

    camera->start(&controls);
    for (auto &request : requests) {
        camera->queueRequest(request.get());
        pthread_mutex_lock(&requestsAtCameraMetricMutex);
        requestsAtCameraMetric++;
        pthread_mutex_unlock(&requestsAtCameraMetricMutex);

    }

    // start thread
    pthread_create(&processingThreadId, NULL, processingThread, (void*)&settings);
    pthread_create(&httpServerThreadId, NULL, httpServerThread, (void*)&settings);

    //60 * 60 * 24 * 7; // days
    int duration = 60;
    duration = 3600 * 24; // 12 hours
    duration = 3600 * 6;

    //for (int i = 0; i < duration; i++) {
    int r = 1;
    while (r) {
        //std::cout << "Sleeping" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        int requestsAtCameraMet = 0;
        int requestsAtQueueMet = 0;

        int streamConns = 0;
        int motionConns = 0;

        pthread_mutex_lock(&requestsAtCameraMetricMutex);
        requestsAtCameraMet = requestsAtCameraMetric;
        pthread_mutex_unlock(&requestsAtCameraMetricMutex);

        pthread_mutex_lock(&requestsAtQueueMetricMutex);
        requestsAtQueueMet = requestsAtQueueMetric;
        pthread_mutex_unlock(&requestsAtQueueMetricMutex);

        pthread_mutex_lock(&streamConnectionsMutex);
        streamConns = streamConnections.size();
        pthread_mutex_unlock(&streamConnectionsMutex);

        pthread_mutex_lock(&motionConnectionsMutex);
        motionConns = motionConnections.size();
        pthread_mutex_unlock(&motionConnectionsMutex);

        printf(
            "Req@Camera: %d Req@Queue: %d Stream conns: %d Motion conns %d\n",
            requestsAtCameraMet,
            requestsAtQueueMet,
            streamConns,
            motionConns
        );

        pthread_mutex_lock(&runningMutex);
        r = running;
        pthread_mutex_unlock(&runningMutex);
    }

    running = 0;
    pthread_cancel(processingThreadId);
    pthread_cancel(httpServerThreadId);
    // figure out how to cancel this thread properly
    pthread_mutex_lock(&processingMutex);
    pthread_cond_signal(&processingCondition);
    pthread_mutex_unlock(&processingMutex);

    pthread_join(processingThreadId, &processingThreadStatus);
    pthread_join(httpServerThreadId, &httpServerThreadStatus);

    camera->stop();
    // for each stream
    for (StreamConfiguration &cfg : *config) {
        Stream *stream = cfg.stream();
        allocator->free(stream);
    }
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();

    return 0;
}
