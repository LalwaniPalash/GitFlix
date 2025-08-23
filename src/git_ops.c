#include "git_vid_codec.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

// Helper function to execute git commands
static int execute_git_command(const char* command, char* output, size_t output_size) {
    if (!command) return GVC_ERROR_MEMORY;
    
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        fprintf(stderr, "Failed to open pipe for command: %s\n", command);
        return GVC_ERROR_GIT;
    }
    
    if (output && output_size > 0) {
        if (fgets(output, output_size, pipe) != NULL) {
            // Remove trailing newline
            size_t len = strlen(output);
            if (len > 0 && output[len-1] == '\n') {
                output[len-1] = '\0';
            }
        } else {
            fprintf(stderr, "No output from command: %s\n", command);
        }
    }
    
    int status = pclose(pipe);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return GVC_SUCCESS;
    }
    
    fprintf(stderr, "Command failed with status %d: %s\n", WEXITSTATUS(status), command);
    return GVC_ERROR_GIT;
}

// Helper function to write data to a temporary file
static int write_temp_file(const uint8_t* data, size_t size, char* filename_out) {
    sprintf(filename_out, "/tmp/git_vid_blob_%d_%ld", getpid(), time(NULL));
    
    FILE* file = fopen(filename_out, "wb");
    if (!file) return GVC_ERROR_IO;
    
    size_t written = fwrite(data, 1, size, file);
    fclose(file);
    
    return (written == size) ? GVC_SUCCESS : GVC_ERROR_IO;
}

int git_init_repo(const char* path) {
    if (!path) return GVC_ERROR_MEMORY;
    
    // Create directory if it doesn't exist
    mkdir(path, 0755);
    
    // Save current directory
    char* cwd = getcwd(NULL, 0);
    if (!cwd) return GVC_ERROR_IO;
    
    // Change to target directory
    if (chdir(path) != 0) {
        free(cwd);
        return GVC_ERROR_IO;
    }
    
    // Initialize git repo
    char output[256];
    int result = execute_git_command("git init", output, sizeof(output));
    
    // Debug output
    if (result != GVC_SUCCESS) {
        fprintf(stderr, "Git init failed in directory: %s\n", path);
        fprintf(stderr, "Git output: %s\n", output);
    }
    
    // Restore original directory
    chdir(cwd);
    free(cwd);
    
    return result;
}

int git_create_blob(const uint8_t* data, size_t size, char* hash_out) {
    if (!data || !hash_out) return GVC_ERROR_MEMORY;
    
    char temp_file[256];
    int result = write_temp_file(data, size, temp_file);
    if (result != GVC_SUCCESS) return result;
    
    // Create git blob from temporary file
    char command[512];
    snprintf(command, sizeof(command), "git hash-object -w '%s'", temp_file);
    
    result = execute_git_command(command, hash_out, GIT_HASH_SIZE + 1);
    
    // Clean up temporary file
    unlink(temp_file);
    
    return result;
}

int git_create_commit(const char* blob_hash, const char* message, 
                     const char* parent_hash, char* commit_hash_out) {
    if (!blob_hash || !message || !commit_hash_out) return GVC_ERROR_MEMORY;
    
    // Create tree with the blob
    char tree_command[512];
    snprintf(tree_command, sizeof(tree_command), 
             "echo '100644 blob %s\tframe.bin' | git mktree", blob_hash);
    
    char tree_hash[GIT_HASH_SIZE + 1];
    int result = execute_git_command(tree_command, tree_hash, sizeof(tree_hash));
    if (result != GVC_SUCCESS) return result;
    
    // Create commit
    char commit_command[1024];
    if (parent_hash && strlen(parent_hash) > 0) {
        snprintf(commit_command, sizeof(commit_command),
                 "git commit-tree %s -p %s -m '%s'", tree_hash, parent_hash, message);
    } else {
        snprintf(commit_command, sizeof(commit_command),
                 "git commit-tree %s -m '%s'", tree_hash, message);
    }
    
    result = execute_git_command(commit_command, commit_hash_out, GIT_HASH_SIZE + 1);
    if (result != GVC_SUCCESS) return result;
    
    // Update HEAD to point to new commit
    char update_ref_command[256];
    snprintf(update_ref_command, sizeof(update_ref_command),
             "git update-ref HEAD %s", commit_hash_out);
    
    return execute_git_command(update_ref_command, NULL, 0);
}

