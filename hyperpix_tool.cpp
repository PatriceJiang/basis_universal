#include <dirent.h>
#include <fcntl.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_frontend.h"
#include "transcoder/basisu_transcoder_uastc.h"
#include "zstd/zstd.h"

#include <libgen.h>

#define SIGNATURE "HYPERPIX"

constexpr int SIGNATURE_LEN = 8;
constexpr int FLAGS_LEN = 8;
constexpr int VERSION = 0x01;

static int64_t fileSize0 = 0;
static int64_t fileSize1 = 0;
static int32_t skipFiles = 0;
static int32_t errorFiles = 0;

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
    auto capacity = ZSTD_compressBound(dataLen);
    std::vector<uint8_t> output(capacity);
    size_t compressedLen = ZSTD_compress(output.data(), capacity, data, dataLen, compressLevel);
    output.resize(compressedLen);
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
            if (filepath.ends_with(".png")) {
                output.emplace_back(filepath);
            }
        }
    }
    closedir(dir);
}

static void printProgress(float precent, const char *tag, float seconds) {
    printf("\r\033[K 转码中 [");
    int l = static_cast<int>(precent * 30);
    int r = 30 - l;
    for (int i = 0; i < 30; i++) {
        if (i < l)
            putc('#', stdout);
        else
            putc(' ', stdout);
    }
    printf("] %.2f%%, 剩余: %.2f s  >> %s <<", precent * 100, (seconds < 0 ? 0 : seconds), tag);
    fflush(stdout);
}

bool convertFile(const std::string &file, const std::string &output, basisu::job_pool *pool);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "[error] parameter <path> required!\n");
        return EXIT_FAILURE;
    }

    basisu::basisu_encoder_init();

    basisu::job_pool pool(std::thread::hardware_concurrency());

    for (int arg = 1; arg < argc; arg++) {
        struct stat st;
        if (lstat(argv[arg], &st)) {
            fprintf(stderr, "[error] file <%s> not found!\n", argv[1]);
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            printf("[log] 处理单一文件<%s>...\n", argv[arg]);
            convertFile(argv[arg], argv[arg], &pool);
        } else if (S_ISDIR(st.st_mode)) {
            printf("[log] 查找目录<%s>中的图片...\n", argv[arg]);
            std::vector<std::string> files;
            collectPngFiles(argv[arg], files);
            int i = 0;
            float seconds = -1;
            auto start = std::chrono::high_resolution_clock::now();
            for (auto &f : files) {
                i++;
                std::vector<char> name(f.c_str(), f.c_str() + f.size());
                printProgress(i * 1.0F / files.size(), basename(name.data()), seconds);
                // printf("  %s\n", f.c_str());
                convertFile(f, f, &pool);
                auto end = std::chrono::high_resolution_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                seconds = ms * (files.size() - i) * 0.000000001 / i;
            }
            printf("\r\033[K 转码完成. 共 %d 个图片\n", static_cast<int>(files.size()));
            if(skipFiles) {
                printf("  忽略 %d 个图片\n", skipFiles);
            }
            if(errorFiles) {
                printf("  错误图片: %d\n", errorFiles);
            }
        } else {
            fprintf(stderr, "[error] unknown file type <%s>!\n", argv[arg]);
        }
    }

    if (fileSize1 > 0) {
        printf("   文件大小变化：%lld -> %lld\n", fileSize0, fileSize1);
        auto diff = 100.0 * (fileSize1 - fileSize0) / fileSize0;
        printf("      %s : %.2lf %%\n", diff > 0 ? "膨胀" : "减少", diff);
    }

    return EXIT_SUCCESS;
}

bool convertFile(const std::string &file, const std::string &output, basisu::job_pool *pool) {
    int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[error] failed to open file image <%s>!\n", file.c_str());
        errorFiles ++;
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    if (st.st_size < 16) {
        fprintf(stderr, "[error] file <%s> too small!\n", file.c_str());
        close(fd);
        errorFiles ++;
        return false;
    }
    char header[16];
    auto readn = read(fd, header, 16);
    if (readn < 16) {
        fprintf(stderr, "[error] failed to read header <%s>\n", file.c_str());
        close(fd);
        errorFiles ++;
        return false;
    }
    if (memcmp(header, SIGNATURE, SIGNATURE_LEN) == 0) {
        // printf("\n[log] skip file %s\n", file.c_str());
        close(fd);
        skipFiles ++;
        return false;
    }
    close(fd);

    basisu::basis_compressor_params params;
    basisu::basis_compressor compressor;

    basisu::image srcImage;
    if (!basisu::load_image(file, srcImage)) {
        fprintf(stderr, "[error] failed to load image <%s>!\n", file.c_str());
        errorFiles ++;
        return EXIT_FAILURE;
    }

    fileSize0 += st.st_size;

    // params.m_compression_level = basisu::BASISU_MAX_COMPRESSION_LEVEL;
    params.m_create_ktx2_file = false;
    params.m_uastc = true;
    // params.m_quality_level = 120;
    params.m_pJob_pool = pool;
    params.m_status_output = false;
    // params.m_use_opencl = true;
    // params.m_no_selector_rdo = true;
    // params.m_mip_gen = false;
    // params.m_read_source_images = true;
    params.m_source_images.push_back(srcImage);

    compressor.init(params);
    auto code = compressor.process();
    if (code != basisu::basis_compressor::error_code::cECSuccess) {
        fprintf(stderr, "[error] failed to encode image <%s>!\n", file.c_str());
        errorFiles ++;
        return false;
    }
    const auto &basisFile = compressor.get_output_basis_file();

    auto compressedData = compressFile(basisFile.data(), basisFile.size());

    writeFile(output.c_str(), compressedData.data(), compressedData.size(), srcImage.has_alpha());

    fileSize1 += (compressedData.size() + SIGNATURE_LEN + FLAGS_LEN);

    return true;
}