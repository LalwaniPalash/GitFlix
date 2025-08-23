#include "git_vid_codec.h"
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

// Global variables for signal handling
static volatile int should_exit = 0;
static volatile int frame_count = 0;
static struct timeval start_time;

// Frame buffer for performance optimization
#define FRAME_BUFFER_SIZE 16
static raw_frame_t frame_buffer[FRAME_BUFFER_SIZE];
static volatile int buffer_read_pos = 0;
static volatile int buffer_write_pos = 0;
static volatile int buffer_count = 0;
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

// Signal handler for graceful exit
void signal_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    should_exit = 1;
}

// High-precision timer functions
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t nanoseconds) {
    struct timespec ts;
    ts.tv_sec = nanoseconds / 1000000000ULL;
    ts.tv_nsec = nanoseconds % 1000000000ULL;
    nanosleep(&ts, NULL);
}

// Frame buffer management functions
static void buffer_put_frame(const raw_frame_t* frame) {
    pthread_mutex_lock(&buffer_mutex);
    
    while (buffer_count >= FRAME_BUFFER_SIZE && !should_exit) {
        pthread_cond_wait(&buffer_not_full, &buffer_mutex);
    }
    
    if (!should_exit) {
        // Deep copy frame data
        frame_buffer[buffer_write_pos] = *frame;
        size_t data_size = frame->width * frame->height * frame->channels;
        frame_buffer[buffer_write_pos].pixels = malloc(data_size);
        memcpy(frame_buffer[buffer_write_pos].pixels, frame->pixels, data_size);
        
        buffer_write_pos = (buffer_write_pos + 1) % FRAME_BUFFER_SIZE;
        buffer_count++;
        pthread_cond_signal(&buffer_not_empty);
    }
    
    pthread_mutex_unlock(&buffer_mutex);
}

static int buffer_get_frame(raw_frame_t* frame) {
    pthread_mutex_lock(&buffer_mutex);
    
    while (buffer_count == 0 && !should_exit) {
        pthread_cond_wait(&buffer_not_empty, &buffer_mutex);
    }
    
    if (should_exit) {
        pthread_mutex_unlock(&buffer_mutex);
        return GVC_ERROR_IO;
    }
    
    *frame = frame_buffer[buffer_read_pos];
    buffer_read_pos = (buffer_read_pos + 1) % FRAME_BUFFER_SIZE;
    buffer_count--;
    pthread_cond_signal(&buffer_not_full);
    
    pthread_mutex_unlock(&buffer_mutex);
    return GVC_SUCCESS;
}

// Optimized decode function without display
static int decode_and_display_frame_buffered(const char* commit_hash, 
                                           const raw_frame_t* previous_frame,
                                           raw_frame_t* current_frame_out) {
    // Read frame data from Git commit
    uint8_t* frame_data;
    size_t frame_data_size;
    
    int result = git_read_frame_from_commit(commit_hash, &frame_data, &frame_data_size);
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    // Deserialize frame
    frame_t compressed_frame;
    result = deserialize_frame(frame_data, frame_data_size, &compressed_frame);
    free(frame_data);
    
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    // Decompress frame
    if (compressed_frame.header.compression_type == 0) {
        // Raw compression
        result = decompress_frame_raw(&compressed_frame, current_frame_out);
    } else if (compressed_frame.header.compression_type == 1) {
        // Delta compression
        if (!previous_frame) {
            free_frame(&compressed_frame);
            return GVC_ERROR_FORMAT;
        }
        result = decompress_frame_delta(&compressed_frame, previous_frame, current_frame_out);
    } else {
        free_frame(&compressed_frame);
        return GVC_ERROR_FORMAT;
    }
    
    free_frame(&compressed_frame);
    return result;
}

// Decoder thread data structure
typedef struct {
    char** commit_hashes;
    int num_commits;
    int current_commit;
    raw_frame_t previous_frame;
    int has_previous_frame;
} decoder_thread_data_t;

