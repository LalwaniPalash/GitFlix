#include "git_vid_codec.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

// Function to check if FFmpeg is available
static int check_ffmpeg_available(void) {
    int status = system("ffmpeg -version > /dev/null 2>&1");
    return WEXITSTATUS(status) == 0 ? GVC_SUCCESS : GVC_ERROR_IO;
}

// Function to get video information using FFprobe
static int get_video_info(const char* input_file, int* width, int* height, double* fps, int* frame_count) {
    char cmd[1024];
    FILE* pipe;
    
    // Get video dimensions and fps
    snprintf(cmd, sizeof(cmd), 
        "ffprobe -v quiet -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 '%s'", 
        input_file);
    
    pipe = popen(cmd, "r");
    if (!pipe) return GVC_ERROR_IO;
    
    char line[256];
    if (fgets(line, sizeof(line), pipe)) {
        int num, den;
        if (sscanf(line, "%d,%d,%d/%d", width, height, &num, &den) == 4) {
            *fps = (double)num / den;
        } else {
            pclose(pipe);
            return GVC_ERROR_FORMAT;
        }
    } else {
        pclose(pipe);
        return GVC_ERROR_FORMAT;
    }
    pclose(pipe);
    
    // Get frame count
    snprintf(cmd, sizeof(cmd), 
        "ffprobe -v quiet -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 '%s'", 
        input_file);
    
    pipe = popen(cmd, "r");
    if (!pipe) return GVC_ERROR_IO;
    
    if (fgets(line, sizeof(line), pipe)) {
        *frame_count = atoi(line);
    } else {
        // Fallback: count frames manually (slower)
        pclose(pipe);
        snprintf(cmd, sizeof(cmd), 
            "ffprobe -v quiet -select_streams v:0 -show_entries packet=n_frames -of csv=p=0 '%s' | wc -l", 
            input_file);
        pipe = popen(cmd, "r");
        if (pipe && fgets(line, sizeof(line), pipe)) {
            *frame_count = atoi(line);
        } else {
            *frame_count = -1; // Unknown
        }
    }
    pclose(pipe);
    
    return GVC_SUCCESS;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.mp4> <output_repo_path>\n", argv[0]);
        fprintf(stderr, "\nConverts an MP4 video file to a Git repository using the Git Video Codec.\n");
        fprintf(stderr, "\nRequirements:\n");
        fprintf(stderr, "  - FFmpeg must be installed and available in PATH\n");
        fprintf(stderr, "  - Input video will be scaled to 1920x1080 at 60fps\n");
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s input.mp4 ./video_repo\n", argv[0]);
        return 1;
    }

    const char* mp4_path = argv[1];
    const char* repo_path = argv[2];

    printf("Git Video Codec - MP4 Converter\n");
    printf("Input: %s\n", mp4_path);
    printf("Output: %s\n", repo_path);
    printf("\n");

    int result = convert_mp4_to_repo(mp4_path, repo_path);
    
    if (result == GVC_SUCCESS) {
        printf("\nConversion completed successfully!\n");
        printf("You can now play the video with: ./git-vid-play %s\n", repo_path);
        return 0;
    } else {
        fprintf(stderr, "Error: Conversion failed with error code: %d\n", result);
        return 1;
    }
}

// Function to extract frames from MP4 to temporary directory
static int extract_frames_to_temp(const char* input_file, const char* temp_dir) {
    char cmd[2048];
    
    // Create temporary directory
    if (mkdir(temp_dir, 0755) != 0 && errno != EEXIST) {
        return GVC_ERROR_IO;
    }
    
    // Extract frames as RGB raw format
    // Scale to 1920x1080 if needed, maintain aspect ratio with padding
    snprintf(cmd, sizeof(cmd), 
        "ffmpeg -i '%s' -vf 'scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black' "
        "-f image2 -vcodec rawvideo -pix_fmt rgb24 '%s/frame_%%06d.rgb' -y > /dev/null 2>&1", 
        input_file, temp_dir);
    
    printf("Extracting frames from MP4...\n");
    int status = system(cmd);
    
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: FFmpeg extraction failed\n");
        return GVC_ERROR_IO;
    }
    
    return GVC_SUCCESS;
}

// Function to count extracted frames
static int count_extracted_frames(const char* temp_dir) {
    DIR* dir = opendir(temp_dir);
    if (!dir) return GVC_ERROR_IO;
    
    int count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "frame_") && strstr(entry->d_name, ".rgb")) {
            count++;
        }
    }
    
    closedir(dir);
    return count;
}

// Function to cleanup temporary directory
static void cleanup_temp_dir(const char* temp_dir) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
    system(cmd);
}