int git_read_blob(const char* hash, uint8_t** data_out, size_t* size_out) {
    if (!hash || !data_out || !size_out) return GVC_ERROR_MEMORY;
    
    // Get blob size first
    char size_command[256];
    snprintf(size_command, sizeof(size_command), "git cat-file -s %s", hash);
    
    char size_str[32];
    int result = execute_git_command(size_command, size_str, sizeof(size_str));
    if (result != GVC_SUCCESS) {
        return result;
    }
    
    size_t blob_size = strtoul(size_str, NULL, 10);
    if (blob_size == 0) return GVC_ERROR_FORMAT;
    
    // Read blob data
    char read_command[256];
    snprintf(read_command, sizeof(read_command), "git cat-file blob %s", hash);
    
    FILE *pipe = popen(read_command, "r");
    if (!pipe) {
        return GVC_ERROR_GIT;
    }
    
    uint8_t* buffer = malloc(blob_size);
    if (!buffer) {
        pclose(pipe);
        return GVC_ERROR_MEMORY;
    }
    
    size_t bytes_read = fread(buffer, 1, blob_size, pipe);
    int status = pclose(pipe);
    
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && bytes_read == blob_size) {
        *data_out = buffer;
        *size_out = blob_size;
        return GVC_SUCCESS;
    }
    

    free(buffer);
    return GVC_ERROR_GIT;
}

int git_get_commit_chain(char commits[][GIT_HASH_SIZE + 1], int max_commits) {
    if (!commits || max_commits <= 0) return GVC_ERROR_MEMORY;
    
    // Get commit chain in reverse chronological order
    char command[] = "git log --reverse --format=%H";
    
    FILE* pipe = popen(command, "r");
    if (!pipe) return GVC_ERROR_GIT;
    
    int count = 0;
    char line[GIT_HASH_SIZE + 2]; // +2 for newline and null terminator
    
    while (count < max_commits && fgets(line, sizeof(line), pipe) != NULL) {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        if (strlen(line) == GIT_HASH_SIZE) {
            strcpy(commits[count], line);
            count++;
        }
    }
    
    pclose(pipe);
    return count;
}

int git_checkout_commit(const char* commit_hash) {
    if (!commit_hash) return GVC_ERROR_MEMORY;
    
    char command[256];
    snprintf(command, sizeof(command), "git checkout %s", commit_hash);
    
    return execute_git_command(command, NULL, 0);
}

// Helper function to get blob hash from commit
int git_get_blob_from_commit(const char* commit_hash, char* blob_hash_out) {
    if (!commit_hash || !blob_hash_out) return GVC_ERROR_MEMORY;
    
    char command[256];
    snprintf(command, sizeof(command), 
             "git ls-tree %s | grep 'frame.bin' | cut -f1 | cut -d' ' -f3", 
             commit_hash);
    
    return execute_git_command(command, blob_hash_out, GIT_HASH_SIZE + 1);
}

// Optimized function to read frame data from a specific commit
int git_read_frame_from_commit(const char* commit_hash, uint8_t** data_out, size_t* size_out) {
    if (!commit_hash || !data_out || !size_out) return GVC_ERROR_MEMORY;
    
    // Use git show to directly read the file content in one command
    char command[256];
    snprintf(command, sizeof(command), "git show %s:frame.bin", commit_hash);
    
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        return GVC_ERROR_GIT;
    }
    
    // Read data in chunks to handle large files efficiently
    size_t capacity = 1024 * 1024; // Start with 1MB
    size_t size = 0;
    uint8_t* buffer = malloc(capacity);
    if (!buffer) {
        pclose(pipe);
        return GVC_ERROR_MEMORY;
    }
    
    size_t bytes_read;
    while ((bytes_read = fread(buffer + size, 1, capacity - size, pipe)) > 0) {
        size += bytes_read;
        if (size >= capacity) {
            capacity *= 2;
            uint8_t* new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                pclose(pipe);
                return GVC_ERROR_MEMORY;
            }
            buffer = new_buffer;
        }
    }
    
    int status = pclose(pipe);
    
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && size > 0) {
        // Trim buffer to actual size
        uint8_t* final_buffer = realloc(buffer, size);
        if (final_buffer) {
            buffer = final_buffer;
        }
        *data_out = buffer;
        *size_out = size;
        return GVC_SUCCESS;
    }
    
    free(buffer);
    return GVC_ERROR_GIT;
}