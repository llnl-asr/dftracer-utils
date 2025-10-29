#include "testing_utilities.h"

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
size_t mb_to_b(double mb) { return static_cast<std::size_t>(mb * 1024 * 1024); }
}  // extern "C"

namespace dft_utils_test {
bool compress_file_to_gzip(const std::string& input_file,
                           const std::string& output_file) {
    std::ifstream input(input_file, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    gzFile gz_output = gzopen(output_file.c_str(), "wb");
    if (!gz_output) {
        return false;
    }

    const std::size_t buffer_size = 8192;
    std::vector<char> buffer(buffer_size);

    while (input.read(buffer.data(), buffer_size) || input.gcount() > 0) {
        unsigned int bytes_read = static_cast<unsigned int>(input.gcount());
        if (gzwrite(gz_output, buffer.data(), bytes_read) !=
            static_cast<int>(bytes_read)) {
            gzclose(gz_output);
            return false;
        }
    }

    gzclose(gz_output);
    return true;
}

// Helper function to write tar header
static void write_tar_header(std::ostream& out, const std::string& filename,
                             std::size_t file_size) {
    char header[512];
    std::memset(header, 0, 512);

    // File name (100 bytes)
    std::strncpy(header, filename.c_str(), 99);

    // File mode (8 bytes) - regular file, readable/writable
    std::strcpy(header + 100, "0000644");

    // Owner ID (8 bytes)
    std::strcpy(header + 108, "0000000");

    // Group ID (8 bytes)
    std::strcpy(header + 116, "0000000");

    // File size in octal (12 bytes)
    std::snprintf(header + 124, 12, "%011lo",
                  static_cast<unsigned long>(file_size));

    // Modification time (12 bytes) - current time in octal
    std::snprintf(header + 136, 12, "%011lo",
                  static_cast<unsigned long>(std::time(nullptr)));

    // Checksum placeholder (8 bytes) - filled with spaces initially
    std::memset(header + 148, ' ', 8);

    // Type flag - regular file
    header[156] = '0';

    // Calculate checksum
    unsigned int checksum = 0;
    for (int i = 0; i < 512; i++) {
        checksum += static_cast<unsigned char>(header[i]);
    }

    // Write checksum in octal
    std::snprintf(header + 148, 8, "%06o\0", checksum);

    out.write(header, 512);
}

bool create_tar_archive(const std::vector<TarFileInfo>& files,
                        const std::string& output_path) {
    std::ofstream tar_file(output_path, std::ios::binary);
    if (!tar_file.is_open()) {
        return false;
    }

    for (const auto& file_info : files) {
        // Write tar header
        write_tar_header(tar_file, file_info.filename,
                         file_info.content.size());

        // Write file content
        tar_file.write(file_info.content.c_str(), file_info.content.size());

        // Pad to 512-byte boundary
        std::size_t padding = 512 - (file_info.content.size() % 512);
        if (padding != 512) {
            std::vector<char> pad(padding, 0);
            tar_file.write(pad.data(), padding);
        }
    }

    // Write two zero blocks to mark end of archive
    std::vector<char> end_marker(1024, 0);
    tar_file.write(end_marker.data(), 1024);

    tar_file.close();
    return true;
}

bool create_tar_gz_archive(const std::vector<TarFileInfo>& files,
                           const std::string& output_path) {
    std::string tar_path = output_path + ".tmp";

    // Create tar archive first
    if (!create_tar_archive(files, tar_path)) {
        return false;
    }

    // Compress tar to gzip
    bool success = compress_file_to_gzip(tar_path, output_path);

    // Clean up temporary tar file
    fs::remove(tar_path);

    return success;
}

std::vector<std::string> list_tar_contents(const std::string& tar_path) {
    std::vector<std::string> file_list;
    std::ifstream tar_file(tar_path, std::ios::binary);

    if (!tar_file.is_open()) {
        return file_list;
    }

    char header[512];
    while (tar_file.read(header, 512)) {
        // Check if this is the end marker (all zeros)
        bool is_zero = true;
        for (int i = 0; i < 512; i++) {
            if (header[i] != 0) {
                is_zero = false;
                break;
            }
        }

        if (is_zero) {
            break;  // End of archive
        }

        // Extract filename (first 100 bytes, null-terminated)
        std::string filename(header, strnlen(header, 100));
        if (!filename.empty()) {
            file_list.push_back(filename);
        }

        // Extract file size from header (bytes 124-135, octal)
        char size_str[13];
        std::memcpy(size_str, header + 124, 12);
        size_str[12] = '\0';

        unsigned long file_size = std::strtoul(size_str, nullptr, 8);

        // Skip file content and padding
        std::size_t total_size = file_size;
        if (total_size % 512 != 0) {
            total_size += 512 - (total_size % 512);
        }
        tar_file.seekg(total_size, std::ios::cur);
    }

    return file_list;
}

bool extract_from_tar_gz(const std::string& tar_gz_path,
                         const std::string& file_path,
                         const std::string& output_path) {
    // For now, this is a simplified implementation
    // In a real scenario, you'd want to use libarchive or similar
    return false;  // Placeholder - would need actual tar.gz extraction logic
}

TestEnvironment::TestEnvironment(std::size_t lines, Format format)
    : num_lines(lines), format_(format) {
    // @note: enable this for debugging
    // DFTRACER_UTILS_LOGGER_INIT();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);

    fs::path temp_base = fs::temp_directory_path();
    fs::path test_path =
        temp_base / ("dftracer_test_" + std::to_string(dis(gen)));

    try {
        if (fs::create_directories(test_path)) {
            test_dir = test_path.string();
        }
    } catch (const std::exception& e) {
        // Leave test_dir empty to indicate failure
    }
}

TestEnvironment::~TestEnvironment() {
    if (!test_dir.empty()) {
        fs::remove_all(test_dir);
    }
}

const std::string& TestEnvironment::get_dir() const { return test_dir; }
bool TestEnvironment::is_valid() const { return !test_dir.empty(); }

std::string TestEnvironment::create_test_file() {
    switch (format_) {
        case Format::GZIP:
            return create_test_gzip_file_impl();
        case Format::TAR_GZIP:
            return create_test_tar_gzip_file_impl();
        default:
            return "";
    }
}

std::string TestEnvironment::create_test_gzip_file() {
    return create_test_gzip_file_impl();
}

std::string TestEnvironment::create_test_tar_gzip_file() {
    return create_test_tar_gzip_file_impl();
}

std::string TestEnvironment::create_test_gzip_file_impl() {
    if (test_dir.empty()) {
        return "";
    }

    // Create test file in the unique directory
    std::string gz_file = test_dir + "/test_data.gz";
    std::string idx_file = test_dir + "/test_data.gz.idx";
    std::string txt_file = test_dir + "/test_data.txt";

    // Write test data to text file
    std::ofstream f(txt_file);
    if (!f.is_open()) {
        return "";
    }

    for (std::size_t i = 1; i <= num_lines; ++i) {
        f << "{\"id\": " << i << ", \"message\": \"Test message " << i
          << "\"}\n";
    }
    f.close();

    bool success = compress_file_to_gzip(txt_file, gz_file);

    fs::remove(txt_file);

    if (success) {
        return gz_file;
    }

    return "";
}

std::string TestEnvironment::create_test_tar_gzip_file_impl() {
    if (test_dir.empty()) {
        return "";
    }

    std::string tar_gz_file = test_dir + "/test_data.tar.gz";

    // Create multiple files with different content
    std::vector<TarFileInfo> files;

    // Main data file
    std::ostringstream main_content;
    std::size_t lines_per_file = num_lines / 3;
    std::size_t remaining_lines = num_lines - (2 * lines_per_file);

    for (std::size_t i = 1; i <= lines_per_file; ++i) {
        main_content << "{\"id\": " << i
                     << ", \"message\": \"Main file message " << i
                     << "\", \"file\": \"main\"}\n";
    }
    files.push_back({"main.jsonl", main_content.str(), lines_per_file});

    // Secondary data file
    std::ostringstream secondary_content;
    for (std::size_t i = 1; i <= lines_per_file; ++i) {
        std::size_t id = lines_per_file + i;
        secondary_content << "{\"id\": " << id
                          << ", \"message\": \"Secondary file message " << i
                          << "\", \"file\": \"secondary\"}\n";
    }
    files.push_back(
        {"secondary.jsonl", secondary_content.str(), lines_per_file});

    // Additional data file with remaining lines
    std::ostringstream additional_content;
    for (std::size_t i = 1; i <= remaining_lines; ++i) {
        std::size_t id = (2 * lines_per_file) + i;
        additional_content << "{\"id\": " << id
                           << ", \"message\": \"Additional file message " << i
                           << "\", \"file\": \"additional\"}\n";
    }
    files.push_back(
        {"logs/additional.jsonl", additional_content.str(), remaining_lines});

    // Metadata file
    std::ostringstream metadata_content;
    metadata_content << "{\"total_files\": 3, \"total_lines\": " << num_lines
                     << ", \"format\": \"tar.gz\", \"created\": \""
                     << std::time(nullptr) << "\"}\n";
    files.push_back({"metadata.json", metadata_content.str(), 1});

    bool success = create_tar_gz_archive(files, tar_gz_file);

    if (success) {
        return tar_gz_file;
    }

    return "";
}

std::string TestEnvironment::get_index_path(const std::string& gz_file) {
    return gz_file + ".idx";
}
}  // namespace dft_utils_test

// C API implementations
extern "C" {

test_environment_handle_t test_environment_create(void) {
    return test_environment_create_with_lines(100);
}

test_environment_handle_t test_environment_create_with_lines(
    std::size_t lines) {
    try {
        auto* env = new dft_utils_test::TestEnvironment(
            lines, dft_utils_test::Format::GZIP);
        if (env->is_valid()) {
            return reinterpret_cast<test_environment_handle_t>(env);
        } else {
            delete env;
            return nullptr;
        }
    } catch (...) {
        return nullptr;
    }
}

void test_environment_destroy(test_environment_handle_t env) {
    if (env) {
        auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
        delete cpp_env;
    }
}

int test_environment_is_valid(test_environment_handle_t env) {
    if (!env) return 0;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
    return cpp_env->is_valid() ? 1 : 0;
}

const char* test_environment_get_dir(test_environment_handle_t env) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
    return cpp_env->get_dir().c_str();
}

char* test_environment_create_test_gzip_file(test_environment_handle_t env) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
    std::string gz_file = cpp_env->create_test_gzip_file();
    if (gz_file.empty()) {
        return nullptr;
    }
    char* result = static_cast<char*>(malloc(gz_file.length() + 1));
    if (result) {
        strcpy(result, gz_file.c_str());
    }
    return result;
}

