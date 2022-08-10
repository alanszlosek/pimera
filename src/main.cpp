#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

#include <pthread.h>
#include <sys/mman.h>
#include <string.h>

#include <jpeglib.h>
#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

using namespace libcamera;

static std::shared_ptr<Camera> camera;

// TODO: convert this to vector or array[3] of uint8_t*
// we can pre-calculate length of each YUV data buffer in main
// and store in settings
std::map<FrameBuffer *, std::vector<uint8_t*>> mapped_buffers;


pthread_mutex_t processingMutex;
pthread_cond_t processingCondition;
std::list<libcamera::Request*> processingQueue;

pthread_mutex_t connectionsMutex;
unsigned int mjpegConnections = 0;

// for saving JPEG while debugging, will go away
FILE* fp;

int running = 1;


//int width = 1640;
//int height = 922;
// 1920 1080
// 640 480
int width = 640;
int height = 480;
int fps = 10;

typedef struct PiMeraSettings {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int fps;
    unsigned int mjpeg_quality = 90;
    // if this percentage of pixels change, motion is detected
    float percentage = 0.05;

    // TODO: pre-calculate these in main for YUV plane buffers
    size_t y_length;
    size_t uv_length;

} PiMeraSettings;

PiMeraSettings settings;


static void* processingThread(void* arg) {
    const PiMeraSettings *settings = (const PiMeraSettings*)arg;
    libcamera::Request* request;

    // BEGIN JPEG STUFF
    // set up jpeg stuff once
    int stride2 = settings->stride / 2;
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

    // yuv
    cinfo_yuv.err = jpeg_std_error(&jerr_yuv);
    jpeg_create_compress(&cinfo_yuv);
    cinfo_yuv.image_width = settings->stride;
    cinfo_yuv.image_height = settings->height;
    cinfo_yuv.input_components = 3;
    cinfo_yuv.in_color_space = JCS_YCbCr;
    cinfo_yuv.jpeg_color_space = cinfo_yuv.in_color_space;
    cinfo_yuv.restart_interval = 0;
    jpeg_set_defaults(&cinfo_yuv);
    cinfo_yuv.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo_yuv, settings->mjpeg_quality, TRUE);

    // grayscale
    cinfo_grayscale.err = jpeg_std_error(&jerr_grayscale);
    jpeg_create_compress(&cinfo_grayscale);
    // TODO: figure out how to exclude extra stride pixels
    cinfo_grayscale.image_width = settings->stride;
    cinfo_grayscale.image_height = settings->height;
    //cinfo_grayscale.num_components = 1;
    cinfo_grayscale.input_components = 3;
    cinfo_grayscale.in_color_space = JCS_YCbCr;
    //cinfo_grayscale.jpeg_color_space = JCS_GRAYSCALE;
    cinfo_grayscale.restart_interval = 0;
    jpeg_set_defaults(&cinfo_grayscale);
    // since can't get grayscale color space to work, prepare array of uv data to simulate grayscale from Y plane data
    // prepare 8 rows of data, maybe?
    int uv_length = (settings->stride * 8) / 2;
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
    jpeg_set_quality(&cinfo_grayscale, settings->mjpeg_quality, TRUE);
    

    // END JPEG STUFF


    unsigned int frame_counter = 0;

    // TODO: the threshold variable names need a rethink
    unsigned int mjpeg_delta = settings->fps; // * 60; // once a second
    unsigned int mjpeg_threshold = mjpeg_delta;
    unsigned int mjpeg_connections = 0;

    // BEGIN MOTION DETECTION STUFF
    uint8_t* previousFrame1 = (uint8_t*) malloc(settings->stride * settings->height);
    uint8_t* previousFrame2 = (uint8_t*) malloc(settings->stride * settings->height);
    uint8_t* previousFrame;
    uint8_t* motionFrame = (uint8_t*) malloc(settings->stride * settings->height);
    // we'll push pixels with motion to 255, leave others at their regular values,
    // unsure whether this will help us visualize where motion took place
    uint8_t* highlightedMotionFrame = (uint8_t*) malloc(settings->stride * settings->height);; // 
    unsigned int detection_delta = settings->fps / 3;
    unsigned int detection_threshold = detection_delta;

    // number of pixels that must be changed to detect motion
    unsigned int detected_pixels_threshold = settings->y_length * settings->percentage;
    // END MOTION DETECTION STUFF

    printf("Pixel threshold %d\n", detected_pixels_threshold);

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
    previousFrame = previousFrame1;
    Y = mapped_buffers[ fb ][0];
    // TODO: only copy regions we care about, once that data is available
    for (int i = 0; i < (settings->stride * settings->height); i++) {
        previousFrame[i] = Y[i];
    }

    // for testing grayscale
    /*
    for (int y = 0, i = 0; y < settings->height; y++) {
        for (int x = 0; x < settings->width; x++, i++) {
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

        const std::map<const Stream*, FrameBuffer*> &buffers2 = request->buffers();
        FrameBuffer* fb = buffers2.begin()->second;

        // do mjpeg compression?
        // only if we have connections
        // TODO: fix naming on these
        pthread_mutex_lock(&connectionsMutex);
        mjpeg_connections = mjpegConnections;
        pthread_mutex_unlock(&connectionsMutex);

        // TODO: how do we add timestamp into the frame?

        // is anyone trying to watch the stream?
        if (mjpeg_connections > 0) {
            if (frame_counter > mjpeg_threshold) {
                // update threshold for when we should encode next mjpeg frame
                mjpeg_threshold = frame_counter + mjpeg_delta;

                Y = mapped_buffers[ fb ][0];
                U = mapped_buffers[ fb ][1];
                V = mapped_buffers[ fb ][2];
                Y_max = (Y + settings->y_length) - settings->stride;
                U_max = (U + settings->uv_length) - stride2;
                V_max = (V + settings->uv_length) - stride2;

                auto start_time = std::chrono::high_resolution_clock::now();
                // use a fresh jpeg_buffer each iteration to avoid OOM:
                // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
                jpeg_mem_dest(&cinfo_yuv, &jpeg_buffer_yuv, &jpeg_len_yuv);
                // this takes 80-130ms, longer than frame at 10fps
                jpeg_start_compress(&cinfo_yuv, TRUE);

                for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; cinfo_yuv.next_scanline < settings->height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->stride) {
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
                printf("Encode time %ldms\n", ms_int.count());
                // TODO: would love to emit metrics to influx

                snprintf(filename, 40, "/home/pi/stills/%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer_yuv, jpeg_len_yuv, 1, fp);
                fclose(fp);

                free(jpeg_buffer_yuv);
                jpeg_buffer_yuv = NULL;
            }
        }


        // DO H264 ENCODING?

        // SHOULD WE DO MOTION DETECTION FOR THIS FRAME?
        // let's try assuming the Y channel IS gray
        // TODO: speed this up with vector processing or SIMD
        if (frame_counter > detection_threshold) {
            detection_threshold = frame_counter + detection_delta;
            //printf("Checking frame for motion\n");

            uint8_t* nextFrame = (previousFrame == previousFrame1 ? previousFrame2 : previousFrame1);
            uint detected_pixels = 0;
            uint compared_pixels = 0;

            auto start_time = std::chrono::high_resolution_clock::now();

            Y = mapped_buffers[ fb ][0];
            /*
            NOTE: copying pixels in bulk ahead of time, instead of using an else below
            to copy unchanged pixels individually saves 20ms off
            */
            memcpy(motionFrame, Y, settings->y_length);
            // TODO: only compare regions we care about, once that data is available
            // TODO: this takes 500ms to 1080p, not fast enough
            int end = (settings->stride * settings->height);
            for (int i = 0; i < end; i++) {
                // compare previous and current
                int delta = abs(previousFrame[i] - Y[i]);
                //printf("Delta: %d Previous: %d Current %d\n", delta, previousFrame[i], Y[i]);
                compared_pixels++;
                if (delta > 35) {
                    // highlight pixels that have changed
                    motionFrame[i] = 255;
                    detected_pixels++;
                }
            }
            auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);

            if (detected_pixels > detected_pixels_threshold) {
                // motion was detected!
                printf("Time: %ldms Compared pixels: %d Changed pixels: %d MOTION DETECTED\n", ms_int.count(), compared_pixels, detected_pixels);

                // swap previousFrame pointer for next time
                memcpy(previousFrame, Y, settings->y_length);

                // need to push to thread
                Y = motionFrame;
                Y_max = (Y + settings->y_length) - settings->stride;
                U = uv_data;
                U_max = (U + settings->uv_length) - stride2;

                // use a fresh jpeg_buffer each iteration to avoid OOM:
                // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
                jpeg_mem_dest(&cinfo_grayscale, &jpeg_buffer_grayscale, &jpeg_len_grayscale);
                // this takes 80-130ms, longer than frame at 10fps
                jpeg_start_compress(&cinfo_grayscale, TRUE);

                for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; cinfo_grayscale.next_scanline < settings->height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->stride) {
                        //rows_grayscale[i] = std::min(Y_row, Y_max);
                        y_rows[i] = std::min(Y_row, Y_max);
                    }

                    jpeg_write_raw_data(&cinfo_grayscale, grayscale_rows, 16);
                    //jpeg_write_scanlines(&cinfo_grayscale, rows_grayscale, 16);
                }
                jpeg_finish_compress(&cinfo_grayscale);
                
                snprintf(filename, 40, "/home/pi/stills/motion_%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer_grayscale, jpeg_len_grayscale, 1, fp);
                fclose(fp);

                free(jpeg_buffer_grayscale);
                jpeg_buffer_grayscale = NULL;
            } else {
                printf("Time: %ldms Compared pixels: %d Changed pixels: %d\n", ms_int.count(), compared_pixels, detected_pixels);
            }
        }

        request->reuse(Request::ReuseBuffers);
        camera->queueRequest(request);
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

    pthread_mutex_lock(&processingMutex);
    processingQueue.push_back(request);
    pthread_cond_signal(&processingCondition);
    pthread_mutex_unlock(&processingMutex);
}

