#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "git_vid_codec.h"

// Create a 600-frame demo video with various visual patterns for benchmarking
int main() {
    const int width = 1920;
    const int height = 1080;
    const int frames = 600; // Exactly 600 frames for consistent benchmarking
    
    printf("Creating 600-frame demo video for benchmarking...\n");
    
    // Create output directory
    if (mkdir("demo_frames", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error creating demo_frames directory: %s\n", strerror(errno));
        return 1;
    }
    
    for (int frame = 0; frame < frames; frame++) {
        char filename[256];
        generate_frame_path("demo_frames", frame, filename, sizeof(filename));
        
        FILE* f = fopen(filename, "wb");
        if (!f) {
            fprintf(stderr, "Error creating frame %d: %s\n", frame, strerror(errno));
            continue;
        }
        
        // Create different scenes based on frame number (600 frames total)
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint8_t r, g, b;
                
                // Scene 1: Animated rainbow gradient (frames 0-99)
                if (frame < 100) {
                    float t = (float)frame / 99.0f;
                    float hue = fmod((float)x / width + t, 1.0f) * 6.0f;
                    int hi = (int)hue;
                    float f = hue - hi;
                    
                    switch (hi % 6) {
                        case 0: r = 255; g = (uint8_t)(255 * f); b = 0; break;
                        case 1: r = (uint8_t)(255 * (1-f)); g = 255; b = 0; break;
                        case 2: r = 0; g = 255; b = (uint8_t)(255 * f); break;
                        case 3: r = 0; g = (uint8_t)(255 * (1-f)); b = 255; break;
                        case 4: r = (uint8_t)(255 * f); g = 0; b = 255; break;
                        case 5: r = 255; g = 0; b = (uint8_t)(255 * (1-f)); break;
                    }
                }
                // Scene 2: Rotating spiral pattern (frames 100-199)
                else if (frame < 200) {
                    float cx = width / 2.0f;
                    float cy = height / 2.0f;
                    float dx = x - cx;
                    float dy = y - cy;
                    float angle = atan2(dy, dx) + (frame - 100) * 0.1f;
                    float dist = sqrt(dx*dx + dy*dy);
                    
                    float spiral = sin(angle * 8 + dist * 0.1f) * 0.5f + 0.5f;
                    r = (uint8_t)(spiral * 255);
                    g = (uint8_t)((1-spiral) * 255);
                    b = (uint8_t)(sin(dist * 0.05f + frame * 0.1f) * 127 + 128);
                }
                // Scene 3: Bouncing circles (frames 200-299)
                else if (frame < 300) {
                    r = g = b = 20; // Dark background
                    
                    // Multiple bouncing circles
                    for (int i = 0; i < 5; i++) {
                        float t = (frame - 200) * 0.1f + i * 1.2f;
                        float cx = width * 0.5f + sin(t) * width * 0.3f;
                        float cy = height * 0.5f + cos(t * 1.3f + i) * height * 0.3f;
                        float dist = sqrt((x-cx)*(x-cx) + (y-cy)*(y-cy));
                        
                        if (dist < 40) {
                            r = (uint8_t)(255 * (i % 3 == 0));
                            g = (uint8_t)(255 * (i % 3 == 1));
                            b = (uint8_t)(255 * (i % 3 == 2));
                        }
                    }
                }
                // Scene 4: Plasma effect (frames 300-399)
                else if (frame < 400) {
                    float t = (frame - 300) * 0.1f;
                    float plasma = sin(x * 0.02f + t) + sin(y * 0.03f + t) + 
                                  sin((x + y) * 0.02f + t) + sin(sqrt(x*x + y*y) * 0.02f + t);
                    plasma = (plasma + 4) / 8; // Normalize to 0-1
                    
                    r = (uint8_t)(sin(plasma * M_PI) * 255);
                    g = (uint8_t)(sin(plasma * M_PI + 2) * 255);
                    b = (uint8_t)(sin(plasma * M_PI + 4) * 255);
                }
                // Scene 5: Matrix-style falling code (frames 400-499)
                else if (frame < 500) {
                    r = g = b = 0; // Black background
                    
                    // Vertical streams of green characters
                    int stream_x = x / 20;
                    int stream_y = (y + (frame - 400) * 5) % (height + 100);
                    
                    if (stream_x % 3 == 0 && stream_y < height && 
                        (stream_y % 20) < 15 && (x % 20) < 15) {
                        float intensity = 1.0f - (float)stream_y / height;
                        g = (uint8_t)(intensity * 255);
                    }
                }
                // Scene 6: Mandelbrot zoom (frames 500-599)
                else {
                    float zoom = pow(1.05f, frame - 500); // Slower zoom for 100 frames
                    float cx = -0.7269f;
                    float cy = 0.1889f;
                    
                    float zx = (x - width/2.0f) / (width/4.0f) / zoom + cx;
                    float zy = (y - height/2.0f) / (height/4.0f) / zoom + cy;
                    
                    int iter = 0;
                    float x0 = zx, y0 = zy;
                    while (iter < 100 && x0*x0 + y0*y0 < 4) {
                        float xtemp = x0*x0 - y0*y0 + zx;
                        y0 = 2*x0*y0 + zy;
                        x0 = xtemp;
                        iter++;
                    }
                    
                    if (iter == 100) {
                        r = g = b = 0;
                    } else {
                        float t = (float)iter / 100.0f;
                        r = (uint8_t)(sin(t * 16) * 127 + 128);
                        g = (uint8_t)(sin(t * 13 + 2) * 127 + 128);
                        b = (uint8_t)(sin(t * 11 + 4) * 127 + 128);
                    }
                }
                
                fputc(r, f);
                fputc(g, f);
                fputc(b, f);
            }
        }
        
        fclose(f);
        
        if (frame % 50 == 0) {
            printf("Generated frame %d/%d (%.1f%%)\n", frame + 1, frames, 
                   (float)(frame + 1) / frames * 100.0f);
        }
    }
    
    printf("\n600-frame demo created successfully!\n");
    printf("Frames saved in demo_frames/ directory\n");
    printf("Total frames: %d\n", frames);
    printf("Resolution: %dx%d\n", width, height);
    printf("Size per frame: %d bytes\n", width * height * 3);
    printf("Total uncompressed size: %.2f MB\n", 
           (float)(width * height * 3 * frames) / (1024 * 1024));
    
    return 0;
}