char* test_environment_get_index_path(test_environment_handle_t env,
                                      const char* gz_file) {
    if (!env || !gz_file) return nullptr;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
    std::string idx_path = cpp_env->get_index_path(gz_file);
    char* result = static_cast<char*>(malloc(idx_path.length() + 1));
    if (result) {
        strcpy(result, idx_path.c_str());
    }
    return result;
}

char* test_environment_create_test_file_with_format(
    test_environment_handle_t env, test_format_t format) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);

    // Create a temporary environment with the desired format
    dft_utils_test::Format cpp_format = (format == TEST_FORMAT_TAR_GZIP)
                                            ? dft_utils_test::Format::TAR_GZIP
                                            : dft_utils_test::Format::GZIP;

    try {
        dft_utils_test::TestEnvironment temp_env(
            cpp_env->get_dir().empty() ? 100 : 100, cpp_format);
        std::string file_path;

        if (format == TEST_FORMAT_TAR_GZIP) {
            file_path = cpp_env->create_test_tar_gzip_file();
        } else {
            file_path = cpp_env->create_test_gzip_file();
        }

        if (file_path.empty()) {
            return nullptr;
        }

        char* result = static_cast<char*>(malloc(file_path.length() + 1));
        if (result) {
            strcpy(result, file_path.c_str());
        }
        return result;
    } catch (...) {
        return nullptr;
    }
}

