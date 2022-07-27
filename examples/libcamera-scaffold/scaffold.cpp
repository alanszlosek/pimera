#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/libcamera.h>

using namespace libcamera;

static std::shared_ptr<Camera> camera;

time_t previousSeconds = 0;
int frames = 0;
static void requestComplete(Request *request)
{
    std::unique_ptr<Request> request2;
    if (request->status() == Request::RequestCancelled)
        return;
    const std::map<const Stream *, FrameBuffer *> &buffers = request->buffers();

    request->reuse(Request::ReuseBuffers);
    camera->queueRequest(request);

    struct timespec delta;
    clock_gettime(CLOCK_REALTIME, &delta);
    if (previousSeconds == delta.tv_sec) {
        frames++;
    } else {
        fprintf(stdout, "Frames: %d\n", frames);
        frames = 1;
        previousSeconds = delta.tv_sec;
    }
}

int main()
{
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
    streamConfig.pixelFormat = libcamera::formats::BGR888;
    streamConfig.size.width = 1640; //640;
    streamConfig.size.height = 922; //480;
    // This seems to default to 4, but we want to queue buffers for post
    // processing, so we need to raise it.
    // 10 works ... oddly, but 20 fails behind the scenes. doesn't apear
    // to be an error we can catch
    streamConfig.bufferCount = 20;

    // TODO: check return value of this
    CameraConfiguration::Status status = config->validate();
    if (status == CameraConfiguration::Invalid) {
        fprintf(stderr, "Camera Configuration is invalid\n");
    } else if (status == CameraConfiguration::Adjusted) {
        fprintf(stderr, "Camera Configuration was invalid and has been adjusted\n");
    }

    camera->configure(config.get());

    FrameBufferAllocator *allocator = new FrameBufferAllocator(camera);

    for (StreamConfiguration &cfg : *config) {
        // TODO: it's possible we'll need our own allocator for raspi,
        // so we can enqueue many frames for processing
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
    int framerate = 30;
    int64_t frame_time = 1000000 / framerate; // in microseconds
    controls.set(libcamera::controls::FrameDurationLimits, { frame_time, frame_time });

    camera->start(&controls);
    for (auto &request : requests)
       camera->queueRequest(request.get());

    //60 * 60 * 24 * 7; // days
    int duration = 10;

    for (int i = 0; i < duration; i++) {
        std::cout << "Sleeping" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // TODO: add graceful stop and shutdown code here


    return 0;
}
