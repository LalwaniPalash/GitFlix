#include "git_vid_codec.h"
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>

// Global variables for signal handling
static volatile int should_exit = 0;
static volatile int frame_count = 0;
static struct timeval start_time;

// Lock-free ring buffer for decoded frames
#define RING_BUFFER_SIZE 16
typedef struct {
    raw_frame_t frame;
    atomic_int ready;
} frame_slot_t;

static frame_slot_t frame_ring[RING_BUFFER_SIZE];
static atomic_int write_index = 0;
static atomic_int read_index = 0;
static atomic_int frame_count_atomic = 0;

// Parallel decode/display queues
static dispatch_queue_t decode_queue;
static dispatch_queue_t display_queue;
static dispatch_semaphore_t frame_semaphore;

// Performance monitoring
static uint64_t decode_time_total = 0;
static uint64_t display_time_total = 0;
static int performance_samples = 0;

// Signal handler for graceful exit
void signal_handler(int sig) {
    (void)sig;
    should_exit = 1;
}

// High-precision timer
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Lock-free ring buffer operations
static int ring_put_frame(const raw_frame_t* frame) {
    int current_count = atomic_load(&frame_count_atomic);
    if (current_count >= RING_BUFFER_SIZE) {
        return 0; // Buffer full
    }
    
    int write_idx = atomic_load(&write_index);
    frame_slot_t* slot = &frame_ring[write_idx];
    
    // Check if slot is available
    int expected = 0;
    if (!atomic_compare_exchange_weak(&slot->ready, &expected, 0)) {
        return 0; // Slot busy
    }
    
    // Copy frame data
    slot->frame.width = frame->width;
    slot->frame.height = frame->height;
    slot->frame.channels = frame->channels;
    
    size_t frame_size = frame->width * frame->height * frame->channels;
    if (!slot->frame.pixels) {
        slot->frame.pixels = malloc(frame_size);
    }
    memcpy(slot->frame.pixels, frame->pixels, frame_size);
    
    // Mark as ready
    atomic_store(&slot->ready, 1);
    
    // Advance write index
    atomic_store(&write_index, (write_idx + 1) % RING_BUFFER_SIZE);
    atomic_fetch_add(&frame_count_atomic, 1);
    
    return 1;
}

static int ring_get_frame(raw_frame_t* frame) {
    int current_count = atomic_load(&frame_count_atomic);
    if (current_count <= 0) {
        return 0; // Buffer empty
    }
    
    int read_idx = atomic_load(&read_index);
    frame_slot_t* slot = &frame_ring[read_idx];
    
    // Check if frame is ready
    int expected = 1;
    if (!atomic_compare_exchange_weak(&slot->ready, &expected, 0)) {
        return 0; // Frame not ready
    }
    
    // Copy frame data
    *frame = slot->frame;
    slot->frame.pixels = NULL; // Transfer ownership
    
    // Advance read index
    atomic_store(&read_index, (read_idx + 1) % RING_BUFFER_SIZE);
    atomic_fetch_sub(&frame_count_atomic, 1);
    
    return 1;
}