// Decoder thread function
static void* decoder_thread(void* arg) {
    decoder_thread_data_t* data = (decoder_thread_data_t*)arg;
    
    while (data->current_commit < data->num_commits && !should_exit) {
        raw_frame_t current_frame;
        const raw_frame_t* prev_frame = data->has_previous_frame ? &data->previous_frame : NULL;
        
        int result = decode_and_display_frame_buffered(data->commit_hashes[data->current_commit], 
                                                      prev_frame, &current_frame);
        
        if (result == GVC_SUCCESS) {
            // Make a copy for the buffer
            raw_frame_t buffer_frame = current_frame;
            size_t data_size = current_frame.width * current_frame.height * current_frame.channels;
            buffer_frame.pixels = malloc(data_size);
            memcpy(buffer_frame.pixels, current_frame.pixels, data_size);
            buffer_put_frame(&buffer_frame);
            
            // Update previous frame - transfer ownership instead of copying
            if (data->has_previous_frame) {
                free(data->previous_frame.pixels);
            }
            
            // Transfer ownership of current frame to previous frame
            data->previous_frame = current_frame;
            data->has_previous_frame = 1;
            
            // Don't free current_frame.pixels - ownership transferred to previous_frame
        }
        
        data->current_commit++;
    }
    
    return NULL;
}

// Function to read short commit hash from stdin
static int read_short_hash(char* hash_out) {
    if (!fgets(hash_out, 64, stdin)) {
        return GVC_ERROR_IO;
    }
    
    // Remove trailing newline
    size_t len = strlen(hash_out);
    if (len > 0 && hash_out[len-1] == '\n') {
        hash_out[len-1] = '\0';
    }
    
    // Validate hash length (accept both short and full hashes)
    len = strlen(hash_out);
    if (len < 7 || len > 40) {
        return GVC_ERROR_FORMAT;
    }
    
    return GVC_SUCCESS;
}

// Function to expand all short hashes to full hashes in batch
static int expand_hashes_batch(char** short_hashes, int num_hashes, char** full_hashes) {
    // Create a single git rev-parse command for all hashes
    char* cmd = malloc(64 + num_hashes * 50); // Conservative size estimate
    strcpy(cmd, "git rev-parse");
    
    for (int i = 0; i < num_hashes; i++) {
        strcat(cmd, " ");
        strcat(cmd, short_hashes[i]);
    }
    
    FILE* fp = popen(cmd, "r");
    free(cmd);
    
    if (!fp) {
        return GVC_ERROR_GIT;
    }
    
    // Read all expanded hashes
    for (int i = 0; i < num_hashes; i++) {
        if (!fgets(full_hashes[i], GIT_HASH_SIZE + 2, fp)) {
            pclose(fp);
            return GVC_ERROR_GIT;
        }
        
        // Remove trailing newline
        size_t len = strlen(full_hashes[i]);
        if (len > 0 && full_hashes[i][len-1] == '\n') {
            full_hashes[i][len-1] = '\0';
        }
        
        // Validate expanded hash length
        if (strlen(full_hashes[i]) != GIT_HASH_SIZE) {
            pclose(fp);
            return GVC_ERROR_FORMAT;
        }
    }
    
    pclose(fp);
    return GVC_SUCCESS;
}

// Function to decode and display a single frame
static int decode_and_display_frame(const char* commit_hash, 
                                   const raw_frame_t* previous_frame,
                                   raw_frame_t* current_frame_out) {
    // Read frame data from Git commit
    uint8_t* frame_data;
    size_t frame_data_size;
    
    int result = git_read_frame_from_commit(commit_hash, &frame_data, &frame_data_size);
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    // Deserialize frame
    frame_t compressed_frame;
    result = deserialize_frame(frame_data, frame_data_size, &compressed_frame);
    free(frame_data);
    
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    // Decompress frame
    if (compressed_frame.header.compression_type == 0) {
        // Raw compression
        result = decompress_frame_raw(&compressed_frame, current_frame_out);
    } else if (compressed_frame.header.compression_type == 1) {
        // Delta compression
        if (!previous_frame) {
            free_frame(&compressed_frame);
            return GVC_ERROR_FORMAT;
        }
        result = decompress_frame_delta(&compressed_frame, previous_frame, current_frame_out);
    } else {
        free_frame(&compressed_frame);
        return GVC_ERROR_FORMAT;
    }
    
    free_frame(&compressed_frame);
    
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    // Display frame
    result = display_frame(current_frame_out);
    if (result != GVC_SUCCESS) {
        free_raw_frame(current_frame_out);
        return result;
    }
    
    return GVC_SUCCESS;
}

