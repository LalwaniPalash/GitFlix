#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>
#include "git_vid_codec.h"
#include <dispatch/dispatch.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <stdatomic.h>

// Double-buffer configuration for tighter synchronization
#define NUM_BUFFERS 2
#define RING_BUFFER_SIZE 16

// Metal rendering state
static id<MTLDevice> device;
static id<MTLCommandQueue> commandQueue;
static id<MTLRenderPipelineState> pipelineState;
static id<MTLTexture> textures[NUM_BUFFERS];
static CVMetalTextureCacheRef textureCache;
static CAMetalLayer* metalLayer;
static NSWindow* window;
static MTKView* metalView;

// Triple-buffer synchronization
static semaphore_t bufferSemaphore;
static atomic_int currentWriteBuffer = 0;
static atomic_int currentDisplayBuffer = 0;

// Lock-free ring buffer for decoded frames
typedef struct {
    uint8_t* pixels;
    int ready;
} frame_slot_t;

static frame_slot_t frame_ring[RING_BUFFER_SIZE];
static atomic_int write_index = 0;
static atomic_int read_index = 0;
static atomic_int frame_count = 0;

// Parallel decode thread
static dispatch_queue_t decodeQueue;
static dispatch_queue_t displayQueue;
static volatile int shouldExit = 0;

// Performance monitoring
static uint64_t frameStartTime;
static int totalFrames = 0;

// Metal shader source (combined to avoid struct duplication)
static const char* shaderSource = 
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct VertexOut {\n"
"    float4 position [[position]];\n"
"    float2 texCoord;\n"
"};\n"
"\n"
"vertex VertexOut vertex_main(uint vertexID [[vertex_id]]) {\n"
"    VertexOut out;\n"
"    float2 positions[4] = {\n"
"        float2(-1.0, -1.0),\n"
"        float2( 1.0, -1.0),\n"
"        float2(-1.0,  1.0),\n"
"        float2( 1.0,  1.0)\n"
"    };\n"
"    float2 texCoords[4] = {\n"
"        float2(0.0, 1.0),\n"
"        float2(1.0, 1.0),\n"
"        float2(0.0, 0.0),\n"
"        float2(1.0, 0.0)\n"
"    };\n"
"    out.position = float4(positions[vertexID], 0.0, 1.0);\n"
"    out.texCoord = texCoords[vertexID];\n"
"    return out;\n"
"}\n"
"\n"
"fragment float4 fragment_main(VertexOut in [[stage_in]],\n"
"                             texture2d<float> colorTexture [[texture(0)]]) {\n"
"    constexpr sampler textureSampler(mag_filter::linear, min_filter::linear);\n"
"    return colorTexture.sample(textureSampler, in.texCoord);\n"
"}\n";

// Utility functions
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Lock-free ring buffer operations
static int ring_buffer_put_frame(uint8_t* pixels) {
    int current_count = atomic_load(&frame_count);
    if (current_count >= RING_BUFFER_SIZE) {
        return -1; // Buffer full
    }
    
    int write_idx = atomic_load(&write_index);
    frame_slot_t* slot = &frame_ring[write_idx];
    
    if (slot->ready) {
        return 0; // Slot still in use
    }
    
    // Copy frame data
    memcpy(slot->pixels, pixels, FRAME_WIDTH * FRAME_HEIGHT * 4); // RGBA
    slot->ready = 1;
    
    // Advance write index
    atomic_store(&write_index, (write_idx + 1) % RING_BUFFER_SIZE);
    atomic_fetch_add(&frame_count, 1);
    
    return 1;
}

static uint8_t* ring_buffer_get_frame(void) {
    int current_count = atomic_load(&frame_count);
    if (current_count == 0) {
        return NULL; // Buffer empty
    }
    
    int read_idx = atomic_load(&read_index);
    frame_slot_t* slot = &frame_ring[read_idx];
    
    if (!slot->ready) {
        return NULL; // Frame not ready
    }
    
    return slot->pixels;
}

static void ring_buffer_consume_frame(void) {
    int read_idx = atomic_load(&read_index);
    frame_slot_t* slot = &frame_ring[read_idx];
    
    slot->ready = 0;
    atomic_store(&read_index, (read_idx + 1) % RING_BUFFER_SIZE);
    atomic_fetch_sub(&frame_count, 1);
}

// Metal setup functions
static int setup_metal_pipeline(void) {
    device = MTLCreateSystemDefaultDevice();
    if (!device) {
        fprintf(stderr, "Failed to create Metal device\n");
        return GVC_ERROR_DISPLAY;
    }
    
    commandQueue = [device newCommandQueue];
    if (!commandQueue) {
        fprintf(stderr, "Failed to create Metal command queue\n");
        return GVC_ERROR_DISPLAY;
    }
    
    // Create shader library
    NSString* source = [NSString stringWithUTF8String:shaderSource];
    
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source
        options:nil error:&error];
    
    if (!library) {
        fprintf(stderr, "Failed to create Metal library: %s\n", 
                [[error localizedDescription] UTF8String]);
        return GVC_ERROR_DISPLAY;
    }
    
    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    
    // Create render pipeline
    MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    
    pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    if (!pipelineState) {
        fprintf(stderr, "Failed to create Metal pipeline state: %s\n",
                [[error localizedDescription] UTF8String]);
        return GVC_ERROR_DISPLAY;
    }
    
    return GVC_SUCCESS;
}