char* test_environment_create_test_tar_gzip_file(
    test_environment_handle_t env) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dft_utils_test::TestEnvironment*>(env);
    std::string tar_gz_file = cpp_env->create_test_tar_gzip_file();
    if (tar_gz_file.empty()) {
        return nullptr;
    }
    char* result = static_cast<char*>(malloc(tar_gz_file.length() + 1));
    if (result) {
        strcpy(result, tar_gz_file.c_str());
    }
    return result;
}

int create_tar_archive_and_compress(const char** file_paths, size_t num_files,
                                    const char* output_file) {
    if (!file_paths || !output_file || num_files == 0) return 0;

    try {
        std::vector<dft_utils_test::TarFileInfo> files;

        for (size_t i = 0; i < num_files; ++i) {
            if (!file_paths[i]) continue;

            std::ifstream file(file_paths[i]);
            if (!file.is_open()) continue;

            std::ostringstream content;
            content << file.rdbuf();

            fs::path path(file_paths[i]);
            std::string filename = path.filename().string();

            files.push_back({filename, content.str(), 0});
        }

        return dft_utils_test::create_tar_gz_archive(files, output_file) ? 1
                                                                         : 0;
    } catch (...) {
        return 0;
    }
}

int extract_file_from_tar_gz(const char* tar_gz_path, const char* file_path,
                             const char* output_path) {
    if (!tar_gz_path || !file_path || !output_path) return 0;
    try {
        return dft_utils_test::extract_from_tar_gz(tar_gz_path, file_path,
                                                   output_path)
                   ? 1
                   : 0;
    } catch (...) {
        return 0;
    }
}

char** get_tar_file_list(const char* tar_path, size_t* num_files) {
    if (!tar_path || !num_files) return nullptr;

    try {
        auto file_list = dft_utils_test::list_tar_contents(tar_path);
        *num_files = file_list.size();

        if (file_list.empty()) {
            return nullptr;
        }

        char** result =
            static_cast<char**>(malloc(file_list.size() * sizeof(char*)));
        if (!result) {
            return nullptr;
        }

        for (size_t i = 0; i < file_list.size(); ++i) {
            result[i] = static_cast<char*>(malloc(file_list[i].length() + 1));
            if (result[i]) {
                strcpy(result[i], file_list[i].c_str());
            }
        }

        return result;
    } catch (...) {
        *num_files = 0;
        return nullptr;
    }
}

void free_tar_file_list(char** file_list, size_t num_files) {
    if (!file_list) return;

    for (size_t i = 0; i < num_files; ++i) {
        free(file_list[i]);
    }
    free(file_list);
}

int compress_file_to_gzip_c(const char* input_file, const char* output_file) {
    if (!input_file || !output_file) return 0;
    try {
        return dft_utils_test::compress_file_to_gzip(input_file, output_file)
                   ? 1
                   : 0;
    } catch (...) {
        return 0;
    }
}
}
