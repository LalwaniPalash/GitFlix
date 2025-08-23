#include "git_vid_codec.h"

// Main function for encoder binary
int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <input_path|test> <output_repo_path>\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s test ./video_repo          # Generate test frames\n", argv[0]);
        printf("  %s ./frames ./video_repo      # Encode from frame files\n", argv[0]);
        return 1;
    }
    
    const char* input_path = argv[1];
    const char* repo_path = argv[2];
    
    int result = encode_video_sequence(input_path, repo_path);
    
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Encoding failed with error code: %d\n", result);
        return 1;
    }
    
    return 0;
}