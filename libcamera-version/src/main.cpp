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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <jpeglib.h>
#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

//#include "encoder.hpp"
#include "logging.hpp"
#include "annotate.hpp"
#include "encoder.hpp"
#include "image.hpp"
#include "settings.hpp"
#include "reuse.hpp"

using namespace libcamera;

static std::shared_ptr<Camera> camera;

// TODO: convert this to vector or array[3] of uint8_t*
// we can pre-calculate length of each YUV data buffer in main
// and store in settings
std::map<FrameBuffer *, std::vector<uint8_t*>> mapped_buffers;

pthread_mutex_t processing_mutex;
pthread_cond_t processing_condition;
std::list<libcamera::Request*> processing_queue;

pthread_mutex_t reuse_queue_mutex;
std::queue<Request*> reuse_queue;

#define MAX_POLL_FDS 100
pthread_mutex_t stream_connections_mutex;
std::list<int> stream_connections;
pthread_mutex_t motion_connections_mutex;
std::list<int> motion_connections;
struct pollfd http_fds[MAX_POLL_FDS];
int http_fds_count = 0;


pthread_mutex_t running_mutex;
int running = 1;

// Miscellaneous
pthread_mutex_t requestsAtCameraMetricMutex;
pthread_mutex_t requestsAtQueueMetricMutex;
int requestsAtCameraMetric = 0;
int requestsAtQueueMetric = 0;

// TODO: raise this when figure out how to alloc more buffers
// a map of numbered Request to the timestamp it came into requestComplete()
struct timeval request_timestamps[30];




PiMeraSettings settings;


void signalHandler(int signal_number) {
    logError("Got signal. Exiting", __func__);
    // TODO: think there's an atomic variable meant to help with "signal caught" flags
    pthread_mutex_lock(&running_mutex);
    running = 0;
    pthread_mutex_unlock(&running_mutex);
    logInfo("Signaled");
}