// Function to play video from stdin (commit hashes) with multithreaded buffering
int play_from_stdin(void) {
    printf("Git Video Codec Player\n");
    printf("Reading commit hashes from stdin...\n");
    printf("Press ESC or Ctrl+C to exit\n\n");
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize display
    int result = display_init(FRAME_WIDTH, FRAME_HEIGHT);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Failed to initialize display\n");
        return result;
    }
    
    // Read all short commit hashes first
    char** short_hashes = malloc(sizeof(char*) * 1000); // Assume max 1000 frames
    char** commit_hashes = malloc(sizeof(char*) * 1000);
    int num_commits = 0;
    char short_hash[64];
    
    while (read_short_hash(short_hash) == GVC_SUCCESS && num_commits < 1000) {
        short_hashes[num_commits] = malloc(64);
        strcpy(short_hashes[num_commits], short_hash);
        commit_hashes[num_commits] = malloc(GIT_HASH_SIZE + 1);
        num_commits++;
    }
    
    if (num_commits > 0) {
        // Expand all hashes in batch for better performance
        int expand_result = expand_hashes_batch(short_hashes, num_commits, commit_hashes);
        if (expand_result != GVC_SUCCESS) {
            fprintf(stderr, "Error: Failed to expand commit hashes\n");
            for (int i = 0; i < num_commits; i++) {
                free(short_hashes[i]);
                free(commit_hashes[i]);
            }
            free(short_hashes);
            free(commit_hashes);
            display_cleanup();
            return expand_result;
        }
    }
    
    // Free short hashes as we no longer need them
    for (int i = 0; i < num_commits; i++) {
        free(short_hashes[i]);
    }
    free(short_hashes);
    
    if (num_commits == 0) {
        fprintf(stderr, "No frames to play\n");
        free(commit_hashes);
        display_cleanup();
        return GVC_ERROR_IO;
    }
    
    // Initialize decoder thread data
    decoder_thread_data_t decoder_data = {
        .commit_hashes = commit_hashes,
        .num_commits = num_commits,
        .current_commit = 0,
        .has_previous_frame = 0
    };
    
    // Start decoder thread
    pthread_t decoder_tid;
    pthread_create(&decoder_tid, NULL, decoder_thread, &decoder_data);
    
    gettimeofday(&start_time, NULL);
    
    // Main display loop
    while (!should_exit && !display_should_close() && frame_count < num_commits) {
        raw_frame_t frame;
        
        // Get frame from buffer
        if (buffer_get_frame(&frame) != GVC_SUCCESS) {
            break;
        }
        
        // Display frame
        result = display_frame(&frame);
        if (result != GVC_SUCCESS) {
            free(frame.pixels);
            break;
        }
        
        frame_count++;
        
        // Free frame data
        free(frame.pixels);
        
        // No artificial frame rate limiting - run at maximum speed
        
        // Progress indicator
        if (frame_count % 60 == 0) {
            struct timeval current_time;
            gettimeofday(&current_time, NULL);
            
            double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                           (current_time.tv_usec - start_time.tv_usec) / 1000000.0;
            double fps = frame_count / elapsed;
            
            printf("\rFrames: %d, FPS: %.1f, Elapsed: %.1fs", 
                   frame_count, fps, elapsed);
            fflush(stdout);
        }
    }
    
    // Signal decoder thread to stop and wait for it
    should_exit = 1;
    pthread_cond_broadcast(&buffer_not_full);
    pthread_join(decoder_tid, NULL);
    
    // Clean up remaining frames in buffer
    raw_frame_t frame;
    while (buffer_get_frame(&frame) == GVC_SUCCESS) {
        free(frame.pixels);
    }
    
    // Clean up commit hashes
    for (int i = 0; i < num_commits; i++) {
        free(commit_hashes[i]);
    }
    free(commit_hashes);
    
    // Clean up decoder data
    if (decoder_data.has_previous_frame) {
        free(decoder_data.previous_frame.pixels);
    }
    
    display_cleanup();
    
    // Final statistics
    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    double total_elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                          (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    double avg_fps = frame_count / total_elapsed;
    
    printf("\n\nPlayback complete:\n");
    printf("Total frames: %d\n", frame_count);
    printf("Total time: %.2f seconds\n", total_elapsed);
    printf("Average FPS: %.2f\n", avg_fps);
    
    return GVC_SUCCESS;
}

