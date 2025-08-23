#include "git_vid_codec.h"

// Magic number to identify our frame format
#define FRAME_MAGIC 0x47564346  // "GVCF" in little endian

int serialize_frame(const frame_t* frame, uint8_t** buffer_out, size_t* size_out) {
    if (!frame || !buffer_out || !size_out) return GVC_ERROR_MEMORY;
    
    // Calculate total size: magic + header + data
    size_t total_size = sizeof(uint32_t) + sizeof(frame_header_t) + frame->data_size;
    
    uint8_t* buffer = malloc(total_size);
    if (!buffer) return GVC_ERROR_MEMORY;
    
    size_t offset = 0;
    
    // Write magic number
    uint32_t magic = FRAME_MAGIC;
    memcpy(buffer + offset, &magic, sizeof(magic));
    offset += sizeof(magic);
    
    // Write header
    memcpy(buffer + offset, &frame->header, sizeof(frame->header));
    offset += sizeof(frame->header);
    
    // Write data
    if (frame->data && frame->data_size > 0) {
        memcpy(buffer + offset, frame->data, frame->data_size);
        offset += frame->data_size;
    }
    
    *buffer_out = buffer;
    *size_out = total_size;
    
    return GVC_SUCCESS;
}

int deserialize_frame(const uint8_t* buffer, size_t size, frame_t* frame_out) {
    if (!buffer || !frame_out || size < sizeof(uint32_t) + sizeof(frame_header_t)) {
        return GVC_ERROR_FORMAT;
    }
    
    size_t offset = 0;
    
    // Read and verify magic number
    uint32_t magic;
    memcpy(&magic, buffer + offset, sizeof(magic));
    offset += sizeof(magic);
    
    if (magic != FRAME_MAGIC) {
        return GVC_ERROR_FORMAT;
    }
    
    // Read header
    memcpy(&frame_out->header, buffer + offset, sizeof(frame_out->header));
    offset += sizeof(frame_out->header);
    
    // Validate header
    if (frame_out->header.width != FRAME_WIDTH ||
        frame_out->header.height != FRAME_HEIGHT ||
        frame_out->header.channels != FRAME_CHANNELS) {
        return GVC_ERROR_FORMAT;
    }
    
    // Check if we have enough data
    if (offset + frame_out->header.compressed_size > size) {
        return GVC_ERROR_FORMAT;
    }
    
    // Read data
    frame_out->data_size = frame_out->header.compressed_size;
    if (frame_out->data_size > 0) {
        frame_out->data = malloc(frame_out->data_size);
        if (!frame_out->data) return GVC_ERROR_MEMORY;
        
        memcpy(frame_out->data, buffer + offset, frame_out->data_size);
        
        // Verify checksum
        uint32_t calculated_checksum = calculate_checksum(frame_out->data, frame_out->data_size);
        if (calculated_checksum != frame_out->header.checksum) {
            free(frame_out->data);
            frame_out->data = NULL;
            return GVC_ERROR_FORMAT;
        }
    } else {
        frame_out->data = NULL;
    }
    
    return GVC_SUCCESS;
}

void free_frame(frame_t* frame) {
    if (frame && frame->data) {
        free(frame->data);
        frame->data = NULL;
        frame->data_size = 0;
    }
}

void free_raw_frame(raw_frame_t* frame) {
    if (frame && frame->pixels) {
        free(frame->pixels);
        frame->pixels = NULL;
    }
}

// Helper function to create a raw frame from RGB data
int create_raw_frame(const uint8_t* rgb_data, uint32_t width, uint32_t height, 
                    raw_frame_t* frame_out) {
    if (!rgb_data || !frame_out) return GVC_ERROR_MEMORY;
    
    size_t pixel_count = width * height * FRAME_CHANNELS;
    
    frame_out->pixels = malloc(pixel_count);
    if (!frame_out->pixels) return GVC_ERROR_MEMORY;
    
    memcpy(frame_out->pixels, rgb_data, pixel_count);
    frame_out->width = width;
    frame_out->height = height;
    frame_out->channels = FRAME_CHANNELS;
    
    return GVC_SUCCESS;
}

// Helper function to validate frame dimensions
int validate_frame_dimensions(uint32_t width, uint32_t height, uint32_t channels) {
    if (width != FRAME_WIDTH || height != FRAME_HEIGHT || channels != FRAME_CHANNELS) {
        return GVC_ERROR_FORMAT;
    }
    return GVC_SUCCESS;
}

// Helper function to copy raw frame
int copy_raw_frame(const raw_frame_t* src, raw_frame_t* dst) {
    if (!src || !dst) return GVC_ERROR_MEMORY;
    
    size_t pixel_count = src->width * src->height * src->channels;
    
    dst->pixels = malloc(pixel_count);
    if (!dst->pixels) return GVC_ERROR_MEMORY;
    
    memcpy(dst->pixels, src->pixels, pixel_count);
    dst->width = src->width;
    dst->height = src->height;
    dst->channels = src->channels;
    
    return GVC_SUCCESS;
}

// Function to estimate compression ratio
double calculate_compression_ratio(size_t original_size, size_t compressed_size) {
    if (compressed_size == 0) return 0.0;
    return (double)original_size / (double)compressed_size;
}

// Function to generate frame filename
void generate_frame_filename(uint32_t frame_number, char* filename_out, size_t max_len) {
    snprintf(filename_out, max_len, "frame_%06u.rgb", frame_number);
}

// Function to generate frame path with directory
void generate_frame_path(const char* directory, uint32_t frame_number, char* path_out, size_t max_len) {
    snprintf(path_out, max_len, "%s/frame_%06u.rgb", directory, frame_number);
}

// Function to parse frame number from filename
int parse_frame_number_from_filename(const char* filename, uint32_t* frame_number_out) {
    if (!filename || !frame_number_out) return GVC_ERROR_MEMORY;
    
    // Expected format: "frame_XXXXXX.rgb"
    if (strncmp(filename, "frame_", 6) != 0) {
        return GVC_ERROR_FORMAT;
    }
    
    char* endptr;
    unsigned long frame_num = strtoul(filename + 6, &endptr, 10);
    
    if (endptr == filename + 6 || strcmp(endptr, ".rgb") != 0) {
        return GVC_ERROR_FORMAT;
    }
    
    *frame_number_out = (uint32_t)frame_num;
    return GVC_SUCCESS;
}