static void* processingThread(void* arg) {
    const PiMeraSettings *settings = (const PiMeraSettings*)arg;
    libcamera::Request* request;
    FrameBuffer* mjpeg_buffer;
    FrameBuffer* h264_buffer;
    struct timeval request_timestamp;
    bool reuse_request = true;
    bool recording = false;

    // BEGIN JPEG STUFF
    // set up jpeg stuff once
    uint8_t *Y;
    uint8_t *U;
    uint8_t *V;
    uint8_t *Y_max;
    uint8_t *U_max;
    uint8_t *V_max;
    
    // fullsize JPEG for timelapse images
    PiMeraJPEG full;

    // for yuv data
    PiMeraJPEG color;
    
    // for grayscale data
    PiMeraJPEG grayscale;

    // grayscale helpers
    uint8_t* uv_data;
    // static row data for U and V since we want grayscale
    JSAMPROW grayscale_uv_rows[8];
    // use same data for u and v
    JSAMPARRAY grayscale_rows[] = { grayscale.y_rows, grayscale_uv_rows, grayscale_uv_rows };

    char filename[101];

    // mjpeg streaming stuff
    const char *boundary = "--HATCHA\r\nContent-Type: image/jpeg\r\nContent-length: ";
    int boundary_length = strlen(boundary);
    char content_length[20];
    int content_length_length;

    imageInit(&full, settings->h264.width, settings->h264.height, 90);

    // yuv
    imageInit(&color, settings->mjpeg.width, settings->mjpeg.height, settings->mjpeg.quality);

    // grayscale
    imageInit(&grayscale, settings->mjpeg.width, settings->mjpeg.height, settings->mjpeg.quality);


    // since can't get grayscale color space to work, prepare array of uv data to simulate grayscale from Y plane data
    // prepare 8 rows of data, maybe?
    int uv_length = (settings->mjpeg.stride * 8) / 2;
    uv_data = (uint8_t*) malloc(uv_length);
    memset(uv_data, 128, uv_length);
    // prepare grayscale_uv_rows pointers
    uint8_t* U_row = uv_data;
    for (int i = 0; i < 8; i++, U_row += settings->mjpeg.stride2) {
        grayscale_uv_rows[i] = U_row;
    }
    grayscale.yuv_rows[1] = grayscale_uv_rows;
    grayscale.yuv_rows[2] = grayscale_uv_rows;
    // END JPEG STUFF


    unsigned int frame_counter = 0;

    // MJPEG related
    unsigned int mjpeg_sleep = settings->h264.fps / 10; // N times a second
    unsigned int mjpeg_at = mjpeg_sleep;
    unsigned int num_mjpeg_connections = 0;

    // BEGIN MOTION DETECTION STUFF
    uint8_t* previous_frame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);
    uint8_t* motion_frame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);
    // we'll push pixels with motion to 255, leave others at their regular values,
    // unsure whether this will help us visualize where motion took place
    uint8_t* highlighted_motion_frame = (uint8_t*) malloc(settings->mjpeg.stride * settings->mjpeg.height);

    unsigned int detection_sleep = settings->h264.fps / 3;
    unsigned int detection_at = detection_sleep;

    unsigned int cooldown_sleep = settings->h264.fps * 2;
    unsigned int cooldown_at = 0;

    // 10 second timelapse images
    unsigned int timelapse_sleep = settings->h264.fps *  10;
    unsigned int timelapse_at = timelapse_sleep;


    // number of pixels that must be changed to detect motion
    unsigned int changed_pixels_threshold = settings->mjpeg.y_length * settings->percentage_for_motion;
    // END MOTION DETECTION STUFF

    printf("Pixel threshold %d\n", changed_pixels_threshold);

    // Wait for first frame, and store it in previous_frame
    pthread_mutex_lock(&processing_mutex);
    while (processing_queue.size() == 0) {
        pthread_cond_wait(&processing_condition, &processing_mutex);
    }
    printf("Using first frame as previous\n");
    request = processing_queue.front();
    processing_queue.pop_front(); // TODO: why both?
    pthread_mutex_unlock(&processing_mutex);

    mjpeg_buffer = request->findBuffer(settings->mjpeg.stream);
    Y = mapped_buffers[ mjpeg_buffer ][0];
    // TODO: only copy regions we care about, once that data is available
    for (int i = 0; i < (settings->mjpeg.stride * settings->mjpeg.height); i++) {
        previous_frame[i] = Y[i];
    }

    // for testing grayscale
    /*
    for (int y = 0, i = 0; y < settings->mjpeg.height; y++) {
        for (int x = 0; x < settings->mjpeg.width; x++, i++) {
            if (x > 255) {
                motion_frame[i] = 255;
            } else {
                motion_frame[i] = x;
            }
        }
    }
    */

    while (running) {

        // Check re-use queue and re-add to camera
        pthread_mutex_lock(&reuse_queue_mutex);
        while(reuse_queue.size() > 0) {
            Request *r = reuse_queue.front();
            reuse_queue.pop();
            pthread_mutex_unlock(&reuse_queue_mutex);
            
            logInfo("Queueing request back to camera");
            r->reuse(Request::ReuseBuffers);
            camera->queueRequest(r);

            pthread_mutex_lock(&requestsAtCameraMetricMutex);
            requestsAtCameraMetric++;
            pthread_mutex_unlock(&requestsAtCameraMetricMutex);

            pthread_mutex_lock(&reuse_queue_mutex);
        }
        pthread_mutex_unlock(&reuse_queue_mutex);


        pthread_mutex_lock(&processing_mutex);
        while (processing_queue.size() == 0) {
            // TODO: this can starve the "return to camera" loop above
            pthread_cond_wait(&processing_condition, &processing_mutex);
        }
        request = processing_queue.front();
        processing_queue.pop_front();
        pthread_mutex_unlock(&processing_mutex);

        pthread_mutex_lock(&requestsAtQueueMetricMutex);
        requestsAtQueueMetric--;
        pthread_mutex_unlock(&requestsAtQueueMetricMutex);

        // TODO: make this work with graceful shutdowns and HUPs
        if (request->status() == Request::RequestCancelled) {
            // skip?
            printf("CANCEL\n");
            return NULL;
        }

        reuse_request = true;

        // Decisions
        /*
        - do we need to convert to jpeg and send to http?
        - do we need to convert to h264
        - do we need to do motion detection?
        */

        frame_counter++;

        //unsigned int index = request->cookie();
        request_timestamp = request_timestamps[ request->cookie() ];

        //mjpeg_buffer = request->findBuffer(settings->mjpeg.stream);



        // do mjpeg compression?
        // only if we have connections
        // TODO: fix naming on these
        pthread_mutex_lock(&stream_connections_mutex);
        num_mjpeg_connections = stream_connections.size();
        pthread_mutex_unlock(&stream_connections_mutex);

        //maybe?
        /*
        if (mjpeg_at < frame_counter) {
            // check conns
            pthread_mutex_lock(&stream_connections_mutex);
            num_mjpeg_connections = stream_connections.size();
            pthread_mutex_unlock(&stream_connections_mutex);
            if (num_mjpeg_connections > 0) {
                // next time
                mjpeg_at = frame_counter + 1;
            }

        } else if (mjpeg_at == frame_counter) {
            // do

        } else {
            // wait
        }
        */

        // Is anyone watching the stream?
        if (0 && num_mjpeg_connections > 0 && frame_counter >= mjpeg_at) {
            // update threshold for when we should encode next mjpeg frame
            mjpeg_at = frame_counter + mjpeg_sleep;

            mjpeg_buffer = request->findBuffer(settings->mjpeg.stream);
            Y = mapped_buffers[ mjpeg_buffer ][0];
            U = mapped_buffers[ mjpeg_buffer ][1];
            V = mapped_buffers[ mjpeg_buffer ][2];
            Y_max = Y + settings->mjpeg.y_max;
            U_max = U + settings->mjpeg.uv_max;
            V_max = V + settings->mjpeg.uv_max;

            // TODO: hoist this up, only compute once if we need it
            char timestamp[40];
            struct tm *t = localtime(&request_timestamp.tv_sec);
            strftime(timestamp, 39, "%Y-%m-%d %H:%M:%S", t);
            annotate(timestamp, 3, strlen(timestamp), Y, 10 + (10 * settings->mjpeg.stride), settings->mjpeg.stride);

            auto start_time = std::chrono::high_resolution_clock::now();
            imageStart(&color);

            for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; color.cinfo.next_scanline < settings->mjpeg.height;)
            {
                // TODO: I don't like all these min calls
                for (int i = 0; i < 16; i++, Y_row += settings->mjpeg.stride) {
                    color.y_rows[i] = std::min(Y_row, Y_max);
                }
                for (int i = 0; i < 8; i++, U_row += settings->mjpeg.stride2, V_row += settings->mjpeg.stride2) {
                    color.u_rows[i] = std::min(U_row, U_max);
                    color.v_rows[i] = std::min(V_row, V_max);
                }
                jpeg_write_raw_data(&color.cinfo, color.yuv_rows, 16);
            }
            jpeg_finish_compress(&color.cinfo);
            
            auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);
            //printf("    MJPEG compress time %ldms\n", ms_int.count());
            // TODO: would love to emit metrics to influx

            // Stream to browsers
            // prepare headers
            content_length_length = snprintf(content_length, 20, "%lu\r\n\r\n", color.buffer_length);
            pthread_mutex_lock(&stream_connections_mutex);
            auto end = stream_connections.end();
            for (auto fd = stream_connections.begin(); fd != end; fd++) {
                send(*fd, boundary, boundary_length, 0);
                send(*fd, content_length, content_length_length, 0);
                send(*fd, color.buffer, color.buffer_length, 0);
            }
            pthread_mutex_unlock(&stream_connections_mutex);

            /*

            snprintf(filename, 40, "/home/pi/stills/%d.jpeg", frame_counter);
            fp = fopen(filename, "wb");
            fwrite(jpeg_buffer_yuv, jpeg_len_yuv, 1, fp);
            fclose(fp);
            */

            free(color.buffer);
            color.buffer = NULL;
        }


        // SHOULD WE DO MOTION DETECTION FOR THIS FRAME?
        if (frame_counter >= detection_at) {
            detection_at = frame_counter + detection_sleep;
            printf("Checking frame for motion\n");

            uint changed_pixels = 0;
            uint compared_pixels = 0;

            auto start_time = std::chrono::high_resolution_clock::now();

            mjpeg_buffer = request->findBuffer(settings->mjpeg.stream);
            Y = mapped_buffers[ mjpeg_buffer ][0];

            // skipping motion_frame stuff altogether saves 2ms more
            //memcpy(motion_frame, Y, settings->y_length);


            // TODO: only compare regions we care about, once that data is available
            // TODO: this takes 500ms to 1080p, not fast enough
            // 320ms without motion_frame writes
            // 40ms for 640x480

            int batch = 16;
            // constants
            uint8x16_t _threshold = vdupq_n_u8(settings->pixel_delta_threshold);
            uint8x16_t _one = vdupq_n_u8(1);

            uint8x16_t _a, _b, _c;

            uint8_t* current = Y;
            uint8_t* current_max = current + settings->mjpeg.y_length;
            uint8_t* previous = previous_frame;
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


            auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);

            if (changed_pixels > changed_pixels_threshold) {
                // motion was detected!
                memcpy(previous_frame, Y, settings->mjpeg.y_length);

                Y_max = Y + settings->mjpeg.y_max;

                // skipping motion frame for now
                // TODO: need conditional around this
                /*
                imageStart(&grayscale);

                for (uint8_t *Y_row = Y; grayscale.cinfo.next_scanline < settings->mjpeg.height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->mjpeg.stride) {
                        grayscale.y_rows[i] = std::min(Y_row, Y_max);
                    }

                    jpeg_write_raw_data(&grayscale.cinfo, grayscale.yuv_rows, 16);
                }
                jpeg_finish_compress(&grayscale.cinfo);

                // Stream to browsers
                // prepare headers
                content_length_length = snprintf(content_length, 20, "%lu\r\n\r\n", grayscale.buffer_length);
                pthread_mutex_lock(&motion_connections_mutex);
                auto end = motion_connections.end();
                for (auto fd = motion_connections.begin(); fd != end; fd++) {
                    send(*fd, boundary, boundary_length, 0);
                    send(*fd, content_length, content_length_length, 0);
                    send(*fd, grayscale.buffer, grayscale.buffer_length, 0);
                }
                pthread_mutex_unlock(&motion_connections_mutex);
                free(grayscale.buffer);
                grayscale.buffer = NULL;
                */
                

                /*
                snprintf(filename, 40, "/home/pi/stills/motion_%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer_grayscale, jpeg_len_grayscale, 1, fp);
                fclose(fp);
                */



                printf("  Detection Time: %ldms Changed pixels: %d MOTION DETECTED\n", ms_int.count(), changed_pixels);

                // don't re-use this, we're going to hand data to h264 encoder
                reuse_request = false;


                //logInfo("Encoding DMABUF");
                if (!recording) {
                    recording = true;
                    // signal first frame in recording
                    encoder_enqueue(request, request_timestamp, true, false);
                } else {
                    encoder_enqueue(request, request_timestamp, false, false);
                }
                cooldown_at = frame_counter + cooldown_sleep;

            } else {
                printf("  Detection Time: %ldms Changed pixels: %d\n", ms_int.count(), changed_pixels);

                // TODO: wait for cooldown
                if (recording && frame_counter > cooldown_at) {
                    recording = false;
                    // signal last frame in recording
                    encoder_enqueue(request, request_timestamp, false, true);
                    reuse_request = false;
                }


                /*
                if reached cooldown (300ms after motion last detected?)
                    if we were saving to h2
                        tell encoder to save and close
                */
            }
        } else if (recording) {
            // Not time to check for motion, but save frame if recording
            
            encoder_enqueue(request, request_timestamp, false, false);
            reuse_request = false;
        }

        if (0 && frame_counter >= timelapse_at) {
            timelapse_at = frame_counter + timelapse_sleep;

            h264_buffer = request->findBuffer(settings->h264.stream);

            Y = mapped_buffers[ h264_buffer ][0];
            U = mapped_buffers[ h264_buffer ][1];
            V = mapped_buffers[ h264_buffer ][2];
            // TODO: perhaps precompute these into settings
            Y_max = Y + settings->h264.y_max;
            U_max = U + settings->h264.uv_max;
            V_max = V + settings->h264.uv_max;

            char timestamp[40];
            struct tm *t = localtime(&request_timestamp.tv_sec);
            strftime(timestamp, 39, "%Y-%m-%d %H:%M:%S", t);
            annotate(timestamp, 4, strlen(timestamp), Y, 10 + (10 * settings->h264.stride), settings->h264.stride);

            auto start_time = std::chrono::high_resolution_clock::now();
            imageStart(&full);

            for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; full.cinfo.next_scanline < settings->h264.height;)
            {
                for (int i = 0; i < 16; i++, Y_row += settings->h264.stride) {
                    full.y_rows[i] = std::min(Y_row, Y_max);
                }
                for (int i = 0; i < 8; i++, U_row += settings->h264.stride2, V_row += settings->h264.stride2) {
                    full.u_rows[i] = std::min(U_row, U_max);
                    full.v_rows[i] = std::min(V_row, V_max);
                }
                jpeg_write_raw_data(&full.cinfo, full.yuv_rows, 16);
            }
            jpeg_finish_compress(&full.cinfo);
            
            auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);
            printf("    Full-sized image compress time %ldms\n", ms_int.count());
            // TODO: would love to emit metrics to influx
            strftime(timestamp, 39, "%Y%m%d%H%M%S", t);
            snprintf(filename, 100, "/home/pi/stills/%s.%dx%d.jpeg", timestamp, settings->h264.width, settings->h264.height);
            FILE *fp = fopen(filename, "wb");
            fwrite(full.buffer, full.buffer_length, 1, fp);
            fclose(fp);

            free(full.buffer);
            full.buffer = NULL;
        }

        // reuse if we haven't handed data over to h264 encoder
        if (reuse_request) {
            request->reuse(Request::ReuseBuffers);
            camera->queueRequest(request);
            pthread_mutex_lock(&requestsAtCameraMetricMutex);
            requestsAtCameraMetric++;
            pthread_mutex_unlock(&requestsAtCameraMetricMutex);
        }

    }
    //jpeg_destroy_compress(&cinfo_yuv);
    jpeg_destroy_compress(&color.cinfo);
    jpeg_destroy_compress(&full.cinfo);
    jpeg_destroy_compress(&grayscale.cinfo);
    // Free motion detection buffers
    free(previous_frame);
    free(uv_data);
    
    return NULL;
}


