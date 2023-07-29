#ifndef HANDLES_H
#define HANDLES_H

typedef struct HANDLES_S HANDLES;

struct HANDLES_S {
    MMAL_COMPONENT_T *camera;    /// Pointer to the camera component

    MMAL_COMPONENT_T *splitter;  /// Pointer to the splitter component
    MMAL_COMPONENT_T *full_splitter; // Pointer to splitter
    MMAL_COMPONENT_T *resized_splitter; // Pointer to splitter
    MMAL_CONNECTION_T *full_splitter_connection;/// Pointer to the connection from camera to splitter
    MMAL_CONNECTION_T *resized_splitter_connection;/// Pointer to the connection from camera to splitter

    MMAL_COMPONENT_T *resizer;
    MMAL_CONNECTION_T *resizer_connection;

    MMAL_PORT_T* splitterYuvPort;

    MMAL_COMPONENT_T *h264_encoder;   /// Pointer to the encoder component
    MMAL_COMPONENT_T *mjpeg_encoder;   /// Pointer to the encoder component
    MMAL_CONNECTION_T *h264_encoder_connection; /// Pointer to the connection from camera to encoder
    MMAL_CONNECTION_T *mjpeg_encoder_connection;

    MMAL_PORT_T* h264EncoderOutputPort;

    MMAL_POOL_T *h264_encoder_pool; /// Pointer to the pool of buffers used by splitter output port 0
    MMAL_POOL_T *mjpeg_encoder_pool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *yuvPool; /// Pointer to the pool of buffers used by encoder output port

    //SETTINGS* settings;
    //CALLBACK_USERDATA* h264CallbackUserdata;        /// Used to move data to the encoder callback
};


#endif