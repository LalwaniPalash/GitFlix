#include "git_vid_codec.h"

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Display* display = NULL;
static Window window;
static GC gc;
static XImage* ximage = NULL;
static int screen;
static Visual* visual;
static int depth;
static char* image_data = NULL;

#elif __APPLE__
#include <Cocoa/Cocoa.h>
#include <CoreGraphics/CoreGraphics.h>

static NSWindow* window = nil;
static NSImageView* imageView = nil;
static CGColorSpaceRef colorSpace = NULL;
static CGContextRef bitmapContext = NULL;
static uint8_t* bitmapData = NULL;
static int window_width, window_height;
static int should_close = 0;

#elif _WIN32
#include <windows.h>

static HWND hwnd = NULL;
static HDC hdc = NULL;
static BITMAPINFO bmi;
static int window_width, window_height;
static int should_close = 0;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY:
            should_close = 1;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                should_close = 1;
                PostQuitMessage(0);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

#endif

int display_init(uint32_t width, uint32_t height) {
#ifdef __linux__
    display = XOpenDisplay(NULL);
    if (!display) return GVC_ERROR_DISPLAY;
    
    screen = DefaultScreen(display);
    visual = DefaultVisual(display, screen);
    depth = DefaultDepth(display, screen);
    
    if (depth != 24 && depth != 32) {
        XCloseDisplay(display);
        return GVC_ERROR_DISPLAY;
    }
    
    window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                0, 0, width, height, 1,
                                BlackPixel(display, screen),
                                BlackPixel(display, screen));
    
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);
    
    gc = XCreateGC(display, window, 0, NULL);
    
    // Allocate image data
    int bytes_per_pixel = (depth == 32) ? 4 : 3;
    image_data = malloc(width * height * bytes_per_pixel);
    if (!image_data) {
        XCloseDisplay(display);
        return GVC_ERROR_MEMORY;
    }
    
    ximage = XCreateImage(display, visual, depth, ZPixmap, 0,
                         image_data, width, height, 32, 0);
    if (!ximage) {
        free(image_data);
        XCloseDisplay(display);
        return GVC_ERROR_DISPLAY;
    }
    
#elif __APPLE__
    // Initialize Cocoa application
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    
    colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) {
        fprintf(stderr, "Failed to create color space\n");
        return GVC_ERROR_DISPLAY;
    }
    
    window_width = width;
    window_height = height;
    
    // Get screen dimensions
    NSScreen *mainScreen = [NSScreen mainScreen];
    NSRect screenFrame = [mainScreen visibleFrame];
    
    // Calculate window size that fits on screen while maintaining aspect ratio
    float aspectRatio = (float)width / (float)height;
    int windowWidth = width;
    int windowHeight = height;
    
    // Scale down if too large for screen (leave some margin)
    int maxWidth = (int)(screenFrame.size.width * 0.9);
    int maxHeight = (int)(screenFrame.size.height * 0.9);
    
    if (windowWidth > maxWidth) {
        windowWidth = maxWidth;
        windowHeight = (int)(windowWidth / aspectRatio);
    }
    
    if (windowHeight > maxHeight) {
        windowHeight = maxHeight;
        windowWidth = (int)(windowHeight * aspectRatio);
    }
    
    // Center the window on screen
    int x = (int)((screenFrame.size.width - windowWidth) / 2);
    int y = (int)((screenFrame.size.height - windowHeight) / 2);
    
    // Create window with calculated dimensions
    NSRect frame = NSMakeRect(x, y, windowWidth, windowHeight);
    window = [[NSWindow alloc] initWithContentRect:frame
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    
    [window setTitle:@"Git Video Codec Player"];
    [window makeKeyAndOrderFront:nil];
    
    // Create image view with content frame
    NSRect contentFrame = NSMakeRect(0, 0, windowWidth, windowHeight);
    imageView = [[NSImageView alloc] initWithFrame:contentFrame];
    [imageView setImageScaling:NSImageScaleProportionallyUpOrDown];
    [window setContentView:imageView];
    
    // Create reusable bitmap context for better performance
    // Use RGBA format for better Core Graphics compatibility
    size_t bytesPerRow = width * 4;
    bitmapData = malloc(bytesPerRow * height);
    if (!bitmapData) {
        fprintf(stderr, "Failed to allocate bitmap data\n");
        CGColorSpaceRelease(colorSpace);
        return GVC_ERROR_MEMORY;
    }
    
    bitmapContext = CGBitmapContextCreate(bitmapData, width, height, 8, bytesPerRow,
                                         colorSpace, kCGImageAlphaPremultipliedLast);
    if (!bitmapContext) {
        fprintf(stderr, "Failed to create bitmap context\n");
        free(bitmapData);
        CGColorSpaceRelease(colorSpace);
        return GVC_ERROR_DISPLAY;
    }
    
    should_close = 0;
    
#elif _WIN32
    const char* className = "GitVidCodecPlayer";
    
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    if (!RegisterClass(&wc)) return GVC_ERROR_DISPLAY;
    
    hwnd = CreateWindowEx(0, className, "Git Video Codec Player",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         width, height, NULL, NULL, GetModuleHandle(NULL), NULL);
    
    if (!hwnd) return GVC_ERROR_DISPLAY;
    
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    
    hdc = GetDC(hwnd);
    
    // Setup bitmap info
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    window_width = width;
    window_height = height;
    should_close = 0;
    
#endif
    
    return GVC_SUCCESS;
}