// High-performance frame decoder
static void decode_frame_async(const char* commit_hash, const raw_frame_t* previous_frame) {
    dispatch_async(decode_queue, ^{
        if (should_exit) return;
        
        uint64_t decode_start = get_time_ns();
        
        // Read compressed frame data using libgit2
        uint8_t* compressed_data;
        size_t compressed_size;
        int result = git_read_blob_libgit2(commit_hash, &compressed_data, &compressed_size);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Failed to read blob %s\n", commit_hash);
            return;
        }
        
        // Deserialize frame
        frame_t compressed_frame;
        result = deserialize_frame(compressed_data, compressed_size, &compressed_frame);
        free(compressed_data);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Failed to deserialize frame %s (error %d)\n", commit_hash, result);
            return;
        }
        
        // Decompress frame
        raw_frame_t decoded_frame;
        if (compressed_frame.header.compression_type == 1) {
            if (previous_frame) {
                // Delta compression with previous frame
                result = decompress_frame_delta(&compressed_frame, previous_frame, &decoded_frame);
                if (result != GVC_SUCCESS) {
                    fprintf(stderr, "Failed to decompress delta frame %s (error %d, type=%d, size=%zu)\n", 
                           commit_hash, result, compressed_frame.header.compression_type, compressed_frame.data_size);
                }
            } else {
                // First frame with delta compression type - treat as raw
                result = decompress_frame_raw(&compressed_frame, &decoded_frame);
                if (result != GVC_SUCCESS) {
                    fprintf(stderr, "Failed to decompress first delta frame as raw %s (error %d, type=%d, size=%zu)\n", 
                           commit_hash, result, compressed_frame.header.compression_type, compressed_frame.data_size);
                }
            }
        } else {
            // Raw compression (type 0)
            result = decompress_frame_raw(&compressed_frame, &decoded_frame);
            if (result != GVC_SUCCESS) {
                fprintf(stderr, "Failed to decompress raw frame %s (error %d, type=%d, size=%zu)\n", 
                       commit_hash, result, compressed_frame.header.compression_type, compressed_frame.data_size);
            }
        }
        
        free_frame(&compressed_frame);
        
        if (result != GVC_SUCCESS) {
            return;
        }
        
        uint64_t decode_end = get_time_ns();
        decode_time_total += (decode_end - decode_start);
        
        // Put frame in ring buffer
        while (!ring_put_frame(&decoded_frame) && !should_exit) {
            usleep(100); // Brief wait if buffer full
        }
        
        // Signal frame available
        dispatch_semaphore_signal(frame_semaphore);
    });
}

// High-performance display loop
static void display_loop(void) {
    dispatch_async(display_queue, ^{
        while (!should_exit && !display_should_close()) {
            // Wait for frame
            dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC); // 16ms timeout
            if (dispatch_semaphore_wait(frame_semaphore, timeout) != 0) {
                continue; // Timeout, check exit condition
            }
            
            if (should_exit) break;
            
            uint64_t display_start = get_time_ns();
            
            // Get frame from ring buffer
            raw_frame_t frame;
            if (!ring_get_frame(&frame)) {
                continue;
            }
            
            // Display frame using Metal
            int result = display_frame(&frame);
            if (result != GVC_SUCCESS) {
                free(frame.pixels);
                break;
            }
            
            uint64_t display_end = get_time_ns();
            display_time_total += (display_end - display_start);
            performance_samples++;
            
            frame_count++;
            free(frame.pixels);
            
            // Performance monitoring
            if (frame_count % 60 == 0) {
                struct timeval current_time;
                gettimeofday(&current_time, NULL);
                
                double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                               (current_time.tv_usec - start_time.tv_usec) / 1000000.0;
                double fps = frame_count / elapsed;
                
                double avg_decode_ms = (decode_time_total / performance_samples) / 1000000.0;
                double avg_display_ms = (display_time_total / performance_samples) / 1000000.0;
                
                printf("\rMetal FPS: %.1f, Decode: %.1fms, Display: %.1fms, Frames: %d", 
                       fps, avg_decode_ms, avg_display_ms, frame_count);
                fflush(stdout);
            }
        }
    });
}

