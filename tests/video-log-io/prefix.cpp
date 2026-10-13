#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define PROPERTY_VALUE_MAX 92
#define COLOR_FMT_NV12 0
#define VENUS_Y_STRIDE(format, width) (((width) + 7) & ~7UL)
#define VENUS_Y_SCANLINES(format, height) (((height) + 3) & ~3UL)
#define VENUS_BUFFER_SIZE(format, width, height) 64
using OMX_U32 = unsigned long;
struct OMX_BUFFERHEADERTYPE { unsigned char *pBuffer; OMX_U32 nFilledLen; };
constexpr unsigned OMX_ExtraDataNone = 0;
struct OMX_OTHER_EXTRADATATYPE { OMX_U32 nSize; unsigned eType; };
struct session { bool secure = false; bool is_secure_session() { return secure; } };
class venc_dev {
public:
    struct debug_state {
        FILE *outfile = nullptr, *extradatafile = nullptr, *infile = nullptr;
        char outfile_name[PROPERTY_VALUE_MAX] = "retained-output";
        char extradatafile_name[PROPERTY_VALUE_MAX] = "retained-extradata";
        char infile_name[PROPERTY_VALUE_MAX] = "retained-input";
        char log_loc[PROPERTY_VALUE_MAX] = "/logs";
        bool extradata_log = true;
    } m_debug;
    struct config { unsigned long codectype = V4L2_PIX_FMT_H264, input_width = 4, input_height = 2; } m_sVenc_cfg;
    session owner;
    session *venc_handle = &owner;
    int metadatamode = 1;
    int venc_output_log_buffers(const char *, int);
    int venc_extradata_log_buffers(char *);
    int venc_input_log_buffers(OMX_BUFFERHEADERTYPE *, int, int);
};

struct io_state {
    int format_result = INT_MAX;
    bool open_failure = false, map_failure = false;
    unsigned formats = 0, opens = 0, maps = 0, unmaps = 0, writes = 0;
    unsigned invalid_writes = 0, invalid_unmaps = 0;
    std::string filename;
    std::vector<unsigned char> data;
};
static io_state io;
static unsigned char mapping[64];
static unsigned char input[64];
static const char encoded[] = "encoded";
static OMX_OTHER_EXTRADATATYPE extra = {sizeof(extra), OMX_ExtraDataNone};
static unsigned failures;
static const char *case_name;

static void expect(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL %s: %s\n", case_name, message); failures++; }
}
static void diagnostic(const char *, ...) {}
#define DEBUG_PRINT_ERROR(...) diagnostic(__VA_ARGS__)
#define DEBUG_PRINT_LOW(...) diagnostic(__VA_ARGS__)
#define DEBUG_PRINT_HIGH(...) diagnostic(__VA_ARGS__)

static int test_snprintf(char *destination, size_t capacity, const char *format, ...) {
    io.formats++;
    if (io.format_result < 0) {
        std::memset(destination, 'x', capacity);
        return io.format_result;
    }
    va_list arguments;
    va_start(arguments, format);
    int result = std::vsnprintf(destination, capacity, format, arguments);
    va_end(arguments);
    return io.format_result == INT_MAX ? result : io.format_result;
}
static FILE *test_fopen(const char *filename, const char *mode) {
    io.opens++;
    expect(std::strcmp(mode, "ab") == 0, "stream keeps append-binary mode");
    size_t length = strnlen(filename, PROPERTY_VALUE_MAX);
    if (length == PROPERTY_VALUE_MAX) {
        io.filename = "unterminated";
    } else {
        io.filename.assign(filename, length);
    }
    if (io.open_failure) { errno = EACCES; return nullptr; }
    return stderr;
}
static size_t test_fwrite(const void *address, size_t size, size_t count, FILE *stream) {
    io.writes++;
    expect(stream == stderr, "write uses the acquired stream");
    const auto pointer = reinterpret_cast<uintptr_t>(address);
    const unsigned char *storage[] = {mapping, input, reinterpret_cast<const unsigned char *>(encoded), reinterpret_cast<const unsigned char *>(&extra)};
    const size_t capacities[] = {sizeof(mapping), sizeof(input), sizeof(encoded), sizeof(extra)};
    for (unsigned index = 0; index < 4; index++) {
        const auto start = reinterpret_cast<uintptr_t>(storage[index]);
        if (pointer >= start && pointer - start <= capacities[index] && size * count <= capacities[index] - (pointer - start)) {
            const auto *bytes = static_cast<const unsigned char *>(address);
            io.data.insert(io.data.end(), bytes, bytes + size * count);
            return count;
        }
    }
    io.invalid_writes++;
    return 0;
}
static void *test_mmap(void *address, size_t length, int protection, int flags, int descriptor, off_t offset) {
    io.maps++;
    expect(address == nullptr && length == sizeof(mapping), "mapping uses full modeled buffer extent");
    expect(protection == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED, "mapping retains access and sharing mode");
    expect(descriptor == 23 && offset == 4096, "mapping preserves borrowed fd and plane offset");
    return io.map_failure ? MAP_FAILED : mapping;
}
static int test_munmap(void *address, size_t length) {
    io.unmaps++;
    if (address != mapping || length != sizeof(mapping)) io.invalid_unmaps++;
    return 0;
}
#define snprintf test_snprintf
#define fopen test_fopen
#define fwrite test_fwrite
#define mmap test_mmap
#define munmap test_munmap
