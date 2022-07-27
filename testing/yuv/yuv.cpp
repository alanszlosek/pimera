#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

typedef struct PiMeraSettings {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
} PiMeraSettings;


#include <jpeglib.h>
#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif


int frame_counter = 0;
void processingThread(const PiMeraSettings *settings) {
    //other settings
    int jpeg_quality = 90;

    // testing stuff
    int buf_len = settings->width * settings->height * 1.5;
    uint8_t* buf;
    FILE* in;

    // BEGIN JPEG STUFF
    // much of this guided by libcamera-apps repo
    // set up jpeg stuff once
    char filename[40];
    uint8_t* jpeg_buffer = NULL;
    jpeg_mem_len_t jpeg_len;
    uint8_t *Y;
    uint8_t *U;
    uint8_t *V;
    size_t Y_size;
    size_t U_size;
    size_t V_size;
    uint8_t* Y_max;
    uint8_t* U_max;
    uint8_t* V_max;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    int stride2 = settings->stride / 2;

    // libjpeg allocates its own buffer. so this needs to be NULL
    jpeg_len = 0;
    jpeg_buffer = NULL;

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
    jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_len);
    
    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];
    JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
    
    // END JPEG STUFF

    // testing stuff
    buf = (uint8_t*)malloc(buf_len);
    Y_size = settings->width * settings->height;
    U_size = V_size = (settings->width/2) * (settings->height/2);
    Y = buf;
    U = buf + Y_size;
    V = buf + Y_size + U_size;
   
    Y_max = (Y + Y_size) - settings->stride;
    U_max = (U + U_size) - stride2;
    V_max = (V + V_size) - stride2;


    int i = 1;
    while (i < 10) {
        i++;
        frame_counter++;

        in = fopen("/home/pi/stills/test.yuv", "rb");
        fread(buf, buf_len, 1, in);
        fclose(in);

        // use a fresh jpeg_buffer each iteration to avoid OOM:
        // https://github.com/libjpeg-turbo/libjpeg-turbo/issues/610
        jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_len);
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
            jpeg_write_raw_data(&cinfo, rows, 16);
        }
        jpeg_finish_compress(&cinfo);
        printf("Length of jpeg_mem_dest buffer: %lu\n", jpeg_len);

        snprintf(filename, 40, "/home/pi/stills/%d.jpeg", frame_counter);
        FILE* fp = fopen(filename, "wb");
        fwrite(jpeg_buffer, jpeg_len, 1, fp);
        fclose(fp);

        free(jpeg_buffer);
        jpeg_buffer = NULL;
    }
    jpeg_destroy_compress(&cinfo);
    
    return;
}

int main()
{
    PiMeraSettings settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.stride = settings.width;

    processingThread(&settings);


    return 0;
}