// Main function to convert MP4 to Git Video Codec repository
int convert_mp4_to_repo(const char* input_file, const char* repo_path) {
    if (!input_file || !repo_path) {
        return GVC_ERROR_MEMORY;
    }
    
    // Check if input file exists
    struct stat st;
    if (stat(input_file, &st) != 0) {
        fprintf(stderr, "Input file does not exist: %s\n", input_file);
        return GVC_ERROR_IO;
    }
    
    // Check FFmpeg availability
    if (check_ffmpeg_available() != GVC_SUCCESS) {
        fprintf(stderr, "FFmpeg is not available. Please install FFmpeg.\n");
        fprintf(stderr, "macOS: brew install ffmpeg\n");
        fprintf(stderr, "Ubuntu: sudo apt-get install ffmpeg\n");
        return GVC_ERROR_IO;
    }
    
    // Get video information
    int width, height, frame_count;
    double fps;
    
    printf("Analyzing video file: %s\n", input_file);
    int result = get_video_info(input_file, &width, &height, &fps, &frame_count);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Failed to get video information\n");
        return result;
    }
    
    printf("Video info: %dx%d, %.2f fps", width, height, fps);
    if (frame_count > 0) {
        printf(", %d frames\n", frame_count);
    } else {
        printf(", frame count unknown\n");
    }
    
    // Warn if not 1920x1080
    if (width != FRAME_WIDTH || height != FRAME_HEIGHT) {
        printf("Note: Video will be scaled/padded to %dx%d\n", FRAME_WIDTH, FRAME_HEIGHT);
    }
    
    // Create temporary directory for extracted frames
    char temp_dir[256];
    snprintf(temp_dir, sizeof(temp_dir), "/tmp/gvc_frames_%d", getpid());
    
    // Extract frames
    result = extract_frames_to_temp(input_file, temp_dir);
    if (result != GVC_SUCCESS) {
        cleanup_temp_dir(temp_dir);
        return result;
    }
    
    // Count actual extracted frames
    int actual_frame_count = count_extracted_frames(temp_dir);
    if (actual_frame_count <= 0) {
        fprintf(stderr, "No frames were extracted\n");
        cleanup_temp_dir(temp_dir);
        return GVC_ERROR_IO;
    }
    
    printf("Extracted %d frames\n", actual_frame_count);
    
    // Initialize Git repository
    result = git_init_repo(repo_path);
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Error: Failed to initialize Git repository\n");
        cleanup_temp_dir(temp_dir);
        return result;
    }
    
    // Change to repository directory
    char original_cwd[1024];
    if (getcwd(original_cwd, sizeof(original_cwd)) == NULL) {
        cleanup_temp_dir(temp_dir);
        return GVC_ERROR_IO;
    }
    
    if (chdir(repo_path) != 0) {
        fprintf(stderr, "Error: Failed to change to repository directory\n");
        cleanup_temp_dir(temp_dir);
        return GVC_ERROR_IO;
    }
    
    printf("Encoding frames to Git repository...\n");
    
    // Encode frames to Git commits
    raw_frame_t current_frame, previous_frame;
    char current_commit_hash[GIT_HASH_SIZE + 1] = {0};
    char previous_commit_hash[GIT_HASH_SIZE + 1] = {0};
    
    size_t total_original_size = 0;
    
    for (int frame_num = 0; frame_num < actual_frame_count; frame_num++) {
        // Read frame from temporary directory
        char frame_filename[512];
        generate_frame_path(temp_dir, frame_num + 1, frame_filename, sizeof(frame_filename)); // FFmpeg starts from 1
        
        result = read_raw_frame(frame_filename, &current_frame);
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
        if (frame_num % 30 == 0 || frame_num == actual_frame_count - 1) {
            printf("Progress: %d/%d frames (%.1f%%)\n", 
                   frame_num + 1, actual_frame_count, 
                   (float)(frame_num + 1) / actual_frame_count * 100.0f);
        }
    }
    
    // Cleanup last frame
    if (actual_frame_count > 0) {
        free_raw_frame(&previous_frame);
    }
    
    // Return to original directory
    chdir(original_cwd);
    
    // Cleanup temporary directory
    cleanup_temp_dir(temp_dir);
    
    if (result == GVC_SUCCESS) {
        printf("\nConversion complete!\n");
        printf("Frames encoded: %d\n", actual_frame_count);
        printf("Original video: %s\n", input_file);
        printf("Git repository: %s\n", repo_path);
        printf("Original size: %.2f MB\n", total_original_size / (1024.0 * 1024.0));
        
        // Get repository size
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "du -sh '%s/.git'", repo_path);
        system(cmd);
        
        printf("\nTo play the video:\n");
        printf("./git-vid-play-metal '%s'\n", repo_path);
    }
    
    return result;
}