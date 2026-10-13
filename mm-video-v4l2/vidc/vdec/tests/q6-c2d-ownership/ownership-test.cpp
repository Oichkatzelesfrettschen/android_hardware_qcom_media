// The host boundary controls ION, CPU mappings, C2D requirements and driver slots.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>
const int O_RDONLY = 0;
#include <pthread.h>
#include <sys/mman.h>

#define USE_ION
#define OMX_VDEC_HEVC
#define _ANDROID_
#define DEBUG_PRINT_LOW(...) log_message(__VA_ARGS__)
#define DEBUG_PRINT_HIGH(...) log_message(__VA_ARGS__)
#define DEBUG_PRINT_ERROR(...) log_message(__VA_ARGS__)
static void log_message(const char *, ...) {}
#define BITMASK_PRESENT(mask, index) (*(mask) & (1UL << (index)))
#define BITMASK_ABSENT(mask, index) (!BITMASK_PRESENT(mask, index))
#define BITMASK_CLEAR(mask, index) (*(mask) &= ~(1UL << (index)))
#define BITMASK_SET(mask, index) (*(mask) |= (1UL << (index)))
#define ION_HEAP(heap) (1U << (heap))
const unsigned ION_IOMMU_HEAP_ID = 0, ION_ADSP_HEAP_ID = 22, MEM_HEAP_ID = 8;
const int ION_FLAG_CACHED = 1, ION_SECURE = 2;
const int ION_IOC_ALLOC = 1, ION_IOC_MAP = 2, ION_IOC_FREE = 3;
const int VIDIOC_PREPARE_BUF = 10, VIDIOC_STREAMON = 11, VIDIOC_DECODER_CMD = 12, V4L2_DEC_CMD_STOP = 0;
struct v4l2_decoder_cmd { int cmd; };
enum v4l2_buf_type { V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
struct v4l2_buffer { unsigned index; int fd; unsigned offset; };
const char MEM_DEVICE[] = "/dev/ion";
using OMX_U32 = unsigned long;
using OMX_PTR = void *;
using OMX_HANDLETYPE = void *;
using OMX_COLOR_FORMATTYPE = int;
using ColorConvertFormat = int;
const int OMX_COLOR_FormatMax = 0, OMX_COLOR_FormatYUV420Planar = 1;
const int YCbCr420P = 0, NV12_128m = 0, VDEC_YUV_FORMAT_NV12 = 0;
const int C2D_INPUT = 0, C2D_OUTPUT = 1, CAPTURE_PORT = 0, OUTPUT_PORT = 1;
const bool OMX_FALSE = false;
const int OMX_CORE_OUTPUT_PORT_INDEX = 1, OMX_COMPONENT_OUTPUT_FLUSH_PENDING = 0;
const int OMX_QCOM_PLATFORM_PRIVATE_PMEM = 1, OMX_SPEC_VERSION = 1;
enum OMX_ERRORTYPE { OMX_ErrorNone, OMX_ErrorBadParameter, OMX_ErrorInsufficientResources, OMX_ErrorUnsupportedSetting, OMX_ErrorUndefined, OMX_ErrorHardware };
enum vdec_buffer { VDEC_BUFFER_TYPE_OUTPUT };
struct ion_allocation_data { size_t len, align; unsigned heap_mask, flags; void *handle; };
struct ion_fd_data { void *handle; int fd; };
struct vdec_ion { ion_allocation_data ion_alloc_data; ion_fd_data fd_ion_data; int ion_device_fd; };
struct OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO { OMX_U32 pmem_fd, offset; };
struct OMX_QCOM_PLATFORM_PRIVATE_ENTRY { void *entry; int type; };
struct OMX_QCOM_PLATFORM_PRIVATE_LIST { unsigned nEntries; OMX_QCOM_PLATFORM_PRIVATE_ENTRY *entryList; };
struct OMX_BUFFERHEADERTYPE {
    void *pOutputPortPrivate; OMX_U32 nAllocLen, nFilledLen, nFlags, nOutputPortIndex, nSize;
    struct { unsigned nVersion; } nVersion;
    void *pPlatformPrivate; unsigned char *pBuffer; void *pAppPrivate;
};
struct vdec_bufferpayload { unsigned char *bufferaddr; int pmem_fd; size_t mmaped_size; };
struct vdec_setbuffer_cmd { vdec_buffer buffer_type; vdec_bufferpayload buffer; };
struct vdec_allocatorproperty { unsigned actualcount = 3, buffer_size = 4096, alignment = 4096; };
struct MemoryHeapBase { virtual ~MemoryHeapBase() {} };
struct VideoHeap : MemoryHeapBase { VideoHeap(int, unsigned, unsigned char *, void *, int) {} };
template<typename Target> struct sp {
    std::shared_ptr<Target> pointer;
    sp &operator=(Target *value) { pointer.reset(value); return *this; }
    Target *get() { return pointer.get(); }
};
class omx_c2d_conv {
public:
    unsigned opens = 0, closes = 0;
    unsigned source_size = 4096, destination_size = 4096;
    bool succeeds = true;
    bool open(unsigned, unsigned, int, int) { ++opens; return succeeds; }
    void close() { ++closes; }
    void destroy() {}
    bool get_buffer_size(int port, unsigned &size) { size = port == C2D_INPUT ? source_size : destination_size; return succeeds; }
};

struct Resources {
    int next_fd = 10;
    unsigned next_handle = 1;
    std::set<int> fds;
    std::map<void *, ion_allocation_data> handles;
    std::map<int, void *> exported;
    std::map<void *, size_t> mappings;
    std::vector<ion_allocation_data> allocations;
    int fail_ioctl = 0;
    std::set<std::pair<int, unsigned>> registered;
    unsigned prepare_calls = 0, streamon_calls = 0;
    int fail_driver_ioctl = 0;
    std::vector<std::string> shutdown_events;
    bool fail_driver_close = false;
    bool fail_open = false, fail_mmap = false, export_fd_zero = false;
    int open_fd() { const int fd = next_fd++; assert(fds.insert(fd).second); return fd; }
    void empty() { assert(fds.empty()); assert(handles.empty()); assert(mappings.empty()); assert(exported.empty()); assert(registered.empty()); }
} resources;
static int boundary_open(const char *, int) {
    if (resources.fail_open) { resources.fail_open = false; return -EMFILE; }
    return resources.open_fd();
}
static int boundary_close(int fd) {
    if (fd == 1000000) {
        if (resources.fail_driver_close) { resources.fail_driver_close = false; errno = EBADF; return -1; }
        resources.shutdown_events.push_back("driver-close");
        resources.registered.clear();
        return 0;
    }
    assert(resources.fds.erase(fd) == 1);
    resources.exported.erase(fd);
    return 0;
}
static int boundary_ioctl(int fd, int request, void *data) {
    if (fd == 1000000) {
        if (resources.fail_driver_ioctl == request) {
            resources.fail_driver_ioctl = 0;
            return -1;
        }
        if (request == VIDIOC_DECODER_CMD) {
            resources.shutdown_events.push_back("decoder-stop");
        } else if (request == VIDIOC_PREPARE_BUF) {
            ++resources.prepare_calls;
            auto *buffer = static_cast<v4l2_buffer *>(data);
            const auto region = std::make_pair(buffer->fd, buffer->offset);
            if (!resources.registered.insert(region).second) return -EINVAL;
        } else {
            assert(request == VIDIOC_STREAMON);
            ++resources.streamon_calls;
        }
        return 0;
    }
    assert(resources.fds.count(fd));
    if (resources.fail_ioctl == request) { resources.fail_ioctl = 0; return -1; }
    if (request == ION_IOC_ALLOC) {
        auto *allocation = static_cast<ion_allocation_data *>(data);
        allocation->handle = reinterpret_cast<void *>(static_cast<uintptr_t>(resources.next_handle++));
        resources.handles[allocation->handle] = *allocation;
        resources.allocations.push_back(*allocation);
    } else if (request == ION_IOC_MAP) {
        auto *exported = static_cast<ion_fd_data *>(data);
        assert(resources.handles.count(exported->handle));
        if (resources.export_fd_zero) {
            resources.export_fd_zero = false;
            exported->fd = 0;
            assert(resources.fds.insert(0).second);
        } else {
            exported->fd = resources.open_fd();
        }
        resources.exported[exported->fd] = exported->handle;
    } else {
        assert(request == ION_IOC_FREE);
        assert(resources.handles.erase(*static_cast<void **>(data)) == 1);
    }
    return 0;
}
static void *boundary_mmap(void *, size_t length, int, int, int fd, int) {
    assert(resources.fds.count(fd));
    if (resources.fail_mmap) { resources.fail_mmap = false; return MAP_FAILED; }
    void *mapping = std::malloc(length);
    assert(mapping);
    resources.mappings[mapping] = length;
    return mapping;
}
static int boundary_munmap(void *mapping, size_t length) {
    assert(resources.mappings.count(mapping));
    assert(resources.mappings.at(mapping) == length);
    resources.shutdown_events.push_back("unmap");
    resources.mappings.erase(mapping);
    std::free(mapping);
    return 0;
}
static int boundary_join(pthread_t thread, void **) {
    resources.shutdown_events.push_back(thread == 1 ? "message-join" : "async-join");
    return 0;
}
static int unsubscribe_to_events(int) { return 0; }
#define pthread_join boundary_join
static unsigned clip2(unsigned value) { unsigned result = 1; while (result < value) result <<= 1; return result; }
#define open boundary_open
#define close boundary_close
#define ioctl boundary_ioctl
#define mmap boundary_mmap
#define munmap boundary_munmap
// Keep member c2d.close distinct from the intercepted global close.
#undef close
static int close(int fd) { return boundary_close(fd); }
#undef open
static int open(const char *path, int flags) { return boundary_open(path, flags); }

class omx_vdec {
public:
    bool secure_mode = false, m_hevc_q6 = true, dynamic_buf_mode = false;
    bool streaming[2] = {}, in_reconfig = false, m_enable_android_native_buffers = false;
    bool ouput_egl_buffers = false, m_use_output_pmem = false, m_out_bPopulated = true;
    unsigned long m_flags = 0, m_out_bm_count = 0, m_out_bm_registered = 0;
    bool driver_shutdown_failed = false;
    int m_pipe_in = -1, m_pipe_out = -1;
    bool msg_thread_created = false, async_thread_created = false;
    pthread_t msg_thread_id = 1, async_thread_id = 2;
    bool shutdown_driver();
    pthread_mutex_t c_lock = PTHREAD_MUTEX_INITIALIZER;
    struct {
        vdec_allocatorproperty op_buf;
        struct { unsigned frame_height = 240, frame_width = 320; } video_resolution;
        int output_format = VDEC_YUV_FORMAT_NV12;
        int video_driver_fd = 1000000;
        vdec_bufferpayload *ptr_outputbuffer = nullptr;
        vdec_ion *op_buf_ion_info = nullptr;
    } drv_ctx;
    OMX_BUFFERHEADERTYPE *m_out_mem_ptr = nullptr;
    unsigned releases = 0, free_headers = 0, extradata_frees = 0, driver_allocations = 0;
    bool driver_failure = false, null_driver_header = false;
    bool requirements_active = true, requirements_failure = false;
    unsigned requirement_requests = 0, release_failures = 0, streamoff_failures = 0;
    /* ALLOCATOR_DECLARATION */
    void free_ion_memory(vdec_ion *);
    bool is_component_secure() { return secure_mode; }
    bool release_output_done();
    OMX_ERRORTYPE free_output_buffer(OMX_BUFFERHEADERTYPE *);
    OMX_ERRORTYPE prepare_output_buffer(v4l2_buffer *);
    int stream_off(int) { if (streamoff_failures) { --streamoff_failures; return -EIO; } streaming[0] = false; return 0; }
    void free_extradata() { ++extradata_frees; }
    OMX_ERRORTYPE allocate_output_buffer(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **, OMX_U32, OMX_PTR, OMX_U32);
    OMX_ERRORTYPE set_buffer_req(vdec_allocatorproperty *) {
        ++requirement_requests;
        if (requirements_failure) return OMX_ErrorInsufficientResources;
        requirements_active = true;
        return client_buffers.update_buffer_req() ? OMX_ErrorNone : OMX_ErrorInsufficientResources;
    }
    void free_output_buffer_header();
    OMX_ERRORTYPE deinit_output();
    OMX_ERRORTYPE normal_free_output(OMX_BUFFERHEADERTYPE *);
    /* CONVERSION_CLASS */
    allocate_color_convert_buf client_buffers;
    omx_vdec() { client_buffers.omx = this; client_buffers.enabled = true; }
    ~omx_vdec() { assert(shutdown_driver()); assert(!m_out_mem_ptr); pthread_mutex_destroy(&c_lock); }
};
static int release_buffers(omx_vdec *decoder, vdec_buffer) {
    ++decoder->releases;
    if (decoder->release_failures) { --decoder->release_failures; return -EIO; }
    decoder->requirements_active = false;
    resources.registered.clear();
    return 0;
}
/* PRODUCTION_FUNCTIONS */

OMX_ERRORTYPE omx_vdec::allocate_output_buffer(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **header, OMX_U32, OMX_PTR, OMX_U32) {
    ++driver_allocations;
    assert(requirements_active);
    if (driver_failure) return OMX_ErrorInsufficientResources;
    if (null_driver_header) return OMX_ErrorNone;
    if (!m_out_mem_ptr) {
        m_out_mem_ptr = new OMX_BUFFERHEADERTYPE[drv_ctx.op_buf.actualcount]();
        drv_ctx.ptr_outputbuffer = new vdec_bufferpayload[drv_ctx.op_buf.actualcount]();
        drv_ctx.op_buf_ion_info = new vdec_ion[drv_ctx.op_buf.actualcount]();
        // A driver pool bypasses destination fault injection at the controlled boundary.
        vdec_ion pool = {};
        pool.ion_device_fd = resources.open_fd();
        pool.ion_alloc_data.len = drv_ctx.op_buf.actualcount * drv_ctx.op_buf.buffer_size;
        pool.ion_alloc_data.heap_mask = ION_HEAP(ION_ADSP_HEAP_ID);
        const int saved_failure = resources.fail_ioctl;
        resources.fail_ioctl = 0;
        boundary_ioctl(pool.ion_device_fd, ION_IOC_ALLOC, &pool.ion_alloc_data);
        pool.fd_ion_data.handle = pool.ion_alloc_data.handle;
        boundary_ioctl(pool.ion_device_fd, ION_IOC_MAP, &pool.fd_ion_data);
        resources.fail_ioctl = saved_failure;
        const bool saved_map_failure = resources.fail_mmap;
        resources.fail_mmap = false;
        auto *base = static_cast<unsigned char *>(boundary_mmap(nullptr, pool.ion_alloc_data.len, 0, 0, pool.fd_ion_data.fd, 0));
        resources.fail_mmap = saved_map_failure;
        for (unsigned index = 0; index < drv_ctx.op_buf.actualcount; ++index) {
            drv_ctx.op_buf_ion_info[index] = pool;
            drv_ctx.ptr_outputbuffer[index] = {base + index * drv_ctx.op_buf.buffer_size, pool.fd_ion_data.fd, drv_ctx.op_buf.buffer_size};
        }
    }
    for (unsigned index = 0; index < drv_ctx.op_buf.actualcount; ++index) {
        if (!BITMASK_PRESENT(&m_out_bm_count, index)) {
            v4l2_buffer buffer = {index, drv_ctx.ptr_outputbuffer[index].pmem_fd, index * drv_ctx.op_buf.buffer_size};
            const OMX_ERRORTYPE result = prepare_output_buffer(&buffer);
            if (result != OMX_ErrorNone) return result;
            BITMASK_SET(&m_out_bm_count, index);
            *header = &m_out_mem_ptr[index];
            return OMX_ErrorNone;
        }
    }
    return OMX_ErrorInsufficientResources;
}
void omx_vdec::free_output_buffer_header() {
    assert(release_output_done());
    delete[] m_out_mem_ptr;
    delete[] drv_ctx.ptr_outputbuffer;
    delete[] drv_ctx.op_buf_ion_info;
    m_out_mem_ptr = nullptr;
    m_out_bm_registered = 0;
    drv_ctx.ptr_outputbuffer = nullptr;
    drv_ctx.op_buf_ion_info = nullptr;
    ++free_headers;
}
OMX_ERRORTYPE omx_vdec::deinit_output() {
    unsigned i = 0;
    /* DEINIT_OUTPUT_BLOCK */
    free_output_buffer_header();
    return OMX_ErrorNone;
}
OMX_ERRORTYPE omx_vdec::normal_free_output(OMX_BUFFERHEADERTYPE *buffer) {
    const unsigned nPortIndex = buffer - client_buffers.get_il_buf_hdr();
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    /* NORMAL_FREE_OUTPUT_BLOCK */
    return eRet;
}
static OMX_BUFFERHEADERTYPE *allocate(omx_vdec &decoder) {
    OMX_BUFFERHEADERTYPE *header = nullptr;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorNone);
    assert(header);
    return header;
}
static void release(omx_vdec &decoder, OMX_BUFFERHEADERTYPE *header, unsigned index) {
    assert(header == &decoder.client_buffers.get_il_buf_hdr()[index]);
    assert(decoder.normal_free_output(header) == OMX_ErrorNone);
}
static void policy_test() {
    for (bool q6 : {false, true}) for (bool secure : {false, true}) for (int flags : {0, ION_FLAG_CACHED, ION_SECURE}) {
        omx_vdec decoder;
        decoder.m_hevc_q6 = q6;
        decoder.secure_mode = secure;
        for (unsigned consumer = 0; consumer < 2; ++consumer) {
            vdec_ion allocation = {};
            allocation.ion_device_fd = decoder.alloc_map_ion_memory(5001, 8193, &allocation.ion_alloc_data, &allocation.fd_ion_data, flags,
                static_cast<omx_vdec::ion_buffer_consumer>(consumer));
            assert(allocation.ion_device_fd >= 0);
            const unsigned expected_flags = secure ? (flags & ION_SECURE) : (flags & ION_FLAG_CACHED);
            const unsigned expected_heap = secure && (flags & ION_SECURE) ? MEM_HEAP_ID : q6 && consumer == 0 ? ION_ADSP_HEAP_ID : ION_IOMMU_HEAP_ID;
            assert(allocation.ion_alloc_data.heap_mask == ION_HEAP(expected_heap));
            assert(allocation.ion_alloc_data.flags == expected_flags);
            assert(allocation.ion_alloc_data.len == 5001);
            assert(allocation.ion_alloc_data.align == 16384);
            close(allocation.fd_ion_data.fd);
            decoder.free_ion_memory(&allocation);
        }
        resources.empty();
    }
}
static void reuse_test() {
    omx_vdec decoder;
    auto *first = allocate(decoder);
    auto *middle = allocate(decoder);
    auto *last = allocate(decoder);
    const unsigned prepare_calls = resources.prepare_calls;
    assert(decoder.streaming[CAPTURE_PORT]);
    const int pool_fd = decoder.drv_ctx.ptr_outputbuffer[0].pmem_fd;
    const int first_fd = decoder.client_buffers.pmem_fd[0];
    const int last_fd = decoder.client_buffers.pmem_fd[2];
    release(decoder, middle, 1);
    assert(resources.fds.count(pool_fd));
    assert(resources.fds.count(first_fd));
    assert(resources.fds.count(last_fd));
    auto *replacement = allocate(decoder);
    assert(replacement == middle);
    assert(resources.prepare_calls == prepare_calls);
    assert(decoder.streaming[CAPTURE_PORT]);
    assert(decoder.client_buffers.get_dr_buf_hdr(replacement) == &decoder.m_out_mem_ptr[1]);
    assert(last->pBuffer != replacement->pBuffer);
    release(decoder, first, 0);
    assert(resources.fds.count(pool_fd));
    // Reconfiguration changes requested lengths while previous mappings stay live.
    decoder.client_buffers.c2d.destination_size = 8192;
    assert(decoder.client_buffers.update_buffer_req());
    first = allocate(decoder);
    assert(first->nAllocLen == 8192);
    release(decoder, last, 2);
    release(decoder, replacement, 1);
    release(decoder, first, 0);
    resources.empty();
    const unsigned opens = decoder.client_buffers.c2d.opens;
    allocate(decoder);
    assert(decoder.client_buffers.c2d.opens == opens + 1);
    assert(decoder.deinit_output() == OMX_ErrorNone);
    resources.empty();
}
static void failure_test() {
    for (unsigned live_count : {0U, 1U, 2U}) for (int failure : {0, ION_IOC_ALLOC, ION_IOC_MAP, 4}) {
        omx_vdec decoder;
        for (unsigned count = 0; count < live_count; ++count) allocate(decoder);
        const auto prior_fds = resources.fds;
        const auto prior_mappings = resources.mappings;
        if (failure == 0) resources.fail_open = true;
        else if (failure == 4) resources.fail_mmap = true;
        else resources.fail_ioctl = failure;
        OMX_BUFFERHEADERTYPE *header = reinterpret_cast<OMX_BUFFERHEADERTYPE *>(1);
        assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
        assert(!header);
        assert(decoder.m_out_bm_count == ((1UL << live_count) - 1));
        assert(decoder.client_buffers.allocated_count == live_count);
        assert(resources.fds == prior_fds);
        assert(resources.mappings == prior_mappings);
        if (live_count == 0) {
            const unsigned calls = decoder.driver_allocations;
            decoder.requirements_failure = true;
            assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
            assert(decoder.driver_allocations == calls);
            decoder.requirements_failure = false;
        }
        allocate(decoder);
        assert(decoder.deinit_output() == OMX_ErrorNone);
        resources.empty();
    }
}
static void descriptor_and_deinit_test() {
    for (bool conversion : {false, true}) {
        omx_vdec decoder;
        decoder.client_buffers.enabled = conversion;
        auto *first = allocate(decoder);
        if (conversion) resources.export_fd_zero = true;
        allocate(decoder);
        if (conversion) assert(decoder.client_buffers.pmem_fd[1] == 0);
        allocate(decoder);
        release(decoder, first, 0);
        assert(decoder.deinit_output() == OMX_ErrorNone);
        resources.empty();
    }
}
static void registration_lifecycle_test() {
    omx_vdec decoder;
    auto *first = allocate(decoder);
    auto *second = allocate(decoder);
    resources.fail_driver_ioctl = VIDIOC_STREAMON;
    OMX_BUFFERHEADERTYPE *third = nullptr;
    const unsigned before_prepare = resources.prepare_calls;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &third, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
    assert(!third);
    assert(resources.prepare_calls == before_prepare + 1);
    assert(decoder.m_out_bm_registered == 7);
    assert(decoder.m_out_bm_count == 3);
    third = allocate(decoder);
    assert(resources.prepare_calls == before_prepare + 1);
    assert(decoder.streaming[CAPTURE_PORT]);
    release(decoder, first, 0);
    release(decoder, second, 1);
    first = allocate(decoder);
    assert(!decoder.streaming[CAPTURE_PORT]);
    second = allocate(decoder);
    assert(decoder.streaming[CAPTURE_PORT]);
    assert(resources.prepare_calls == before_prepare + 1);
    release(decoder, first, 0);
    release(decoder, second, 1);
    release(decoder, third, 2);
    resources.empty();
}
static void release_failure_test() {
    for (bool fail_streamoff : {false, true}) {
        omx_vdec decoder;
        decoder.drv_ctx.op_buf.actualcount = 1;
        if (fail_streamoff) decoder.streamoff_failures = 2;
        else decoder.release_failures = 2;
        resources.fail_mmap = true;
        OMX_BUFFERHEADERTYPE *header = nullptr;
        assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorHardware);
        assert(!header);
        assert(decoder.client_buffers.driver_release_pending);
        assert(decoder.m_out_mem_ptr);
        const auto *retained_headers = decoder.m_out_mem_ptr;
        const auto retained_fds = resources.fds;
        const auto retained_mappings = resources.mappings;
        const size_t retained_handles = resources.handles.size();
        assert(retained_fds.size() == 2);
        assert(retained_mappings.size() == 1);
        assert(retained_handles == 1);
        assert(decoder.drv_ctx.ptr_outputbuffer[0].pmem_fd >= 0);
        const unsigned calls = decoder.driver_allocations;
        assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorHardware);
        assert(decoder.m_out_mem_ptr == retained_headers);
        assert(resources.fds == retained_fds);
        assert(resources.mappings == retained_mappings);
        assert(resources.handles.size() == retained_handles);
        assert(decoder.driver_allocations == calls);
        allocate(decoder);
        assert(!decoder.client_buffers.driver_release_pending);
        assert(decoder.deinit_output() == OMX_ErrorNone);
        resources.empty();
    }
}
static void live_storage_failure_test() {
    for (unsigned live_count : {1U, 3U}) {
        omx_vdec decoder;
        decoder.drv_ctx.op_buf.actualcount = live_count;
        OMX_BUFFERHEADERTYPE *header = nullptr;
        for (unsigned slot = 0; slot < live_count; ++slot) header = allocate(decoder);
        const auto retained_fds = resources.fds;
        const auto retained_mappings = resources.mappings;
        const size_t retained_handles = resources.handles.size();
        const unsigned long retained_mask = decoder.m_out_bm_count;
        decoder.streamoff_failures = 1;
        const OMX_ERRORTYPE result = decoder.normal_free_output(header);
        assert(result == OMX_ErrorHardware);
        assert(resources.fds == retained_fds);
        assert(resources.mappings == retained_mappings);
        assert(resources.handles.size() == retained_handles);
        assert(decoder.m_out_bm_count == retained_mask);
        assert(decoder.client_buffers.allocated_count == live_count);
        assert(decoder.deinit_output() == OMX_ErrorNone);
        resources.empty();
    }
    {
        omx_vdec decoder;
        decoder.drv_ctx.op_buf.actualcount = 1;
        auto *header = allocate(decoder);
        const auto retained_fds = resources.fds;
        const auto retained_mappings = resources.mappings;
        decoder.release_failures = 1;
        assert(decoder.normal_free_output(header) == OMX_ErrorHardware);
        assert(resources.fds == retained_fds);
        assert(resources.mappings == retained_mappings);
        assert(decoder.m_out_bm_count == 1);
        assert(decoder.deinit_output() == OMX_ErrorNone);
        resources.empty();
    }
    omx_vdec decoder;
    allocate(decoder);
    allocate(decoder);
    const auto retained_fds = resources.fds;
    const auto retained_mappings = resources.mappings;
    decoder.streamoff_failures = 1;
    resources.fail_mmap = true;
    OMX_BUFFERHEADERTYPE *header = nullptr;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorHardware);
    assert(!header);
    assert(resources.fds == retained_fds);
    assert(resources.mappings == retained_mappings);
    assert(decoder.m_out_bm_count == 3);
    assert(decoder.client_buffers.allocated_count == 2);
    allocate(decoder);
    assert(decoder.deinit_output() == OMX_ErrorNone);
    resources.empty();
}
static void forced_shutdown_test() {
    for (bool pending_rollback : {false, true}) {
        omx_vdec decoder;
        decoder.msg_thread_created = true;
        decoder.async_thread_created = true;
        decoder.m_pipe_in = resources.open_fd();
        decoder.m_pipe_out = resources.open_fd();
        decoder.drv_ctx.op_buf.actualcount = 1;
        if (pending_rollback) {
            resources.fail_mmap = true;
            decoder.streamoff_failures = 1;
            OMX_BUFFERHEADERTYPE *header = nullptr;
            assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorHardware);
            assert(decoder.client_buffers.driver_release_pending);
        } else {
            allocate(decoder);
        }
        decoder.streamoff_failures = 3;
        decoder.release_failures = 3;
        resources.shutdown_events.clear();
        assert(decoder.deinit_output() == OMX_ErrorNone);
        const auto &events = resources.shutdown_events;
        const auto message_join = std::find(events.begin(), events.end(), "message-join");
        const auto async_join = std::find(events.begin(), events.end(), "async-join");
        const auto driver_close = std::find(events.begin(), events.end(), "driver-close");
        const auto unmap = std::find(events.begin(), events.end(), "unmap");
        assert(message_join < async_join && async_join < driver_close && driver_close < unmap);
        const auto completed_events = events;
        assert(decoder.shutdown_driver());
        assert(resources.shutdown_events == completed_events);
        assert(decoder.drv_ctx.video_driver_fd == -1);
        resources.empty();
    }
}
static void failed_driver_close_test() {
    omx_vdec decoder;
    allocate(decoder);
    const auto retained_fds = resources.fds;
    const auto retained_mappings = resources.mappings;
    resources.fail_driver_close = true;
    assert(decoder.deinit_output() == OMX_ErrorHardware);
    assert(decoder.driver_shutdown_failed);
    assert(resources.fds == retained_fds);
    assert(resources.mappings == retained_mappings);
    assert(!decoder.shutdown_driver());
    // Fixture recovery releases retained resources; production promises no retry after close error.
    decoder.driver_shutdown_failed = false;
    decoder.drv_ctx.video_driver_fd = 1000000;
    assert(decoder.deinit_output() == OMX_ErrorNone);
    resources.empty();
}
static void boundary_test() {
    omx_vdec decoder;
    decoder.secure_mode = true;
    OMX_BUFFERHEADERTYPE *header = nullptr;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorUnsupportedSetting);
    assert(decoder.driver_allocations == 0);
    decoder.secure_mode = false;
    decoder.client_buffers.c2d.succeeds = false;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
    decoder.client_buffers.c2d.succeeds = true;
    decoder.driver_failure = true;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
    decoder.driver_failure = false;
    decoder.null_driver_header = true;
    assert(decoder.client_buffers.allocate_buffers_color_convert(nullptr, &header, 1, nullptr, 4096) == OMX_ErrorInsufficientResources);
    decoder.null_driver_header = false;
    auto *first = allocate(decoder);
    auto *second = allocate(decoder);
    release(decoder, first, 0);
    assert(decoder.client_buffers.free_output_buffer(first) == OMX_ErrorBadParameter);
    assert(decoder.client_buffers.free_output_buffer(&decoder.m_out_mem_ptr[1]) == OMX_ErrorBadParameter);
    release(decoder, second, 1);
    resources.empty();
}
int main() {
    policy_test();
    reuse_test();
    failure_test();
    boundary_test();
    descriptor_and_deinit_test();
    registration_lifecycle_test();
    release_failure_test();
    live_storage_failure_test();
    forced_shutdown_test();
    failed_driver_close_test();
    std::cout << "PASS: heap policy, middle-slot reuse, immutable mapping lengths, 12 allocation failure/retry cases, registration, storage retention, ordered shutdown and rejection boundaries\n";
}