// Metal buffers for zero-copy unified memory access
static id<MTLBuffer> frame_buffers[NUM_BUFFERS];
static id<MTLBlitCommandEncoder> blitEncoder;
static MTLRenderPassDescriptor* renderPassDescriptor;

static uint8_t* texturePointers[NUM_BUFFERS];

static int create_metal_textures(void) {
    // Create shared buffers for direct CPU write, GPU read
    size_t buffer_size = FRAME_WIDTH * FRAME_HEIGHT * 4; // RGBA
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffers[i] = [device newBufferWithLength:buffer_size
                                              options:MTLResourceStorageModeShared];
        if (!frame_buffers[i]) {
            fprintf(stderr, "Failed to create Metal buffer %d\n", i);
            return GVC_ERROR_DISPLAY;
        }
        
        // Map buffer memory for direct CPU access (zero-copy)
        texturePointers[i] = (uint8_t*)[frame_buffers[i] contents];
        if (!texturePointers[i]) {
            fprintf(stderr, "Failed to map buffer memory %d\n", i);
            return GVC_ERROR_DISPLAY;
        }
    }
    
    // Create textures from buffers for GPU rendering
    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor new];
    textureDescriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    textureDescriptor.width = FRAME_WIDTH;
    textureDescriptor.height = FRAME_HEIGHT;
    textureDescriptor.usage = MTLTextureUsageShaderRead;
    textureDescriptor.storageMode = MTLStorageModeShared;
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        textures[i] = [frame_buffers[i] newTextureWithDescriptor:textureDescriptor
                                                         offset:0
                                                    bytesPerRow:FRAME_WIDTH * 4];
        if (!textures[i]) {
            fprintf(stderr, "Failed to create Metal texture %d\n", i);
            return GVC_ERROR_DISPLAY;
        }
    }
    
    return GVC_SUCCESS;
}

