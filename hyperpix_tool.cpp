#include <dirent.h>
#include <fcntl.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_frontend.h"
#include "zstd/zstd.h"

#include <libgen.h>
#include <atomic>

#define SIGNATURE "HYPERPIX"

#define SIGNATURE_LEN 8
#define FLAGS_LEN     8
#define VERSION       0x01

static std::atomic_int64_t fileSize0 = 0;
static std::atomic_int64_t fileSize1 = 0;
static std::atomic_int32_t skipFiles = 0;
static std::atomic_int32_t errorFiles = 0;

enum HyperPixQuality {
    HPQ_BEST = 0,
    HPQ_HIGH = 1,
    HPQ_DEFAULT = HPQ_HIGH,
    HPQ_NORMAL = 2,
    HPQ_LOW = 3,
};

struct HyperPixFlags {
    uint8_t version = VERSION;
    uint8_t quality = HPQ_HIGH;
    uint8_t hasAlpha = 0;
    uint8_t reserved[5] = {0};
};

std::vector<uint8_t> compressFile(const void *data, size_t dataLen) {
    const int compressLevel = ZSTD_maxCLevel();
#if 0
    auto capacity = ZSTD_compressBound(dataLen);
    std::vector<uint8_t> output(capacity);
    size_t compressedLen = ZSTD_compress(output.data(), capacity, data, dataLen, compressLevel);
    output.resize(compressedLen);
#else
    auto *ctx = ZSTD_createCCtx();
    auto capacity = ZSTD_compressBound(dataLen);
    std::vector<uint8_t> output(capacity);
    auto compressedLen = ZSTD_compressCCtx(ctx, output.data(), output.size(), data, dataLen, compressLevel);
    output.resize(compressedLen);
    ZSTD_freeCCtx(ctx);
#endif
    return output;
}

void writeFile(const char *dst, const void *data, size_t dataLen, bool hasAlpha, HyperPixQuality quality = HPQ_DEFAULT) {
    int fd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        fprintf(stderr, "[error] failed to open <%s>!\n", dst);
        return;
    }
    HyperPixFlags flags;
    flags.hasAlpha = hasAlpha;
    flags.quality = quality;
    ftruncate(fd, dataLen + FLAGS_LEN + SIGNATURE_LEN);
    void *ptr =
        mmap(nullptr, dataLen + 8, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "[error] failed to mmap <%s>!\n", dst);
        return;
    }
    auto *cptr = reinterpret_cast<char *>(ptr);
    if constexpr (FLAGS_LEN + SIGNATURE_LEN > 0) {
        memcpy(ptr, SIGNATURE, SIGNATURE_LEN);
        memcpy(cptr + SIGNATURE_LEN, &flags, FLAGS_LEN);
    }
    memcpy(cptr + SIGNATURE_LEN + FLAGS_LEN, data, dataLen);
    munmap(ptr, dataLen + SIGNATURE_LEN + FLAGS_LEN);
    close(fd);
}

static void collectPngFiles(const std::string &dirname, std::vector<std::string> &output) {
    DIR *dir = opendir(dirname.c_str());
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        auto cdirname = std::string(ent->d_name, ent->d_namlen);

        if (cdirname.length() == 1 && cdirname[0] == '.') continue;
        if (cdirname.length() == 2 && cdirname[0] == '.' && cdirname[1] == '.') continue;

        auto filepath = (dirname + "/").append(cdirname);
        if (ent->d_type == DT_DIR) {
            collectPngFiles(filepath, output);
        } else if (ent->d_type == DT_REG) {
            if (filepath.ends_with(".png") || filepath.ends_with(".jpg") || filepath.ends_with(".jpeg")) {
                output.emplace_back(filepath);
            }
        }
    }
    closedir(dir);
}

static void printProgress(float precent, int a, int b) {
    printf("\r\033[K 转码中 [");
    int l = static_cast<int>(precent * 30);
    int r = 30 - l;
    for (int i = 0; i < 30; i++) {
        if (i < l)
            putc('#', stdout);
        else
            putc(' ', stdout);
    }
    printf("] %.2f%%  >> %d/%d <<", precent * 100, a, b);
    fflush(stdout);
}

bool convertFile(const std::string &file, const std::string &output, std::atomic_bool &);

#ifndef VERSION_STRING
    #define VERSION_STRING "0.1.0"
#endif
#ifndef COMMIT_STRING
    #define COMMIT_STRING "unknow commit id"
#endif