int main()
{
    settings.width = width;
    settings.height = height;
    settings.stride = width;
    settings.fps = fps;

    pthread_t processingThreadId;
    void* processingThreadStatus;
    pthread_mutex_init(&processingMutex, NULL);
    pthread_cond_init(&processingCondition, NULL);
    

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
    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration( { StreamRole::VideoRecording } );
    StreamConfiguration &streamConfig = config->at(0);

    //streamConfig.pixelFormat = libcamera::formats::RGB888;
    streamConfig.pixelFormat = libcamera::formats::YUV420;
    streamConfig.colorSpace = libcamera::ColorSpace::Jpeg; // TODO: is this necessary?

    streamConfig.size.width = width;
    streamConfig.size.height = height;
    // This seems to default to 4, but we want to queue buffers for post
    // processing, so we need to raise it.
    // 10 works ... oddly, but 20 fails behind the scenes. doesn't apear
    // to be an error we can catch
    streamConfig.bufferCount = 10;

    CameraConfiguration::Status status = config->validate();
    if (status == CameraConfiguration::Invalid) {
        fprintf(stderr, "Camera Configuration is invalid\n");
    } else if (status == CameraConfiguration::Adjusted) {
        fprintf(stderr, "Camera Configuration was invalid and has been adjusted\n");
    }
    // Configuration might have set an unexpected stride, use it.
    // Think YUV420 needs 64bit alignment according to:
    // https://github.com/raspberrypi/picamera2/blob/main/picamera2/configuration.py
    settings.stride = streamConfig.stride;
    printf("Stride after configuring: %d\n", streamConfig.stride);
    printf("Color space: %s\n", streamConfig.colorSpace->toString().c_str());
    settings.y_length = settings.stride * settings.height;
    settings.uv_length = settings.y_length / 4; // I think this is the right size

    camera->configure(config.get());

    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    for (StreamConfiguration &cfg : *config) {
        // TODO: it's possible we'll need our own allocator for raspi,
        // so we can enqueue many frames for processing in other threads
        int ret = allocator->allocate(cfg.stream());
        // This error handling doesn't catch a failure to allocate 20 buffers
        if (ret < 0) {
            std::cerr << "Can't allocate buffers" << std::endl;
            return -ENOMEM;
        }

        size_t allocated = allocator->buffers(cfg.stream()).size();
        std::cout << "Allocated " << allocated << " buffers for stream" << std::endl;
    }


    Stream *stream = streamConfig.stream();
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
    std::vector<std::unique_ptr<Request>> requests;

    for (unsigned int i = 0; i < buffers.size(); ++i) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request)
        {
            std::cerr << "Can't create request" << std::endl;
            return -ENOMEM;
        }

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

        /*
        printf("Plane lengths. Y: %d U: %d V: %d\n",
            planes[0].length,
            planes[1].length,
            planes[2].length);
        */

        uint8_t* memory = (uint8_t*)mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, planes[0].fd.get(), 0);
        if (!memory) {
            printf("UNEXPECTED MMAP\n");
            return 1;
        }
        // Get a handle to each plane's memory region within the mmap/dmabuf
        mapped_buffers[buffer.get()].push_back(memory);
        mapped_buffers[buffer.get()].push_back(memory + planes[0].length);
        mapped_buffers[buffer.get()].push_back(memory + planes[0].length + planes[1].length);

        int ret = request->addBuffer(stream, buffer.get());
        if (ret < 0)
        {
            std::cerr << "Can't set buffer for request"
                    << std::endl;
            return ret;
        }

        requests.push_back(std::move(request));
    }

    camera->requestCompleted.connect(requestComplete);

    // sets fps (via frame duration limts)
    // TODO: create ControlList and move to global var
    // TODO: is there a raspi-specific implementation of this?
    libcamera::ControlList controls(libcamera::controls::controls);
    int framerate = fps;
    int64_t frame_time = 1000000 / framerate; // in microseconds
    controls.set(libcamera::controls::FrameDurationLimits, { frame_time, frame_time });

    camera->start(&controls);
    for (auto &request : requests)
       camera->queueRequest(request.get());

    // start thread
    pthread_create(&processingThreadId, NULL, processingThread, (void*)&settings);

    //60 * 60 * 24 * 7; // days
    int duration = 60;
    duration = 3600 * 24; // 12 hours
    duration = 3600 * 2;

    for (int i = 0; i < duration; i++) {
        //std::cout << "Sleeping" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    running = 0;
    pthread_cancel(processingThreadId);
    // figure out how to cancel this thread properly
    pthread_mutex_lock(&processingMutex);
    pthread_cond_signal(&processingCondition);
    pthread_mutex_unlock(&processingMutex);

    pthread_join(processingThreadId, &processingThreadStatus);

    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();

    return 0;
}