time_t previousSeconds = 0;
int frames = 0;
static void requestComplete(Request *request)
{
    // TODO: do we need this?
    struct timeval tv;
    gettimeofday(&tv, NULL);

    //struct timespec delta;
    //clock_gettime(CLOCK_REALTIME, &delta);
    if (previousSeconds == tv.tv_sec) {
        frames++;
    } else {
        fprintf(stdout, "Frames: %d\n", frames);
        frames = 1;
        previousSeconds = tv.tv_sec;
    }

    // associate timestamp with this request
    request_timestamps[ request->cookie() ].tv_sec = tv.tv_sec;
    request_timestamps[ request->cookie() ].tv_usec = tv.tv_usec;

    // TODO: can we add a timestamp into the frame here?
    // unfortunately we can't set the Request cookie after the fact

    pthread_mutex_lock(&requestsAtCameraMetricMutex);
    requestsAtCameraMetric--;
    pthread_mutex_unlock(&requestsAtCameraMetricMutex);

    pthread_mutex_lock(&processing_mutex);
    processing_queue.push_back(request);
    pthread_cond_signal(&processing_condition);
    pthread_mutex_unlock(&processing_mutex);

    pthread_mutex_lock(&requestsAtQueueMetricMutex);
    requestsAtQueueMetric++;
    pthread_mutex_unlock(&requestsAtQueueMetricMutex);
}

