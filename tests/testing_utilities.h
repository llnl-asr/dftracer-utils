#ifndef DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
#define DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H

#ifdef __cplusplus
#include <string>
#include <vector>
extern "C" {
#endif

// C API for testing utilities
typedef struct test_environment* test_environment_handle_t;

/**
 * Create a test environment with default number of lines (100)
 */
test_environment_handle_t test_environment_create(void);

/**
 * Create a test environment with specified number of lines
 */
test_environment_handle_t test_environment_create_with_lines(size_t lines);

/**
 * Destroy a test environment and clean up resources
 */
void test_environment_destroy(test_environment_handle_t env);

/**
 * Check if test environment is valid
 */
int test_environment_is_valid(test_environment_handle_t env);

/**
 * Get the test directory path
 * Returns a pointer to internal string - do not free
 */
const char* test_environment_get_dir(test_environment_handle_t env);

/**
 * Archive formats supported by the test environment
 */
typedef enum { TEST_FORMAT_GZIP = 0, TEST_FORMAT_TAR_GZIP = 1 } test_format_t;

/**
 * Create a test gzip file and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_gzip_file(test_environment_handle_t env);

/**
 * Create a test file with specified format and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_file_with_format(
    test_environment_handle_t env, test_format_t format);

/**
 * Create a tar.gz archive with multiple files
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_tar_gzip_file(test_environment_handle_t env);

/**
 * Create a tar archive and then compress it to gzip
 * Returns 1 on success, 0 on failure
 */
int create_tar_archive_and_compress(const char** file_paths, size_t num_files,
                                    const char* output_file);

/**
 * Extract a specific file from tar.gz archive
 * Returns 1 on success, 0 on failure
 */
int extract_file_from_tar_gz(const char* tar_gz_path, const char* file_path,
                             const char* output_path);

/**
 * Get list of files in tar archive
 * Returns allocated array of strings - caller must free each string and the
 * array
 */
char** get_tar_file_list(const char* tar_path, size_t* num_files);

/**
 * Free file list returned by get_tar_file_list
 */
void free_tar_file_list(char** file_list, size_t num_files);

/**
 * Get index path for a given gzip file
 * Returns allocated string - caller must free
 */
char* test_environment_get_index_path(test_environment_handle_t env,
                                      const char* gz_file);

/**
 * Compress a file to gzip format
 * Returns 1 on success, 0 on failure
 */
int compress_file_to_gzip_c(const char* input_file, const char* output_file);

size_t mb_to_b(double mb);

#ifdef __cplusplus
}

namespace dft_utils_test {

enum class Format { GZIP = 0, TAR_GZIP = 1 };

struct TarFileInfo {
    std::string filename;
    std::string content;
    std::size_t num_lines;
};

bool compress_file_to_gzip(const std::string& input_file,
                           const std::string& output_file);

bool create_tar_archive(const std::vector<TarFileInfo>& files,
                        const std::string& output_path);
bool create_tar_gz_archive(const std::vector<TarFileInfo>& files,
                           const std::string& output_path);
std::vector<std::string> list_tar_contents(const std::string& tar_path);
bool extract_from_tar_gz(const std::string& tar_gz_path,
                         const std::string& file_path,
                         const std::string& output_path);

class TestEnvironment {
   public:
    TestEnvironment() : TestEnvironment(100, Format::GZIP) {}
    TestEnvironment(std::size_t lines) : TestEnvironment(lines, Format::GZIP) {}
    TestEnvironment(std::size_t lines, Format format);
    TestEnvironment(const TestEnvironment&) = delete;
    TestEnvironment& operator=(const TestEnvironment&) = delete;
    ~TestEnvironment();

    const std::string& get_dir() const;
    bool is_valid() const;
    std::string create_test_file();  // Format-aware file creation
    std::string create_test_gzip_file();
    std::string create_test_tar_gzip_file();
    std::string get_index_path(const std::string& gz_file);
    Format get_format() const { return format_; }

    // DFTracer-specific test file creation
    std::string create_dft_test_file(int num_events = 100);
    std::string create_dft_test_gzip_file(int num_events = 100);

   private:
    std::size_t num_lines;
    std::string test_dir;
    Format format_;

    std::string create_test_gzip_file_impl();
    std::string create_test_tar_gzip_file_impl();
};
}  // namespace dft_utils_test
#endif

#endif  // DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
