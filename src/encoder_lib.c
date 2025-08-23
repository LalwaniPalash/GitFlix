#include "git_vid_codec.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// Function to read a raw RGB frame from file
int read_raw_frame(const char* filename, raw_frame_t* frame) {
    FILE* file = fopen(filename, "rb");
    if (!file) return GVC_ERROR_IO;
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Verify expected size
    size_t expected_size = FRAME_WIDTH * FRAME_HEIGHT * FRAME_CHANNELS;
    if ((size_t)file_size != expected_size) {
        fclose(file);
        return GVC_ERROR_FORMAT;
    }
    
    frame->pixels = malloc(expected_size);
    if (!frame->pixels) {
        fclose(file);
        return GVC_ERROR_MEMORY;
    }
    
    size_t bytes_read = fread(frame->pixels, 1, expected_size, file);
    fclose(file);
    
    if (bytes_read != expected_size) {
        free(frame->pixels);
        frame->pixels = NULL;
        return GVC_ERROR_IO;
    }
    
    frame->width = FRAME_WIDTH;
    frame->height = FRAME_HEIGHT;
    frame->channels = FRAME_CHANNELS;
    
    return GVC_SUCCESS;
}

// Function to generate test frames (for demonstration)
static int generate_test_frame(uint32_t frame_number, raw_frame_t* frame) {
    frame->pixels = malloc(FRAME_SIZE);
    if (!frame->pixels) return GVC_ERROR_MEMORY;
    
    frame->width = FRAME_WIDTH;
    frame->height = FRAME_HEIGHT;
    frame->channels = FRAME_CHANNELS;
    
    // Generate animated pattern
    for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
            uint32_t pixel_idx = (y * FRAME_WIDTH + x) * FRAME_CHANNELS;
            
            // Create moving gradient pattern
            uint8_t r = (uint8_t)((x + frame_number) % 256);
            uint8_t g = (uint8_t)((y + frame_number / 2) % 256);
            uint8_t b = (uint8_t)((x + y + frame_number) % 256);
            
            frame->pixels[pixel_idx] = r;
            frame->pixels[pixel_idx + 1] = g;
            frame->pixels[pixel_idx + 2] = b;
        }
    }
    
    return GVC_SUCCESS;
}

// Function to encode a single frame and create Git commit
int encode_frame_to_commit(const raw_frame_t* current_frame, 
                          const raw_frame_t* previous_frame,
                          uint32_t frame_number,
                          const char* parent_commit_hash,
                          char* commit_hash_out) {
    frame_t compressed_frame;
    int result;
    
    // Choose compression method
    if (previous_frame) {
        // Use delta compression for non-keyframes
        result = compress_frame_delta(current_frame, previous_frame, &compressed_frame);
    } else {
        // Use raw compression for keyframes
        result = compress_frame_raw(current_frame, &compressed_frame);
    }
    
    if (result != GVC_SUCCESS) return result;
    
    // Set frame number
    compressed_frame.header.frame_number = frame_number;
    
    // Serialize frame to buffer
    uint8_t* frame_buffer;
    size_t frame_buffer_size;
    result = serialize_frame(&compressed_frame, &frame_buffer, &frame_buffer_size);
    
    if (result != GVC_SUCCESS) {
        free_frame(&compressed_frame);
        return result;
    }
    
    // Create Git blob
    char blob_hash[GIT_HASH_SIZE + 1];
    result = git_create_blob(frame_buffer, frame_buffer_size, blob_hash);
    
    if (result != GVC_SUCCESS) {
        free(frame_buffer);
        free_frame(&compressed_frame);
        return result;
    }
    
    // Create commit message
    char commit_message[MAX_COMMIT_MESSAGE];
    snprintf(commit_message, sizeof(commit_message), 
             "Frame %06u (%s, %u bytes)", 
             frame_number,
             compressed_frame.header.compression_type == 0 ? "raw" : "delta",
             compressed_frame.header.compressed_size);
    
    // Create Git commit
    result = git_create_commit(blob_hash, commit_message, parent_commit_hash, commit_hash_out);
    
    // Cleanup
    free(frame_buffer);
    free_frame(&compressed_frame);
    
    if (result == GVC_SUCCESS) {
        printf("Encoded frame %06u: %s compression, %u bytes\n", 
               frame_number,
               compressed_frame.header.compression_type == 0 ? "raw" : "delta",
               compressed_frame.header.compressed_size);
    }
    
    return result;
}

int encode_video_sequence(const char* input_path, const char* repo_path) {
    if (!input_path || !repo_path) return GVC_ERROR_MEMORY;
    
    // Initialize Git repository
    int result = git_init_repo(repo_path);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Failed to initialize Git repository\n");
        return result;
    }
    
    // Change to repository directory
    if (chdir(repo_path) != 0) {
        fprintf(stderr, "Error: Failed to change to repository directory\n");
        return GVC_ERROR_IO;
    }
    
    printf("Encoding video sequence to Git repository: %s\n", repo_path);
    
    raw_frame_t current_frame, previous_frame;
    char current_commit_hash[GIT_HASH_SIZE + 1] = {0};
    char previous_commit_hash[GIT_HASH_SIZE + 1] = {0};
    
    size_t total_original_size = 0;
    // size_t total_compressed_size = 0; // Unused for now
    
    // For demonstration, generate 600 test frames (10 seconds at 60fps)
    const int num_frames = 600;
    
    for (int frame_num = 0; frame_num < num_frames; frame_num++) {
        // Generate or read frame
        if (strcmp(input_path, "test") == 0) {
            // Generate test frames
            result = generate_test_frame(frame_num, &current_frame);
        } else {
            // Read from input files (would need to be implemented based on input format)
            char frame_filename[256];
            generate_frame_path(input_path, frame_num, frame_filename, sizeof(frame_filename));
            result = read_raw_frame(frame_filename, &current_frame);
        }
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Error: Failed to read frame %d\n", frame_num);
            break;
        }
        
        // Encode frame to Git commit
        const char* parent_hash = (frame_num == 0) ? NULL : previous_commit_hash;
        const raw_frame_t* prev_frame = (frame_num == 0) ? NULL : &previous_frame;
        
        result = encode_frame_to_commit(&current_frame, prev_frame, frame_num, 
                                       parent_hash, current_commit_hash);
        
        if (result != GVC_SUCCESS) {
            fprintf(stderr, "Error: Failed to encode frame %d\n", frame_num);
            free_raw_frame(&current_frame);
            break;
        }
        
        // Update statistics
        total_original_size += FRAME_SIZE;
        
        // Free previous frame and update
        if (frame_num > 0) {
            free_raw_frame(&previous_frame);
        }
        
        previous_frame = current_frame;
        strcpy(previous_commit_hash, current_commit_hash);
        
        // Progress indicator
        if (frame_num % 60 == 0) {
            printf("Progress: %d/%d frames (%.1f%%)\n", 
                   frame_num + 1, num_frames, 
                   (float)(frame_num + 1) / num_frames * 100.0f);
        }
    }
    
    // Cleanup last frame
    if (num_frames > 0) {
        free_raw_frame(&previous_frame);
    }
    
    if (result == GVC_SUCCESS) {
        printf("\nEncoding completed successfully!\n");
        printf("Total frames: %d\n", num_frames);
        printf("Original size: %.2f MB\n", total_original_size / (1024.0 * 1024.0));
        printf("\nYou can now play the video with: ./git-vid-play %s\n", repo_path);
    }
    
    return result;
}