// TODO: FILL THIS OUT
void reuse_request(Request *request) {
    // enqueue for re-add
    pthread_mutex_lock(&reuse_queue_mutex);
    reuse_queue.push(request);
    pthread_mutex_unlock(&reuse_queue_mutex);
}



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
    stream_connections.clear();
    pthread_mutex_unlock(&stream_connections_mutex);
}

static void *http_server_thread(void*) {
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

    pthread_cleanup_push(http_server_thread_cleanup, NULL);

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
                        response2_length = fread(response2, 1, 1024, f);
                        fclose(f);

                        // note the Connection:close header to prevent keep-alive
                        response1_length = snprintf(response1, 1024, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", response2_length);
                        ret = write(http_fds[i].fd, response1, response1_length);
                        ret += write(http_fds[i].fd, response2, response2_length);
                        
                        pthread_mutex_lock(&running_mutex);
                        r = running;
                        pthread_mutex_unlock(&running_mutex);

                    } else if (strncmp(request, "GET /stream.mjpeg HTTP", 22) == 0) {
                        ret = write(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&stream_connections_mutex);
                        stream_connections.push_back(http_fds[i].fd);
                        pthread_mutex_unlock(&stream_connections_mutex);

                    } else if (strncmp(request, "GET /motion.mjpeg HTTP", 22) == 0) {
                        // send motion pixels as mjpeg
                        ret = write(http_fds[i].fd, response_header, response_header_length);

                        pthread_mutex_lock(&stream_connections_mutex);
                        motion_connections.push_back(http_fds[i].fd);
                        pthread_mutex_unlock(&stream_connections_mutex);

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
                    pthread_mutex_lock(&stream_connections_mutex);
                    // remove from mjpeg streaming list
                    stream_connections.remove( http_fds[i].fd );
                    pthread_mutex_unlock(&stream_connections_mutex);

                    pthread_mutex_lock(&motion_connections_mutex);
                    motion_connections.remove( http_fds[i].fd );
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

    pthread_cleanup_pop(http_server_cleanup_arg);
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
    settings.h264.fps = 30;
    
    // a third of full-res
    // 640 x 360
    settings.mjpeg.width = settings.h264.width / 3;
    settings.mjpeg.height = settings.h264.height / 3;
    settings.mjpeg.quality = 95;

    pthread_t processingThreadId;
    void* processingThreadStatus;
    pthread_mutex_init(&processing_mutex, NULL);
    pthread_cond_init(&processing_condition, NULL);

    pthread_t http_server_threadId;
    void* http_server_threadStatus;
    pthread_mutex_init(&stream_connections_mutex, NULL);

    pthread_mutex_init(&requestsAtCameraMetricMutex, NULL);
    pthread_mutex_init(&requestsAtQueueMetricMutex, NULL);
    pthread_mutex_init(&reuse_queue_mutex, NULL);
    

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
    StreamConfiguration &mjpeg_stream_config = config->at(0);
    mjpeg_stream_config.pixelFormat = libcamera::formats::YUV420;
    //mjpeg_stream_config.colorSpace = libcamera::ColorSpace::Jpeg;
    mjpeg_stream_config.size.width = settings.mjpeg.width;
    mjpeg_stream_config.size.height = settings.mjpeg.height;
    mjpeg_stream_config.bufferCount = 10;

    // Full resolution for h264
    StreamConfiguration &h264_stream_config = config->at(1);
    h264_stream_config.pixelFormat = libcamera::formats::YUV420;
    //h264_stream_config.colorSpace = libcamera::ColorSpace::Jpeg; // TODO: is this necessary?
    h264_stream_config.size.width = settings.h264.width;
    h264_stream_config.size.height = settings.h264.height;
    // This seems to default to 4, but we want to queue buffers for post
    // processing, so we need to raise it.
    // 10 works but 20 fails and isn't an error we can catch
    h264_stream_config.bufferCount = 10;

    CameraConfiguration::Status status = config->validate();
    if (status == CameraConfiguration::Invalid) {
        fprintf(stderr, "Camera Configuration is invalid\n");
    } else if (status == CameraConfiguration::Adjusted) {
        fprintf(stderr, "Camera Configuration was invalid and has been adjusted\n");
    }
    settings.mjpeg.stride = mjpeg_stream_config.stride;
    settings.mjpeg.stride2 = settings.mjpeg.stride / 2;
    printf("MJPEG Stride after configuring: %d\n", settings.mjpeg.stride);
    settings.mjpeg.y_length = settings.mjpeg.stride * settings.mjpeg.height;
    settings.mjpeg.uv_length = settings.mjpeg.y_length / 4;
    settings.mjpeg.y_max = settings.mjpeg.y_length - settings.mjpeg.stride;
    settings.mjpeg.uv_max = settings.mjpeg.uv_length - settings.mjpeg.stride2;

    // Configuration might have set an unexpected stride, use it.
    // Think YUV420 needs 64bit alignment according to:
    // https://github.com/raspberrypi/picamera2/blob/main/picamera2/configuration.py

    settings.h264.stride = h264_stream_config.stride;
    settings.h264.stride2 = settings.h264.stride / 2;
    printf("H264 Stride after configuring: %d\n", settings.h264.stride2);
    settings.h264.y_length = settings.h264.stride * settings.h264.height;
    settings.h264.uv_length = settings.h264.y_length / 4;
    settings.h264.yuv_length = settings.h264.y_length + (settings.h264.uv_length * 2);
    settings.h264.y_max = settings.h264.y_length - settings.h264.stride;
    settings.h264.uv_max = settings.h264.uv_length - settings.h264.stride2;
    


    camera->configure(config.get());
    settings.h264.stream = h264_stream_config.stream();
    settings.mjpeg.stream = mjpeg_stream_config.stream();

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
    Stream *stream1 = mjpeg_stream_config.stream();
    Stream *stream2 = h264_stream_config.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers1 = allocator->buffers(stream1);
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers2 = allocator->buffers(stream2);
    
    for (unsigned int i = 0; i < buffers1.size(); ++i) {
        // set cookie to i index
        std::unique_ptr<Request> request = camera->createRequest(i);
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
    pthread_create(&http_server_threadId, NULL, http_server_thread, (void*)&settings);

    encoder_init(&settings);


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

        pthread_mutex_lock(&stream_connections_mutex);
        streamConns = stream_connections.size();
        pthread_mutex_unlock(&stream_connections_mutex);

        pthread_mutex_lock(&motion_connections_mutex);
        motionConns = motion_connections.size();
        pthread_mutex_unlock(&motion_connections_mutex);

        printf(
            "Req@Camera: %d Req@Queue: %d Stream conns: %d Motion conns %d\n",
            requestsAtCameraMet,
            requestsAtQueueMet,
            streamConns,
            motionConns
        );

        pthread_mutex_lock(&running_mutex);
        r = running;
        pthread_mutex_unlock(&running_mutex);
    }

    pthread_mutex_lock(&running_mutex);
    running = 0;
    pthread_mutex_unlock(&running_mutex);

    pthread_cancel(processingThreadId);
    pthread_cancel(http_server_threadId);
    // figure out how to cancel this thread properly
    pthread_mutex_lock(&processing_mutex);
    pthread_cond_signal(&processing_condition);
    pthread_mutex_unlock(&processing_mutex);

    pthread_join(processingThreadId, &processingThreadStatus);
    pthread_join(http_server_threadId, &http_server_threadStatus);

    encoder_destroy();

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
