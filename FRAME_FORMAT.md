# Git Video Codec Frame Format Specification

This document describes the binary format used to store video frames as Git blobs in the Git Video Codec system.

## Overview

Each video frame is stored as a binary blob within a Git commit. The blob contains a header followed by compressed frame data. The format is designed to be:

- **Compact**: Minimal overhead for metadata
- **Verifiable**: Built-in checksums for data integrity
- **Extensible**: Reserved fields for future enhancements
- **Git-friendly**: Respects Git's 100MB object size limit

## Binary Layout

```
+------------------+
| Magic Number (4) |
+------------------+
| Frame Header(28) |
+------------------+
| Compressed Data  |
| (variable size)  |
+------------------+
```

## Magic Number

**Offset**: 0  
**Size**: 4 bytes  
**Type**: uint32_t (little-endian)  
**Value**: `0x47564346` ("GVCF" in ASCII)

Used to identify and validate the frame format.

## Frame Header

**Offset**: 4  
**Size**: 28 bytes  
**Alignment**: 4-byte aligned

```c
struct frame_header {
    uint32_t frame_number;    // Sequential frame number (0-based)
    uint32_t width;           // Frame width in pixels (1920)
    uint32_t height;          // Frame height in pixels (1080)
    uint32_t channels;        // Color channels (3 for RGB)
    uint32_t compressed_size; // Size of compressed data in bytes
    uint32_t checksum;        // CRC32 of compressed data
    uint8_t  compression_type;// Compression algorithm used
    uint8_t  reserved[3];     // Reserved for future use (must be 0)
};
```

### Field Descriptions

#### frame_number
- **Range**: 0 to 4,294,967,295
- **Purpose**: Sequential frame identifier for ordering and seeking
- **Notes**: Must be consecutive within a video sequence

#### width, height
- **Current values**: 1920 × 1080 (fixed)
- **Purpose**: Frame dimensions for validation
- **Future**: May support variable resolutions

#### channels
- **Current value**: 3 (RGB)
- **Purpose**: Color format specification
- **Future**: May support RGBA (4) or other formats

#### compressed_size
- **Range**: 1 to 100,000,000 bytes
- **Purpose**: Size of the compressed data following the header
- **Validation**: Must not exceed Git's object size limit

#### checksum
- **Algorithm**: CRC32 (zlib implementation)
- **Purpose**: Data integrity verification
- **Scope**: Covers only the compressed data, not the header

#### compression_type
- **0**: Raw compression (LZFSE)
- **1**: Delta compression (RLE + LZFSE)
- **2**: Raw compression (zlib fallback)
- **3-255**: Reserved for future algorithms

#### reserved
- **Value**: Must be zero
- **Purpose**: Future extensions (e.g., quality settings, metadata)

## Compression Formats

### Type 0: Raw Compression

Direct LZFSE compression of RGB pixel data.

**Input**: Raw RGB pixels (width × height × 3 bytes)  
**Process**: `compression_encode_buffer(rgb_data, size, NULL, 0, NULL, COMPRESSION_LZFSE)`  
**Output**: Compressed data

**Note**: LZFSE provides better compression ratios than zlib while maintaining fast decompression speeds.

### Type 1: Delta Compression

Run-length encoding of pixel differences followed by zlib compression.

**Algorithm**:
1. Compare current frame with previous frame pixel-by-pixel
2. Encode runs of identical/different pixels:
   - `0x00 + length`: Run of identical pixels
   - `0x01 + length + deltas`: Run of different pixels with delta values
3. Apply LZFSE compression to the encoded delta stream

**Delta encoding**:
- Deltas are signed 8-bit values (-128 to +127)
- Clamped to valid RGB range (0-255) during decoding

## Size Constraints

- **Maximum blob size**: 100 MB (Git limit)
- **Typical frame size**: 50KB - 2MB compressed
- **Header overhead**: 32 bytes (negligible)
- **Minimum compressed size**: 1 byte (theoretical)

## Validation Rules

1. **Magic number** must be `0x47564346`
2. **Dimensions** must match expected values (1920×1080×3)
3. **Compressed size** must be > 0 and ≤ remaining blob size
4. **Checksum** must match CRC32 of compressed data
5. **Compression type** must be valid (0-1 currently)
6. **Reserved fields** must be zero
7. **Frame number** should be sequential (warning if not)

## Error Handling

The codec uses standardized error codes and messages:

- **Invalid magic**: Return `GVC_ERROR_FORMAT`
- **Dimension mismatch**: Return `GVC_ERROR_FORMAT`
- **Checksum failure**: Return `GVC_ERROR_FORMAT`
- **Decompression error**: Return `GVC_ERROR_COMPRESSION`
- **Memory allocation**: Return `GVC_ERROR_MEMORY`
- **I/O operations**: Return `GVC_ERROR_IO`
- **Git operations**: Return `GVC_ERROR_GIT`

All error messages are prefixed with "Error: " for consistency across the codebase.

## Future Extensions

### Planned Features
- **Audio tracks**: Additional blob types for audio data
- **Variable resolution**: Support for different frame sizes
- **Quality levels**: Lossy compression options
- **Metadata**: Timestamps, color profiles, etc.

### Reserved Space
- **3 bytes** in header for flags and parameters
- **Compression types 2-255** for new algorithms
- **Magic number variants** for format versions

## Implementation Notes

### Endianness
All multi-byte integers use little-endian byte order for cross-platform compatibility.

### Alignment
The header is naturally aligned for efficient access on most architectures.

### Memory Usage
Decoders should validate compressed_size before allocation to prevent memory exhaustion attacks.

### Naming Conventions
The codebase follows consistent snake_case naming conventions:
- Functions: `generate_frame_filename()`, `parse_frame_number_from_filename()`
- Variables: `frame_number`, `compressed_size`, `buffer_size`
- Constants: `FRAME_WIDTH`, `FRAME_HEIGHT`, `GVC_SUCCESS`

### Performance
- CRC32 calculation is fast (hardware-accelerated on modern CPUs)
- LZFSE decompression is optimized for Apple platforms with excellent performance
- Delta decoding is linear time O(n) where n = pixel count
- LZFSE provides ~20% better compression than zlib with similar decode speeds

## Example Usage

### Encoding
```c
// Create frame
frame_t frame;
compress_frame_delta(current, previous, &frame);
frame.header.frame_number = 42;

// Serialize
uint8_t* buffer;
size_t size;
serialize_frame(&frame, &buffer, &size);

// Store in Git
git_create_blob(buffer, size, blob_hash);
```

### Decoding
```c
// Read from Git
uint8_t* buffer;
size_t size;
git_read_blob(blob_hash, &buffer, &size);

// Deserialize
frame_t frame;
deserialize_frame(buffer, size, &frame);

// Decompress
raw_frame_t output;
decompress_frame_delta(&frame, previous, &output);
```

## Version History

- **v1.0**: Initial format with raw and delta compression
- **Future**: Audio support, variable resolution, quality levels