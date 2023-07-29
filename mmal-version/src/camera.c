#include <stdio.h>

#include "camera.h"
#include "log.h"

#define DESIRED_OUTPUT_BUFFERS 3
#define MJPEG_BITRATE 25000000 // 25Mbits/s
#define H264_BITRATE  25000000 // 62.5Mbit/s OR 25000000 25Mbits/s
#define DEBUG 1

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

// CAMERA FUNCTIONS

void cameraControlCallback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    fprintf(stdout, " HEY cameraControlCallback, buffer->cmd: %d\n", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

void destroy_camera(MMAL_COMPONENT_T *camera) {
    if (camera) {
        if (DEBUG) {
            logInfo("Destroying camera");
        }
        mmal_component_destroy(camera);
    }
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


MMAL_STATUS_T create_camera(MMAL_COMPONENT_T **camera_handle, SETTINGS *settings) {
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
    format->es->video.width = settings->vcosWidth; // TODO: do we need vcosWidth here?
    format->es->video.height = settings->vcosHeight;
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


// BEGIN NEW SPLITTER FUNCTIONS
MMAL_STATUS_T create_splitter(MMAL_COMPONENT_T **splitter_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, int num_outputs) {
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

void destroy_splitter(MMAL_COMPONENT_T *splitter, MMAL_CONNECTION_T *connection) {
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




MMAL_STATUS_T create_resizer(MMAL_COMPONENT_T **handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MJPEG_SETTINGS *settings) {
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

void destroy_resizer(MMAL_COMPONENT_T *splitter, MMAL_CONNECTION_T *connection) {
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
void destroy_mjpeg_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool) {
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

MMAL_STATUS_T create_mjpeg_encoder(MMAL_COMPONENT_T **handle, MMAL_POOL_T **pool_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MMAL_PORT_BH_CB_T callback, MJPEG_SETTINGS *settings) {
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


// H264 FUNCTIONS
void destroy_h264_encoder(MMAL_COMPONENT_T *component, MMAL_CONNECTION_T *connection, MMAL_POOL_T *pool) {
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

MMAL_STATUS_T create_h264_encoder(MMAL_COMPONENT_T **handle, MMAL_POOL_T **pool_handle, MMAL_CONNECTION_T **connection_handle, MMAL_PORT_T *output_port, MMAL_PORT_BH_CB_T callback, H264_SETTINGS *settings) {
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

// HELPER FUNCTIONS

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

