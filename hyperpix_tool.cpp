#include <dirent.h>
#include <fcntl.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"

#include <libgen.h>

constexpr int SIGNATURE_LEN = 8;
constexpr int FLAGS_LEN = 8;
constexpr int VERSION = 0x01;

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
    uint8_t reserved = 0;
    uint32_t reservedI = 0;
};

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
        memcpy(ptr, "HYPERPIX", SIGNATURE_LEN);
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
            convertFile(argv[arg], "test.basis", &pool);
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
                convertFile(f, "test.basis", &pool);
                auto end = std::chrono::high_resolution_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                seconds = ms * (files.size() - i) * 0.000000001 / i;
            }
            printf("\r\033[K 转码完成. 共 %d 个图片\n", static_cast<int>(files.size()));
        } else {
            fprintf(stderr, "[error] unknown file type <%s>!\n", argv[arg]);
        }
    }

    return EXIT_SUCCESS;
}

bool convertFile(const std::string &file, const std::string &output, basisu::job_pool *pool) {
    basisu::basis_compressor_params params;
    basisu::basis_compressor compressor;

    basisu::image srcImage;
    if (!basisu::load_image(file, srcImage)) {
        fprintf(stderr, "[error] failed to load image <%s>!\n", file.c_str());
        return EXIT_FAILURE;
    }

    params.m_create_ktx2_file = false;
    params.m_uastc = true;
    params.m_pJob_pool = pool;
    params.m_status_output = false;
    // params.m_read_source_images = true;
    params.m_source_images.push_back(srcImage);

    compressor.init(params);
    auto code = compressor.process();
    if (code != basisu::basis_compressor::error_code::cECSuccess) {
        fprintf(stderr, "[error] failed to encode image <%s>!\n", file.c_str());
        return false;
    }
    const auto &basisFile = compressor.get_output_basis_file();
    writeFile(output.c_str(), basisFile.data(), basisFile.size(), srcImage.has_alpha());
    return true;
}