int main(int argc, char **argv) {
    int opt = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") || strcmp(argv[i], "--help")) {
            opt = 'h';
            break;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") || strcmp(argv[i], "--version")) {
            opt = 'v';
            break;
        }
    }
    if (argc == 1) {
        opt = 'h';
    }

    switch (opt) {
        case 'h':
            printf("Usage: %s file_or_directory ... \n", argv[0]);
            return 0;
        case 'v':
            printf("Version: %s\nCommit: %s\n", VERSION_STRING, COMMIT_STRING);
            return 0;
    }

    basisu::basisu_encoder_init();
    basisu::job_pool pool(std::thread::hardware_concurrency());

    std::atomic_bool openCLFailed = false;

    for (int arg = 1; arg < argc; arg++) {
        struct stat st;
        if (lstat(argv[arg], &st)) {
            fprintf(stderr, "[error] file <%s> not found!\n", argv[1]);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            printf("[log] 处理单一文件<%s>...\n", argv[arg]);
            convertFile(argv[arg], argv[arg], openCLFailed);
        } else if (S_ISDIR(st.st_mode)) {
            printf("[log] 查找目录<%s>中的图片...\n", argv[arg]);
            std::vector<std::string> files;
            collectPngFiles(argv[arg], files);
            std::atomic_int32_t i = 0;
            std::mutex mtx;
            std::latch lat(files.size());
            auto total = files.size();

            for (auto &f : files) {
                pool.add_job([ff = f, &i, &openCLFailed, &mtx, &lat, total]() {
                    i++;
                    convertFile(ff, ff, openCLFailed);
                    lat.count_down();
                });
            }
            while (!lat.try_wait()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                printProgress(i * 1.0F / total, i.load(), total);
            }
            printf("\r\033[K 转码完成. 共 %d 个图片\n", static_cast<int>(files.size()));
            if (skipFiles) {
                printf("  忽略 %d 个图片\n", skipFiles.load(std::memory_order_acquire));
            }
            if (errorFiles) {
                printf("  错误图片: %d\n", errorFiles.load(std::memory_order_acquire));
            }
        } else {
            fprintf(stderr, "[error] unknown file type <%s>!\n", argv[arg]);
        }
    }

    pool.wait_for_all();

    if (fileSize1 > 0) {
        printf("   文件大小变化：%lld -> %lld\n", fileSize0.load(), fileSize1.load());
        auto diff = 100.0 * (fileSize1.load() - fileSize0.load()) / fileSize0.load();
        printf("      %s : %.2lf %%\n", diff > 0 ? "膨胀" : "减少", diff);
    }

    return EXIT_SUCCESS;
}

bool convertFile(const std::string &file, const std::string &output, std::atomic_bool &openCLFailed) {
    int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[error] failed to open file image <%s>!\n", file.c_str());
        errorFiles++;
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    if (st.st_size < 16) {
        fprintf(stderr, "[error] file <%s> too small!\n", file.c_str());
        close(fd);
        errorFiles++;
        return false;
    }
    char header[16];
    auto readn = read(fd, header, 16);
    if (readn < 16) {
        fprintf(stderr, "[error] failed to read header <%s>\n", file.c_str());
        close(fd);
        errorFiles++;
        return false;
    }
    if (memcmp(header, SIGNATURE, SIGNATURE_LEN) == 0) {
        // printf("\n[log] skip file %s\n", file.c_str());
        close(fd);
        skipFiles++;
        return false;
    }
    close(fd);

    basisu::basis_compressor_params params;
    basisu::basis_compressor compressor;

    basisu::image srcImage;
    if (!basisu::load_image(file, srcImage)) {
        fprintf(stderr, "[error] failed to load image <%s>!\n", file.c_str());
        errorFiles++;
        return EXIT_FAILURE;
    }

    fileSize0 += st.st_size;

    basisu::job_pool lpool(1);

    params.m_compression_level = basisu::BASISU_MAX_COMPRESSION_LEVEL;
    params.m_create_ktx2_file = false;
    params.m_uastc = true;
    // params.m_quality_level = 120;
    params.m_pJob_pool = &lpool;
    params.m_status_output = false;
    // params.m_use_opencl = true;
    // params.m_no_selector_rdo = true;
    // params.m_mip_gen = false;
    // params.m_read_source_images = true;
    params.m_source_images.push_back(srcImage);

    if (openCLFailed.load(std::memory_order_acquire)) {
        params.m_use_opencl = false;
    } else {
        params.m_use_opencl = true;
    }

    compressor.init(params);

    if (compressor.get_opencl_failed()) {
        openCLFailed.store(true, std::memory_order_release);
    }

    auto code = compressor.process();
    if (code != basisu::basis_compressor::error_code::cECSuccess) {
        fprintf(stderr, "[error] failed to encode image <%s>!\n", file.c_str());
        errorFiles++;
        return false;
    }
    lpool.wait_for_all();

    const auto &basisFile = compressor.get_output_basis_file();

#if 1 // enable compress
    auto compressedData = compressFile(basisFile.data(), basisFile.size());
    writeFile(output.c_str(), compressedData.data(), compressedData.size(), srcImage.has_alpha());
    fileSize1.fetch_add(compressedData.size() + SIGNATURE_LEN + FLAGS_LEN);
#else
    writeFile(output.c_str(), basisFile.data(), basisFile.size(), srcImage.has_alpha());
    fileSize1.fetch_add(basisFile.size() + SIGNATURE_LEN + FLAGS_LEN);
#endif

    return true;
}
