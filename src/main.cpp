#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

#include <pthread.h>
#include <sys/mman.h>

#include <jpeglib.h>
#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

using namespace libcamera;

static std::shared_ptr<Camera> camera;

std::map<FrameBuffer *, std::vector<libcamera::Span<uint8_t>>> mapped_buffers;


pthread_mutex_t processingMutex;
pthread_cond_t processingCondition;
std::list<libcamera::Request*> processingQueue;

pthread_mutex_t connectionsMutex;
unsigned int mjpegConnections = 1;


FILE* fp;

int running = 1;


//int width = 1640;
//int height = 922;
// 1920 1080
int width = 1920;
int height = 1080;

// TODO: i don't know how to align
int align = 16;
int fps = 10;

typedef struct PiMeraSettings {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int fps;
    unsigned int mjpeg_quality = 90;
} PiMeraSettings;

PiMeraSettings settings;


static void* processingThread(void* arg) {
    const PiMeraSettings *settings = (const PiMeraSettings*)arg;
    //other settings
    int jpeg_quality = 90;

    libcamera::Request* request;

    // BEGIN JPEG STUFF
    // set up jpeg stuff once
    int stride2 = settings->stride / 2;
    uint8_t *Y;
    uint8_t *U;
    uint8_t *V;
    size_t Y_size;
    size_t U_size;
    size_t V_size;
    uint8_t *Y_max;
    uint8_t *U_max;
    uint8_t *V_max;
    uint8_t* jpeg_buffer = NULL;
    jpeg_mem_len_t jpeg_len = 0;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];
    JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
    char filename[40];

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    cinfo.image_width = settings->width;
    cinfo.image_height = settings->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    cinfo.jpeg_color_space = cinfo.in_color_space;
    cinfo.restart_interval = 0;

    //jpeg_set_colorspace(&cinfo, cinfo.in_color_space);
    jpeg_set_defaults(&cinfo);
    cinfo.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo, jpeg_quality, TRUE);
    // END JPEG STUFF


    // BEGIN MOTION DETECTION STUFF
    uint8_t* previousFrame = (uint8_t*) malloc(settings->width * settings->height);
    // END MOTION DETECTION STUFF


    unsigned int frame_counter = 0;

    unsigned int mjpeg_delta = settings->fps; // * 60; // once a second
    unsigned int mjpeg_threshold = mjpeg_delta;
    unsigned int mjpeg_connections = 0;

    unsigned int detection_delta = settings->fps / 3;
    unsigned int detection_threshold = detection_delta;

    while (running) {
        pthread_mutex_lock(&processingMutex);
        while (processingQueue.size() == 0) {
            pthread_cond_wait(&processingCondition, &processingMutex);
        }
        request = processingQueue.front();
        processingQueue.pop_front(); // why both?
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
                const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();
                FrameBuffer* fb = buffers.begin()->second;

                // update threshold for when we should encode next mjpeg frame
                mjpeg_threshold = frame_counter + mjpeg_delta;

                // need to push to thread
                Y = mapped_buffers[ fb ][0].data();
                U = mapped_buffers[ fb ][1].data();
                V = mapped_buffers[ fb ][2].data();
                Y_size = mapped_buffers[ fb ][0].size_bytes();
                U_size = mapped_buffers[ fb ][1].size_bytes();
                V_size = mapped_buffers[ fb ][2].size_bytes();
                Y_max = (Y + Y_size) - settings->stride;
                U_max = (U + U_size) - stride2;
                V_max = (V + V_size) - stride2;


                auto start_time = std::chrono::high_resolution_clock::now();
                // use a fresh jpeg_buffer each iteration to avoid OOM:
        // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
                jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_len);	
                // this takes 80-130ms, longer than frame at 10fps
                jpeg_start_compress(&cinfo, TRUE);

                for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; cinfo.next_scanline < settings->height;)
                {
                    for (int i = 0; i < 16; i++, Y_row += settings->stride) {
                        y_rows[i] = std::min(Y_row, Y_max);
                    }
                    for (int i = 0; i < 8; i++, U_row += stride2, V_row += stride2) {
                        u_rows[i] = std::min(U_row, U_max);
                        v_rows[i] = std::min(V_row, V_max);
                    }

                    //JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
                    jpeg_write_raw_data(&cinfo, rows, 16);
                    //jpeg_write_scanlines(&cinfo, rows, 16);
                }
                jpeg_finish_compress(&cinfo);
                printf("new len %lu\n", jpeg_len);
                //jpeg_len = 0;
                
                auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);
                printf("Encode time %ldms\n", ms_int.count());
                // TODO: would love to emit metrics to influx

                snprintf(filename, 40, "/home/pi/stills/%d.jpeg", frame_counter);
                fp = fopen(filename, "wb");
                fwrite(jpeg_buffer, jpeg_len, 1, fp);
                fclose(fp);

                free(jpeg_buffer);
                jpeg_buffer = NULL;
            }
        }


        // DO H264 ENCODING?

        // DO MOTION DETECTION?
        // let's try assuming the Y channel IS gray
        if (frame_counter > detection_threshold) {
            detection_threshold = frame_counter + detection_delta;

            // 
        }


        request->reuse(Request::ReuseBuffers);
        camera->queueRequest(request);
    }
    jpeg_destroy_compress(&cinfo);

    free(jpeg_buffer);
    
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
    pthread_create(&processingThreadId, NULL, processingThread, (void*)&settings);

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

        uint8_t* memory = (uint8_t*)mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, planes[0].fd.get(), 0);
        if (!memory) {
            printf("UNEXPECTED MMAP\n");
            return 1;
        }
        // Get a handle to each plane's memory region within the mmap/dmabuf
        mapped_buffers[buffer.get()].push_back(
            libcamera::Span<uint8_t>(memory, planes[0].length)
        );
        mapped_buffers[buffer.get()].push_back(
            libcamera::Span<uint8_t>(memory + planes[0].length, planes[1].length)
        );
        mapped_buffers[buffer.get()].push_back(
            libcamera::Span<uint8_t>(memory + planes[0].length + planes[1].length, planes[2].length)
        );


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

    //60 * 60 * 24 * 7; // days
    int duration = 60;
    duration = 3600 * 24; // 12 hours

    for (int i = 0; i < duration; i++) {
        std::cout << "Sleeping" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    pthread_cancel(processingThreadId);
    pthread_join(processingThreadId, &processingThreadStatus);

    camera->stop();
    allocator->free(stream);
    delete allocator;
    camera->release();
    camera.reset();
    cm->stop();

    return 0;
}
