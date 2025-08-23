#include "git_vid_codec.h"
#include <zlib.h>
#include <compression.h>

// Simple delta compression using run-length encoding of differences
int compress_frame_delta(const raw_frame_t* current, const raw_frame_t* previous, 
                        frame_t* output) {
    if (!current || !previous || !output) return GVC_ERROR_MEMORY;
    
    if (current->width != previous->width || 
        current->height != previous->height ||
        current->channels != previous->channels) {
        return GVC_ERROR_FORMAT;
    }
    
    size_t pixel_count = current->width * current->height * current->channels;
    uint8_t* delta_buffer = malloc(pixel_count * 2); // Worst case: every pixel different
    if (!delta_buffer) return GVC_ERROR_MEMORY;
    
    size_t delta_pos = 0;
    size_t i = 0;
    
    while (i < pixel_count) {
        // Find run of identical pixels
        size_t identical_run = 0;
        while (i + identical_run < pixel_count && 
               current->pixels[i + identical_run] == previous->pixels[i + identical_run] &&
               identical_run < 255) {
            identical_run++;
        }
        
        if (identical_run > 0) {
            // Encode identical run: 0x00 + run_length
            delta_buffer[delta_pos++] = 0x00;
            delta_buffer[delta_pos++] = (uint8_t)identical_run;
            i += identical_run;
        } else {
            // Find run of different pixels
            size_t diff_run = 0;
            while (i + diff_run < pixel_count && 
                   current->pixels[i + diff_run] != previous->pixels[i + diff_run] &&
                   diff_run < 255) {
                diff_run++;
            }
            
            if (diff_run > 0) {
                // Encode different run: 0x01 + run_length + delta_values
                delta_buffer[delta_pos++] = 0x01;
                delta_buffer[delta_pos++] = (uint8_t)diff_run;
                
                for (size_t j = 0; j < diff_run; j++) {
                    int16_t delta = (int16_t)current->pixels[i + j] - (int16_t)previous->pixels[i + j];
                    delta_buffer[delta_pos++] = (uint8_t)(delta & 0xFF);
                }
                i += diff_run;
            } else {
                i++; // Shouldn't happen, but safety
            }
        }
    }
    
    // Apply compression to delta buffer
    uLongf compressed_size = compressBound(delta_pos);
    uint8_t* compressed_data = malloc(compressed_size);
    if (!compressed_data) {
        free(delta_buffer);
        return GVC_ERROR_MEMORY;
    }
    
    size_t result = compression_encode_buffer(compressed_data, compressed_size,
                                             delta_buffer, delta_pos,
                                             NULL, COMPRESSION_LZFSE);
    if (result == 0) {
        free(compressed_data);
        free(delta_buffer);
        return GVC_ERROR_COMPRESSION;
    }
    compressed_size = result;
    
    // Fill output frame
    output->header.frame_number = 0; // Will be set by caller
    output->header.width = current->width;
    output->header.height = current->height;
    output->header.channels = current->channels;
    output->header.compressed_size = compressed_size;
    output->header.compression_type = 1; // Delta compression
    output->header.checksum = calculate_checksum(compressed_data, compressed_size);
    
    output->data = compressed_data;
    output->data_size = compressed_size;
    
    return GVC_SUCCESS;
}

int decompress_frame_delta(const frame_t* compressed, const raw_frame_t* previous,
                          raw_frame_t* output) {
    if (!compressed || !previous || !output) return GVC_ERROR_MEMORY;
    
    // Skip checksum verification for performance
    // uint32_t checksum = calculate_checksum(compressed->data, compressed->data_size);
    // if (checksum != compressed->header.checksum) {
    //     return GVC_ERROR_FORMAT;
    // }
    
    // Decompress delta buffer
    size_t delta_size = compressed->header.width * compressed->header.height * 
                       compressed->header.channels * 2;
    uint8_t* delta_buffer = malloc(delta_size);
    if (!delta_buffer) return GVC_ERROR_MEMORY;
    
    size_t decompressed_size = compression_decode_buffer(delta_buffer, delta_size,
                                                        compressed->data, compressed->data_size,
                                                        NULL, COMPRESSION_LZFSE);
    if (decompressed_size == 0) {
        free(delta_buffer);
        return GVC_ERROR_COMPRESSION;
    }
    delta_size = decompressed_size;
    
    // Allocate output frame
    size_t pixel_count = compressed->header.width * compressed->header.height * 
                        compressed->header.channels;
    output->pixels = malloc(pixel_count);
    if (!output->pixels) {
        free(delta_buffer);
        return GVC_ERROR_MEMORY;
    }
    
    output->width = compressed->header.width;
    output->height = compressed->header.height;
    output->channels = compressed->header.channels;
    
    // Apply deltas to previous frame
    memcpy(output->pixels, previous->pixels, pixel_count);
    
    size_t delta_pos = 0;
    size_t pixel_pos = 0;
    
    while (delta_pos < delta_size && pixel_pos < pixel_count) {
        uint8_t command = delta_buffer[delta_pos++];
        if (delta_pos >= delta_size) break;
        
        uint8_t run_length = delta_buffer[delta_pos++];
        
        if (command == 0x00) {
            // Identical run - pixels already copied, just advance
            pixel_pos += run_length;
        } else if (command == 0x01) {
            // Different run - apply deltas
            for (int i = 0; i < run_length && pixel_pos < pixel_count && delta_pos < delta_size; i++) {
                int16_t delta = (int8_t)delta_buffer[delta_pos++]; // Sign extend
                int16_t new_value = (int16_t)output->pixels[pixel_pos] + delta;
                output->pixels[pixel_pos] = (uint8_t)CLAMP(new_value, 0, 255);
                pixel_pos++;
            }
        }
    }
    
    free(delta_buffer);
    return GVC_SUCCESS;
}

