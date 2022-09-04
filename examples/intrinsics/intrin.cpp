#include <iomanip>
#include <chrono>
#include <string.h>

#include <arm_neon.h>

// SIMD on RaspberryPi using ARM NEON Intrinsic functions

/*

Motion Detection

1. Convert RGB pixel data to grayscale
2. For each current pixel, subtract from corresponding pixel in previous frame
3. Take absolute value of this subtraction
4. If value is above threshold consider this pixel has having changed
5. If enough pixels have changed, then we've detected motion
*/

int main() {
    // dimensions
    // changed pixel count
    // 2 buffers. filled with 10s
    // fill second occasionally with 20s
    int width = 1920;
    int height = 1080;
    int changed_pixels = 0;
    int pixel_delta_threshold = 10;
    int changed_pixels_threshold = 10;

    uint8_t *current, *previous;
    int len = width * height;
    current = (uint8_t*) malloc(len);
    previous = (uint8_t*) malloc(len);

    // fill both with 10
    memset(current, 10, len);
    memset(previous, 10, len);

    for (int i = 0, j = 0; j < 20; i += 8, j++) {
        previous[i] = 20;
        // will be doing "current - previous:
        // so want previous to contain a higher value so subtraction goes negative
        // to ensure abs() is being used correctly
    }

    uint8_t *c, *p;
    uint8_t *c_max;

    // NON-SIMD VERSION

    c = current;
    p = previous;
    c_max = c + len;

    auto start_time1 = std::chrono::high_resolution_clock::now();

    //printf("%d - %d = %d\n", *c, *p, abs(*c - *p));
    for (; c < c_max; c++, p++) {
        int delta = abs(*c - *p);
        if (delta >= pixel_delta_threshold) {
            changed_pixels++;
        }
    }
    auto ms_int1 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time1);
    printf("Without SIMD: %ldms Changed pixels: %d\n", ms_int1.count(), changed_pixels);
    if (changed_pixels > changed_pixels_threshold) {
        printf("MOTION DETECTED. Pixels changed: %d\n", changed_pixels);
    }

    printf("\n\n");


    // SIMD VERSION

    changed_pixels = 0;
    c = current;
    p = previous;
    //c_max = c + len;

    auto start_time2 = std::chrono::high_resolution_clock::now();
    int batch = 16;

    uint8x16_t a, b, _c;
    uint8x16_t threshold = vdupq_n_u8(pixel_delta_threshold);
    uint8x16_t one = vdupq_n_u8(1);
    uint8x16_t count = vdupq_n_u8(0);

    //printf("%d - %d = %d\n", *c, *p, abs(*c - *p));
    for (; c < c_max; c += batch, p += batch) {
        a = vld1q_u8(c);
        b = vld1q_u8(p);
        // int delta = abs(*c - *p);
        _c = vabdq_u8(a, b);

        // if c > threshold, set bitmask
        a = vcgeq_u8(_c, threshold);

        // bitwise AND the mask with a vector that has 1 in every spot
        b = vandq_u8(a, one);

        // sum across vector to get num pixels that changed in this batch
        changed_pixels += vaddvq_u8(b);
    }



    auto ms_int2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time2);
    printf("With SIMD: %ldms Changed pixels: %d\n", ms_int2.count(), changed_pixels);
    if (changed_pixels > changed_pixels_threshold) {
        printf("MOTION DETECTED. Pixels changed: %d\n", changed_pixels);
    }


    free(current);
    free(previous);


    return 0;
}


















/*
memset(uv_data, 128, uv_length);


for (int i = 0; i < settings->y_length; i++) {
    // compare previous and current
    int delta = abs(previousFrame[i] - Y[i]);
    compared_pixels++;
    if (delta > settings->pixel_change) {
        // highlight pixels that have changed
        motionFrame[i] = 255;
        changed_pixels++;
    }
}

auto start_time = std::chrono::high_resolution_clock::now();

auto ms_int = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);

printf("Time1: %ldms Changed pixels: %d\n", ms_int.count(), changed_pixels);

printf("Time2: %ldms Changed pixels: %d\n", ms_int.count(), changed_pixels);

    _a = vld1q_u8(a);
    _b = vld1q_u8(b);

    // subtraction with absolute value of the result in 1 operation
    uint8x16_t _c;
    _c = vabdq_u8(_b, _a);

    for (int i = 0; i < batch; i++) {
        printf("%d - %d = %d\n", _b[i], _a[i], _c[i]);
    }

    // compare
    // prepare vector to compare against
    uint8x16_t _count = vdupq_n_u8(0); // we'll count into here

    uint8x16_t _threshold = vdupq_n_u8(12);
    uint8x16_t _one = vdupq_n_u8(1);
    // compare, set bitmask for true
    _a = vcgtq_u8(_c, _threshold);

    _b = vandq_u8(_a, _one);
    _count = vaddq_u8(_count, _b);
    _count = vaddq_u8(_count, _b);

    int8_t total = vaddvq_u8(_count);
*/