// Function to play video directly from repository
int play_from_repo(const char* repo_path) {
    if (!repo_path) return GVC_ERROR_MEMORY;
    
    // Change to repository directory
    if (chdir(repo_path) != 0) {
        fprintf(stderr, "Error: Failed to change to repository directory: %s\n", repo_path);
        return GVC_ERROR_IO;
    }
    
    // Get commit chain
    char commits[1000][GIT_HASH_SIZE + 1];
    int commit_count = git_get_commit_chain(commits, 1000);
    
    if (commit_count <= 0) {
        fprintf(stderr, "No commits found in repository\n");
        return GVC_ERROR_GIT;
    }
    
    printf("Found %d commits in repository\n", commit_count);
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize display
    int result = display_init(FRAME_WIDTH, FRAME_HEIGHT);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Failed to initialize display\n");
        return result;
    }
    
    gettimeofday(&start_time, NULL);
    
    raw_frame_t current_frame, previous_frame;
    memset(&current_frame, 0, sizeof(current_frame));
    memset(&previous_frame, 0, sizeof(previous_frame));
    
    uint64_t frame_start_time = get_time_ns();
    int has_previous_frame = 0;
    
    for (int i = 0; i < commit_count && !should_exit && !display_should_close(); i++) {
        // Decode and display frame
        const raw_frame_t* prev_frame = has_previous_frame ? &previous_frame : NULL;
        result = decode_and_display_frame(commits[i], prev_frame, &current_frame);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Error: Failed to decode frame from commit %s\n", commits[i]);
            break;
        }
        
        frame_count++;
        
        // Frame timing control
        uint64_t frame_end_time = get_time_ns();
        uint64_t frame_duration = frame_end_time - frame_start_time;
        
        if (frame_duration < FRAME_TIME_NS) {
            sleep_ns(FRAME_TIME_NS - frame_duration);
        }
        
        // Update for next frame
        if (has_previous_frame) {
            free_raw_frame(&previous_frame);
        }
        
        previous_frame = current_frame;
        memset(&current_frame, 0, sizeof(current_frame));
        has_previous_frame = 1;
        
        frame_start_time = get_time_ns();
        
        // Progress indicator
        if (frame_count % 60 == 0) {
            printf("\rFrame %d/%d (%.1f%%)", i + 1, commit_count, 
                   (float)(i + 1) / commit_count * 100.0f);
            fflush(stdout);
        }
    }
    
    // Cleanup
    if (has_previous_frame) {
        free_raw_frame(&previous_frame);
    }
    if (current_frame.pixels) {
        free_raw_frame(&current_frame);
    }
    
    display_cleanup();
    
    printf("\nPlayback complete\n");
    return GVC_SUCCESS;
}

// Main function for player binary
int main(int argc, char* argv[]) {
    if (argc > 2) {
        printf("Usage: %s [repo_path]\n", argv[0]);
        printf("\nIf repo_path is provided, plays directly from repository.\n");
        printf("Otherwise, reads commit hashes from stdin.\n");
        printf("\nExamples:\n");
        printf("  git log --reverse --format=%%H | %s\n", argv[0]);
        printf("  %s ./video_repo\n", argv[0]);
        return 1;
    }
    
    int result;
    
    if (argc == 2) {
        // Play from repository
        result = play_from_repo(argv[1]);
    } else {
        // Play from stdin
        result = play_from_stdin();
    }
    
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Playback failed with error code: %d\n", result);
        return 1;
    }
    
    return 0;
}