// Metal-optimized playback from repository
int play_from_repo_metal(const char* repo_path) {
    printf("Git Video Codec - Metal Player\n");
    printf("Repository: %s\n", repo_path);
    printf("Press ESC or Ctrl+C to exit\n\n");
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize libgit2
    int result = git_init_libgit2(repo_path);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Failed to initialize libgit2\n");
        return result;
    }
    
    // Get commit chain using libgit2
    char** commit_hashes;
    int num_commits;
    result = git_get_commit_chain_libgit2(&commit_hashes, &num_commits);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Failed to get commit chain\n");
        git_cleanup_libgit2();
        return result;
    }
    
    if (num_commits == 0) {
        fprintf(stderr, "No commits found\n");
        git_cleanup_libgit2();
        return GVC_ERROR_IO;
    }
    
    printf("Found %d frames to play\n", num_commits);
    
    // Start prefetch thread
    git_start_prefetch(commit_hashes, num_commits);
    
    // Initialize Metal display
    result = display_init(FRAME_WIDTH, FRAME_HEIGHT);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Failed to initialize Metal display\n");
        git_cleanup_libgit2();
        return result;
    }
    
    // Initialize ring buffer
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        atomic_init(&frame_ring[i].ready, 0);
        frame_ring[i].frame.pixels = NULL;
    }
    
    // Create dispatch queues
    decode_queue = dispatch_queue_create("decode_queue", 
                                        dispatch_queue_attr_make_with_qos_class(
                                            DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INTERACTIVE, 0));
    display_queue = dispatch_queue_create("display_queue", 
                                         dispatch_queue_attr_make_with_qos_class(
                                             DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    
    frame_semaphore = dispatch_semaphore_create(0);
    
    gettimeofday(&start_time, NULL);
    
    // Start display loop
    display_loop();
    
    // Decode frames with batch optimization when possible
    raw_frame_t previous_frame = {0};
    int has_previous = 0;
    
    for (int i = 0; i < num_commits && !should_exit; i++) {
        uint64_t decode_start = get_time_ns();
        
        // Try batch decompression for two consecutive raw frames
        if (i + 1 < num_commits && !should_exit) {
            // Read both frames
            uint8_t* compressed_data1;
            size_t compressed_size1;
            uint8_t* compressed_data2;
            size_t compressed_size2;
            
            int result1 = git_read_blob_libgit2(commit_hashes[i], &compressed_data1, &compressed_size1);
            int result2 = git_read_blob_libgit2(commit_hashes[i + 1], &compressed_data2, &compressed_size2);
            
            if (result1 == GVC_SUCCESS && result2 == GVC_SUCCESS) {
                // Deserialize both frames
                frame_t compressed_frame1, compressed_frame2;
                int deser1 = deserialize_frame(compressed_data1, compressed_size1, &compressed_frame1);
                int deser2 = deserialize_frame(compressed_data2, compressed_size2, &compressed_frame2);
                
                free(compressed_data1);
                free(compressed_data2);
                
                // Check if both are raw frames (type 0) for batch processing
                if (deser1 == GVC_SUCCESS && deser2 == GVC_SUCCESS &&
                    compressed_frame1.header.compression_type == 0 &&
                    compressed_frame2.header.compression_type == 0) {
                    
                    // Use batch decompression
                    raw_frame_t decoded_frame1, decoded_frame2;
                    int batch_result = decompress_frames_batch(&compressed_frame1, &compressed_frame2,
                                                             NULL, &decoded_frame1, &decoded_frame2);
                    
                    free_frame(&compressed_frame1);
                    free_frame(&compressed_frame2);
                    
                    if (batch_result == GVC_SUCCESS) {
                        uint64_t decode_end = get_time_ns();
                        decode_time_total += (decode_end - decode_start);
                        
                        // Put both frames in ring buffer
                        while (!ring_put_frame(&decoded_frame1) && !should_exit) {
                            usleep(100);
                        }
                        dispatch_semaphore_signal(frame_semaphore);
                        
                        while (!ring_put_frame(&decoded_frame2) && !should_exit) {
                            usleep(100);
                        }
                        dispatch_semaphore_signal(frame_semaphore);
                        
                        // Update previous frame
                        if (has_previous) {
                            free(previous_frame.pixels);
                        }
                        copy_raw_frame(&decoded_frame2, &previous_frame);
                        has_previous = 1;
                        
                        // Skip next iteration since we processed two frames
                        i++;
                        usleep(1000);
                        continue;
                    } else {
                        // Batch failed, fall back to individual processing
                        free(decoded_frame1.pixels);
                        free(decoded_frame2.pixels);
                    }
                } else {
                    // Not suitable for batch, clean up and fall back
                    if (deser1 == GVC_SUCCESS) free_frame(&compressed_frame1);
                    if (deser2 == GVC_SUCCESS) free_frame(&compressed_frame2);
                }
            } else {
                // Failed to read one or both frames, clean up
                if (result1 == GVC_SUCCESS) free(compressed_data1);
                if (result2 == GVC_SUCCESS) free(compressed_data2);
            }
        }
        
        // Single frame processing (fallback or delta frames)
        uint8_t* compressed_data;
        size_t compressed_size;
        int result = git_read_blob_libgit2(commit_hashes[i], &compressed_data, &compressed_size);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Failed to read blob %s\n", commit_hashes[i]);
            continue;
        }
        
        // Deserialize frame
        frame_t compressed_frame;
        result = deserialize_frame(compressed_data, compressed_size, &compressed_frame);
        free(compressed_data);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Failed to deserialize frame %s (error %d)\n", commit_hashes[i], result);
            continue;
        }
        
        // Decompress frame
        raw_frame_t decoded_frame;
        if (compressed_frame.header.compression_type == 1) {
            if (has_previous) {
                // Delta compression with previous frame
                result = decompress_frame_delta(&compressed_frame, &previous_frame, &decoded_frame);
            } else {
                // First frame with delta compression - treat as raw
                result = decompress_frame_raw(&compressed_frame, &decoded_frame);
            }
        } else {
            // Raw compression (type 0)
            result = decompress_frame_raw(&compressed_frame, &decoded_frame);
        }
        
        free_frame(&compressed_frame);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Failed to decompress frame %s (error %d)\n", commit_hashes[i], result);
            continue;
        }
        
        uint64_t decode_end = get_time_ns();
        decode_time_total += (decode_end - decode_start);
        
        // Put frame in ring buffer
        while (!ring_put_frame(&decoded_frame) && !should_exit) {
            usleep(100); // Brief wait if buffer full
        }
        
        // Signal frame available
        dispatch_semaphore_signal(frame_semaphore);
        
        // Update previous frame for next iteration
        if (has_previous) {
            free(previous_frame.pixels);
        }
        copy_raw_frame(&decoded_frame, &previous_frame);
         has_previous = 1;
        
        // Throttle decode rate to prevent overwhelming the system
        usleep(1000); // 1ms delay
    }
    
    // Wait for display to finish
    while (!should_exit && frame_count < num_commits) {
        usleep(10000); // 10ms
    }
    
    // Cleanup
    should_exit = 1;
    dispatch_semaphore_signal(frame_semaphore); // Wake up display thread
    
    // Wait a bit for threads to finish
    usleep(100000); // 100ms
    
    // Clean up ring buffer
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        if (frame_ring[i].frame.pixels) {
            free(frame_ring[i].frame.pixels);
        }
    }
    
    // Clean up commit hashes
    for (int i = 0; i < num_commits; i++) {
        free(commit_hashes[i]);
    }
    free(commit_hashes);
    
    if (has_previous) {
        free(previous_frame.pixels);
    }
    
    display_cleanup();
    git_cleanup_libgit2();
    
    // Final statistics
    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    double total_elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                          (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    double avg_fps = frame_count / total_elapsed;
    
    printf("\n\nMetal Playback Complete:\n");
    printf("Total frames: %d\n", frame_count);
    printf("Total time: %.2f seconds\n", total_elapsed);
    printf("Average FPS: %.2f\n", avg_fps);
    
    if (performance_samples > 0) {
        double avg_decode_ms = (decode_time_total / performance_samples) / 1000000.0;
        double avg_display_ms = (display_time_total / performance_samples) / 1000000.0;
        printf("Average decode time: %.2f ms\n", avg_decode_ms);
        printf("Average display time: %.2f ms\n", avg_display_ms);
    }
    
    return GVC_SUCCESS;
}

// Main function for Metal player
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <git_repository_path>\n", argv[0]);
        return 1;
    }
    
    const char* repo_path = argv[1];
    
    int result = play_from_repo_metal(repo_path);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Playback failed with error code: %d\n", result);
        return 1;
    }
    
    return 0;
}