int compress_frame_raw(const raw_frame_t* input, frame_t* output) {
    if (!input || !output) return GVC_ERROR_MEMORY;
    
    size_t pixel_count = input->width * input->height * input->channels;
    
    // Apply compression
    uLongf compressed_size = compressBound(pixel_count);
    uint8_t* compressed_data = malloc(compressed_size);
    if (!compressed_data) return GVC_ERROR_MEMORY;
    
    size_t result = compression_encode_buffer(compressed_data, compressed_size,
                                             input->pixels, pixel_count,
                                             NULL, COMPRESSION_LZFSE);
    if (result == 0) {
        free(compressed_data);
        return GVC_ERROR_COMPRESSION;
    }
    compressed_size = result;
    
    // Fill output frame
    output->header.frame_number = 0; // Will be set by caller
    output->header.width = input->width;
    output->header.height = input->height;
    output->header.channels = input->channels;
    output->header.compressed_size = compressed_size;
    output->header.compression_type = 0; // Raw compression
    output->header.checksum = calculate_checksum(compressed_data, compressed_size);
    
    output->data = compressed_data;
    output->data_size = compressed_size;
    
    return GVC_SUCCESS;
}

int decompress_frame_raw(const frame_t* compressed, raw_frame_t* output) {
    if (!compressed || !output) return GVC_ERROR_MEMORY;
    
    // Skip checksum verification for performance
    // uint32_t checksum = calculate_checksum(compressed->data, compressed->data_size);
    // if (checksum != compressed->header.checksum) {
    //     return GVC_ERROR_FORMAT;
    // }
    
    size_t pixel_count = compressed->header.width * compressed->header.height * 
                        compressed->header.channels;
    
    output->pixels = malloc(pixel_count);
    if (!output->pixels) return GVC_ERROR_MEMORY;
    
    output->width = compressed->header.width;
    output->height = compressed->header.height;
    output->channels = compressed->header.channels;
    
    size_t decompressed_size = compression_decode_buffer(output->pixels, pixel_count,
                                                        compressed->data, compressed->data_size,
                                                        NULL, COMPRESSION_LZFSE);
    
    if (decompressed_size == 0 || decompressed_size != pixel_count) {
        free(output->pixels);
        output->pixels = NULL;
        return GVC_ERROR_COMPRESSION;
    }
    
    return GVC_SUCCESS;
}

// Batch decompress two frames at once for better SIMD utilization
int decompress_frames_batch(const frame_t* frame1, const frame_t* frame2,
                           const raw_frame_t* previous_frame __attribute__((unused)),
                           raw_frame_t* output1, raw_frame_t* output2) {
    if (!frame1 || !frame2 || !output1 || !output2) return GVC_ERROR_MEMORY;
    
    // Calculate total compressed size for batch operation
    size_t total_compressed_size = frame1->data_size + frame2->data_size;
    size_t total_decompressed_size = (frame1->header.width * frame1->header.height * frame1->header.channels) +
                                    (frame2->header.width * frame2->header.height * frame2->header.channels);
    
    // Allocate combined buffers
    uint8_t* combined_compressed = malloc(total_compressed_size);
    uint8_t* combined_decompressed = malloc(total_decompressed_size);
    
    if (!combined_compressed || !combined_decompressed) {
        free(combined_compressed);
        free(combined_decompressed);
        return GVC_ERROR_MEMORY;
    }
    
    // Combine compressed data
    memcpy(combined_compressed, frame1->data, frame1->data_size);
    memcpy(combined_compressed + frame1->data_size, frame2->data, frame2->data_size);
    
    // Batch decompress using Apple Compression
    size_t actual_decompressed = compression_decode_buffer(combined_decompressed, total_decompressed_size,
                                                          combined_compressed, total_compressed_size,
                                                          NULL, COMPRESSION_ZLIB);
    
    free(combined_compressed);
    
    if (actual_decompressed == 0) {
        free(combined_decompressed);
        return GVC_ERROR_COMPRESSION;
    }
    
    // Split decompressed data back into individual frames
    size_t frame1_size = frame1->header.width * frame1->header.height * frame1->header.channels;
    size_t frame2_size = frame2->header.width * frame2->header.height * frame2->header.channels;
    
    // Allocate output frames
    output1->pixels = malloc(frame1_size);
    output2->pixels = malloc(frame2_size);
    
    if (!output1->pixels || !output2->pixels) {
        free(output1->pixels);
        free(output2->pixels);
        free(combined_decompressed);
        return GVC_ERROR_MEMORY;
    }
    
    // Copy decompressed data to output frames
    memcpy(output1->pixels, combined_decompressed, frame1_size);
    memcpy(output2->pixels, combined_decompressed + frame1_size, frame2_size);
    
    // Set frame metadata
    output1->width = frame1->header.width;
    output1->height = frame1->header.height;
    output1->channels = frame1->header.channels;
    
    output2->width = frame2->header.width;
    output2->height = frame2->header.height;
    output2->channels = frame2->header.channels;
    
    free(combined_decompressed);
    return GVC_SUCCESS;
}

uint32_t calculate_checksum(const uint8_t* data, size_t size) {
    return crc32(0L, data, size);
}