int display_frame(const raw_frame_t* frame) {
    if (!frame || !frame->pixels) return GVC_ERROR_MEMORY;
    
#ifdef __linux__
    if (!display || !ximage) return GVC_ERROR_DISPLAY;
    
    // Convert RGB to display format
    int bytes_per_pixel = (depth == 32) ? 4 : 3;
    
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < frame->width; x++) {
            int src_idx = (y * frame->width + x) * 3;
            int dst_idx = (y * frame->width + x) * bytes_per_pixel;
            
            uint8_t r = frame->pixels[src_idx];
            uint8_t g = frame->pixels[src_idx + 1];
            uint8_t b = frame->pixels[src_idx + 2];
            
            if (bytes_per_pixel == 4) {
                image_data[dst_idx] = b;
                image_data[dst_idx + 1] = g;
                image_data[dst_idx + 2] = r;
                image_data[dst_idx + 3] = 0;
            } else {
                image_data[dst_idx] = b;
                image_data[dst_idx + 1] = g;
                image_data[dst_idx + 2] = r;
            }
        }
    }
    
    XPutImage(display, window, gc, ximage, 0, 0, 0, 0, 
              frame->width, frame->height);
    XFlush(display);
    
#elif __APPLE__
    if (!bitmapContext || !bitmapData || !window || !imageView) return GVC_ERROR_DISPLAY;
    
    // Convert RGB to RGBA and copy to bitmap context
    uint8_t* src = frame->pixels;
    uint8_t* dst = bitmapData;
    for (uint32_t i = 0; i < frame->width * frame->height; i++) {
        dst[0] = src[0]; // R
        dst[1] = src[1]; // G
        dst[2] = src[2]; // B
        dst[3] = 255;    // A (fully opaque)
        src += 3;
        dst += 4;
    }
    
    // Create CGImage from bitmap context
    CGImageRef cgImage = CGBitmapContextCreateImage(bitmapContext);
    if (cgImage) {
        // Convert CGImage to NSImage and display
        NSImage* nsImage = [[NSImage alloc] initWithCGImage:cgImage size:NSZeroSize];
        [imageView setImage:nsImage];
        [nsImage release];
        CGImageRelease(cgImage);
        
        // Minimal event processing for better performance
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskKeyDown
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event && [event type] == NSEventTypeKeyDown && [event keyCode] == 53) {
            should_close = 1;
        }
        
        // Check if window was closed
        if (![window isVisible]) {
            should_close = 1;
        }
    }
    
#elif _WIN32
    if (!hwnd || !hdc) return GVC_ERROR_DISPLAY;
    
    // Convert RGB to BGR for Windows
    uint8_t* bgr_data = malloc(frame->width * frame->height * 3);
    if (!bgr_data) return GVC_ERROR_MEMORY;
    
    for (int i = 0; i < frame->width * frame->height; i++) {
        bgr_data[i * 3] = frame->pixels[i * 3 + 2];     // B
        bgr_data[i * 3 + 1] = frame->pixels[i * 3 + 1]; // G
        bgr_data[i * 3 + 2] = frame->pixels[i * 3];     // R
    }
    
    SetDIBitsToDevice(hdc, 0, 0, frame->width, frame->height,
                     0, 0, 0, frame->height, bgr_data, &bmi, DIB_RGB_COLORS);
    
    free(bgr_data);
    
    // Process Windows messages
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
#endif
    
    return GVC_SUCCESS;
}

int display_should_close(void) {
#ifdef __linux__
    if (!display) return 1;
    
    XEvent event;
    while (XPending(display)) {
        XNextEvent(display, &event);
        if (event.type == KeyPress) {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_Escape || key == XK_q) {
                return 1;
            }
        }
    }
    return 0;
    
#elif __APPLE__
    return should_close;
    
#elif _WIN32
    return should_close;
    
#else
    return 0;
#endif
}

void display_cleanup(void) {
#ifdef __linux__
    if (ximage) {
        XDestroyImage(ximage); // This also frees image_data
        ximage = NULL;
        image_data = NULL;
    }
    if (gc) {
        XFreeGC(display, gc);
        gc = 0;
    }
    if (display) {
        XCloseDisplay(display);
        display = NULL;
    }
    
#elif __APPLE__
    if (bitmapContext) {
        CGContextRelease(bitmapContext);
        bitmapContext = NULL;
    }
    if (bitmapData) {
        free(bitmapData);
        bitmapData = NULL;
    }
    if (imageView) {
        [imageView release];
        imageView = nil;
    }
    if (window) {
        [window close];
        [window release];
        window = nil;
    }
    if (colorSpace) {
        CGColorSpaceRelease(colorSpace);
        colorSpace = NULL;
    }
    
#elif _WIN32
    if (hdc) {
        ReleaseDC(hwnd, hdc);
        hdc = NULL;
    }
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = NULL;
    }
    
#endif
}