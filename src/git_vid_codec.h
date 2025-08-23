#ifndef GIT_VID_CODEC_H
#define GIT_VID_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Frame dimensions and format
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_CHANNELS 3  // RGB
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * FRAME_CHANNELS)
#define TARGET_FPS 60
#define FRAME_TIME_NS (1000000000 / TARGET_FPS)  // 16.67ms in nanoseconds

// Git object limits
#define MAX_GIT_OBJECT_SIZE (100 * 1024 * 1024)  // 100MB
#define GIT_HASH_SIZE 40
#define MAX_COMMIT_MESSAGE 256

// Compression settings
#define COMPRESSION_BLOCK_SIZE 64
#define MAX_DELTA_SIZE (FRAME_SIZE / 2)  // Conservative estimate

// Frame format structures
typedef struct {
    uint32_t frame_number;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t compressed_size;
    uint32_t checksum;
    uint8_t compression_type;  // 0=raw, 1=delta, 2=entropy
    uint8_t reserved[3];
} frame_header_t;

typedef struct {
    frame_header_t header;
    uint8_t* data;
    size_t data_size;
} frame_t;

typedef struct {
    uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
} raw_frame_t;

// Git operations
typedef struct {
    char hash[GIT_HASH_SIZE + 1];
    char message[MAX_COMMIT_MESSAGE];
    time_t timestamp;
} git_commit_t;

// Function prototypes

// compression.c
int compress_frame_delta(const raw_frame_t* current, const raw_frame_t* previous, 
                        frame_t* output);
int decompress_frame_delta(const frame_t* compressed, const raw_frame_t* previous,
                          raw_frame_t* output);
int compress_frame_raw(const raw_frame_t* input, frame_t* output);
int decompress_frame_raw(const frame_t* compressed, raw_frame_t* output);
int decompress_frames_batch(const frame_t* frame1, const frame_t* frame2,
                           const raw_frame_t* previous_frame,
                           raw_frame_t* output1, raw_frame_t* output2);
uint32_t calculate_checksum(const uint8_t* data, size_t size);

// Git operations (legacy)
int git_init_repo(const char* path);
int git_create_blob(const uint8_t* data, size_t size, char* hash_out);
int git_create_commit(const char* blob_hash, const char* message, 
                     const char* parent_hash, char* commit_hash_out);
int git_read_blob(const char* hash, uint8_t** data_out, size_t* size_out);
int git_get_commit_chain(char commits[][GIT_HASH_SIZE + 1], int max_commits);
int git_checkout_commit(const char* commit_hash);
int git_read_frame_from_commit(const char* commit_hash, uint8_t** data_out, size_t* size_out);
int git_show(const char* commit_hash, uint8_t** data_out, size_t* size_out);

// High-performance Git operations using libgit2
int git_init_libgit2(const char* repo_path);
int git_read_blob_libgit2(const char* commit_hash, uint8_t** data_out, size_t* size_out);
int git_get_commit_chain_libgit2(char*** commit_hashes_out, int* num_commits_out);
int git_start_prefetch(char** commit_hashes, int num_commits);
void git_stop_prefetch(void);
void git_cleanup_libgit2(void);

// frame_format.c
int serialize_frame(const frame_t* frame, uint8_t** buffer_out, size_t* size_out);
int deserialize_frame(const uint8_t* buffer, size_t size, frame_t* frame_out);
void free_frame(frame_t* frame);
void free_raw_frame(raw_frame_t* frame);
int copy_raw_frame(const raw_frame_t* src, raw_frame_t* dst);
void generate_frame_filename(uint32_t frame_number, char* filename_out, size_t max_len);
void generate_frame_path(const char* directory, uint32_t frame_number, char* path_out, size_t max_len);
int parse_frame_number_from_filename(const char* filename, uint32_t* frame_number_out);

// display.c (platform-specific)
int display_init(uint32_t width, uint32_t height);
int display_frame(const raw_frame_t* frame);
void display_cleanup(void);
int display_should_close(void);

// encoder.c
int read_raw_frame(const char* filename, raw_frame_t* frame);
int encode_frame_to_commit(const raw_frame_t* current_frame, 
                          const raw_frame_t* previous_frame,
                          uint32_t frame_number,
                          const char* parent_commit_hash,
                          char* commit_hash_out);
int encode_video_sequence(const char* input_path, const char* repo_path);

// mp4_converter.c
int convert_mp4_to_repo(const char* mp4_path, const char* repo_path);

// player.c
int play_from_stdin(void);
int play_from_repo(const char* repo_path);

// Utility macros (avoid conflicts with Foundation framework)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(x, min, max) (MIN(MAX(x, min), max))

// Error codes
#define GVC_SUCCESS 0
#define GVC_ERROR_MEMORY -1
#define GVC_ERROR_IO -2
#define GVC_ERROR_GIT -3
#define GVC_ERROR_COMPRESSION -4
#define GVC_ERROR_FORMAT -5
#define GVC_ERROR_DISPLAY -6
#define GVC_ERROR_THREAD -7

#endif // GIT_VID_CODEC_H