// Display initialization
int display_init(uint32_t width, uint32_t height) {
    if (width != FRAME_WIDTH || height != FRAME_HEIGHT) {
        fprintf(stderr, "Unsupported resolution: %dx%d\n", width, height);
        return GVC_ERROR_DISPLAY;
    }
    
    // Initialize ring buffer
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        frame_ring[i].pixels = malloc(FRAME_WIDTH * FRAME_HEIGHT * 4); // RGBA
        frame_ring[i].ready = 0;
        if (!frame_ring[i].pixels) {
            fprintf(stderr, "Failed to allocate ring buffer slot %d\n", i);
            return GVC_ERROR_MEMORY;
        }
    }
    
    // Pre-allocate render pass descriptor for performance
    renderPassDescriptor = [MTLRenderPassDescriptor new];
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    
    // Create semaphore for double-buffering (tighter synchronization)
    kern_return_t result = semaphore_create(mach_task_self(), &bufferSemaphore, 
                                          SYNC_POLICY_FIFO, NUM_BUFFERS);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Failed to create buffer semaphore\n");
        return GVC_ERROR_DISPLAY;
    }
    
    // Setup Metal pipeline
    if (setup_metal_pipeline() != GVC_SUCCESS) {
        return GVC_ERROR_DISPLAY;
    }
    
    // Create Metal textures
    if (create_metal_textures() != GVC_SUCCESS) {
        return GVC_ERROR_DISPLAY;
    }
    
    // Initialize Cocoa application
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    
    // Create window and Metal view
    NSRect windowRect = NSMakeRect(0, 0, width, height);
    window = [[NSWindow alloc] initWithContentRect:windowRect
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setTitle:@"Git Video Codec - Metal Player"];
    [window center]; // Center the window on screen
    
    metalView = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, width, height) device:device];
    metalView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    metalView.framebufferOnly = YES;
    metalView.enableSetNeedsDisplay = NO;
    metalView.paused = YES; // We'll control rendering manually
    
    [window setContentView:metalView];
    
    // Ensure window is visible and brought to front
    [app finishLaunching];
    [window makeKeyAndOrderFront:nil];
    [window orderFrontRegardless];
    [app activateIgnoringOtherApps:YES];
    [app unhide:nil];
    
    // Force window to be visible with minimal event processing
    [window setLevel:NSFloatingWindowLevel];
    [window display]; // Force immediate display
    [window setLevel:NSNormalWindowLevel];
    
    // Process one event to ensure window appears
    NSEvent *event = [app nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES];
    if (event) {
        [app sendEvent:event];
    }
    
    // Create dispatch queues
    decodeQueue = dispatch_queue_create("decode_queue", 
                                       dispatch_queue_attr_make_with_qos_class(
                                           DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    displayQueue = dispatch_queue_create("display_queue", 
                                        dispatch_queue_attr_make_with_qos_class(
                                            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    
    frameStartTime = get_time_ns();
    shouldExit = 0;
    
    printf("Metal display initialized: %dx%d\n", width, height);
    return GVC_SUCCESS;
}

// Direct memory write to shared buffer (zero-copy)
static void write_frame_to_texture(uint8_t* rgbData, int bufferIndex) {
    // Write directly to mapped buffer memory - no CPU copy!
    uint8_t* dst = texturePointers[bufferIndex];
    uint8_t* src = rgbData;
    
    // Fast RGB to RGBA conversion directly into buffer memory
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
        *dst++ = *src++; // R
        *dst++ = *src++; // G
        *dst++ = *src++; // B
        *dst++ = 255;    // A
    }
}

// Optimized Metal rendering with minimal object allocation
static void render_frame(int bufferIndex) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [((CAMetalLayer*)metalView.layer) nextDrawable];
        if (!drawable) return;
        
        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
        if (!commandBuffer) return;
        
        // Reuse pre-allocated descriptor, just update texture
        renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
        
        id<MTLRenderCommandEncoder> renderEncoder = 
            [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
        if (!renderEncoder) {
            [commandBuffer commit];
            return;
        }
        
        [renderEncoder setRenderPipelineState:pipelineState];
        [renderEncoder setFragmentTexture:textures[bufferIndex] atIndex:0];
        [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [renderEncoder endEncoding];
        
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
        // Async execution for maximum performance
    }
}

// High-performance frame display with zero-copy
int display_frame(const raw_frame_t* frame) {
    if (!frame || !frame->pixels) {
        return GVC_ERROR_MEMORY;
    }
    
    // Wait for available buffer (double-buffering)
    semaphore_wait(bufferSemaphore);
    
    int writeBuffer = atomic_load(&currentWriteBuffer);
    
    // Write directly to mapped texture memory (zero-copy!)
    write_frame_to_texture(frame->pixels, writeBuffer);
    
    // Render frame
    render_frame(writeBuffer);
    
    // Advance to next buffer
    atomic_store(&currentWriteBuffer, (writeBuffer + 1) % NUM_BUFFERS);
    
    semaphore_signal(bufferSemaphore);
    
    totalFrames++;
    
    // Performance monitoring
    if (totalFrames % 60 == 0) {
        uint64_t currentTime = get_time_ns();
        double elapsed = (currentTime - frameStartTime) / 1000000000.0;
        double fps = totalFrames / elapsed;
        printf("\rMetal FPS: %.1f, Frames: %d, Elapsed: %.1fs", 
               fps, totalFrames, elapsed);
        fflush(stdout);
    }
    
    return GVC_SUCCESS;
}

// Check if window should close
int display_should_close(void) {
    return shouldExit || ![window isVisible];
}

// Cleanup
void display_cleanup(void) {
    shouldExit = 1;
    
    // Clean up ring buffer
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        if (frame_ring[i].pixels) {
            free(frame_ring[i].pixels);
            frame_ring[i].pixels = NULL;
        }
    }
    
    // Clean up Metal resources
    for (int i = 0; i < NUM_BUFFERS; i++) {
        textures[i] = nil;
    }
    
    pipelineState = nil;
    commandQueue = nil;
    device = nil;
    
    if (textureCache) {
        CFRelease(textureCache);
        textureCache = NULL;
    }
    
    semaphore_destroy(mach_task_self(), bufferSemaphore);
    
    [window close];
    window = nil;
    metalView = nil;
    
    printf("\nMetal display cleanup complete\n");
}