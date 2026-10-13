#undef snprintf
#undef fopen
#undef fwrite
#undef mmap
#undef munmap

enum class channel { output, extradata, input };
static int log_buffer(venc_dev &device, channel selected) {
    OMX_BUFFERHEADERTYPE buffer = {input, sizeof(input)};
    switch (selected) {
    case channel::output: return device.venc_output_log_buffers(encoded, 7);
    case channel::extradata: return device.venc_extradata_log_buffers(reinterpret_cast<char *>(&extra));
    case channel::input: return device.venc_input_log_buffers(&buffer, 23, 4096);
    }
    std::abort();
}
static char *filename(venc_dev &device, channel selected) {
    switch (selected) {
    case channel::output: return device.m_debug.outfile_name;
    case channel::extradata: return device.m_debug.extradatafile_name;
    case channel::input: return device.m_debug.infile_name;
    }
    std::abort();
}
static void reset(const char *name) {
    case_name = name;
    io = {};
    for (unsigned index = 0; index < sizeof(mapping); index++) {
        mapping[index] = static_cast<unsigned char>(index);
        input[index] = static_cast<unsigned char>(index + 64);
    }
}
static void rejected_filename(channel selected, int format_result, const char *name) {
    reset(name);
    venc_dev device;
    io.format_result = format_result;
    expect(log_buffer(device, selected) == -1, "invalid filename propagates failure");
    expect(io.opens == 0 && io.writes == 0 && io.maps == 0, "invalid filename stops before file or buffer I/O");
    expect(filename(device, selected)[0] == '\0', "invalid filename clears retained bytes");
    io.format_result = INT_MAX;
    expect(log_buffer(device, selected) == 0, "valid filename retries successfully");
    expect(io.opens == 1, "retry opens exactly one stream");
}
int main() {
    for (channel selected : {channel::output, channel::extradata, channel::input}) {
        rejected_filename(selected, -1, "negative snprintf");
        rejected_filename(selected, PROPERTY_VALUE_MAX, "exact-capacity snprintf");
        rejected_filename(selected, PROPERTY_VALUE_MAX + 17, "truncated snprintf");
        reset("last fitting snprintf");
        venc_dev device;
        io.format_result = PROPERTY_VALUE_MAX - 1;
        expect(log_buffer(device, selected) == 0 && io.opens == 1, "capacity-minus-one filename opens successfully");
        reset("real long-directory truncation");
        venc_dev long_directory;
        std::memset(long_directory.m_debug.log_loc, 'd', PROPERTY_VALUE_MAX - 1);
        long_directory.m_debug.log_loc[PROPERTY_VALUE_MAX - 1] = '\0';
        expect(log_buffer(long_directory, selected) == -1 && io.opens == 0, "actual snprintf truncation avoids fopen");
        expect(filename(long_directory, selected)[0] == '\0', "truncated path clears filename");
        reset("fopen failure and retry");
        venc_dev inaccessible;
        io.open_failure = true;
        expect(log_buffer(inaccessible, selected) == -1, "open failure returns -1");
        expect(io.writes == 0 && io.maps == 0 && filename(inaccessible, selected)[0] == '\0', "open failure clears filename before buffer I/O");
        io.open_failure = false;
        expect(log_buffer(inaccessible, selected) == 0 && io.opens == 2, "open failure retains retry path");
    }
    for (channel selected : {channel::output, channel::extradata}) {
        reset("unsupported codec");
        venc_dev device;
        device.m_sVenc_cfg.codectype = V4L2_PIX_FMT_NV12;
        expect(log_buffer(device, selected) == -1, "unsupported filename codec returns failure");
        expect(io.formats == 0 && io.opens == 0 && io.writes == 0, "unsupported codec leaves stale path unopened");
        expect(filename(device, selected)[0] == '\0', "unsupported codec clears retained filename");
        device.m_sVenc_cfg.codectype = V4L2_PIX_FMT_H264;
        expect(log_buffer(device, selected) == 0 && io.opens == 1, "supported codec retries successfully");
    }
    const unsigned long codecs[] = {V4L2_PIX_FMT_MPEG4, V4L2_PIX_FMT_H264, V4L2_PIX_FMT_H263, V4L2_PIX_FMT_VP8};
    const char *suffixes[] = {".m4v", ".264", ".263", ".ivf"};
    for (channel selected : {channel::output, channel::extradata}) {
        for (unsigned index = 0; index < 4; index++) {
            reset("supported codec filename and stream reuse");
            venc_dev device;
            device.m_sVenc_cfg.codectype = codecs[index];
            expect(log_buffer(device, selected) == 0, "supported codec logs successfully");
            const std::string prefix = selected == channel::output ? "/logs/output_enc_4_2_" : "/logs/extradata_enc_4_2_";
            expect(io.filename.rfind(prefix, 0) == 0, "filename retains directory, role and dimensions");
            expect(io.filename.size() >= std::strlen(suffixes[index]) && io.filename.compare(io.filename.size() - std::strlen(suffixes[index]), std::strlen(suffixes[index]), suffixes[index]) == 0, "filename retains codec extension");
            expect(log_buffer(device, selected) == 0 && io.opens == 1 && io.formats == 1, "second buffer reuses stream");
            expect(io.writes == 2 && io.invalid_writes == 0, "two buffers reach the acquired stream");
        }
    }
    reset("MAP_FAILED and retained-stream retry");
    venc_dev failed_mapping;
    io.map_failure = true;
    expect(log_buffer(failed_mapping, channel::input) == -1, "MAP_FAILED returns failure");
    expect(io.maps == 1 && io.writes == 0 && io.unmaps == 0, "MAP_FAILED reaches neither write nor unmap");
    expect(failed_mapping.m_debug.infile == stderr, "mapping failure retains acquired stream for close or retry");
    io.map_failure = false;
    io.data.clear();
    expect(log_buffer(failed_mapping, channel::input) == 0 && io.opens == 1, "mapping retry reuses stream");
    expect(io.unmaps == 1 && io.invalid_unmaps == 0 && io.invalid_writes == 0, "successful mapping releases exactly one valid mapping");
    const std::vector<unsigned char> mapped_expected = {0,1,2,3,8,9,10,11,32,33,34,35};
    expect(io.data == mapped_expected, "mapped NV12 logging emits visible Y and UV bytes without padding");
    reset("direct input buffer");
    venc_dev direct;
    direct.metadatamode = 0;
    expect(log_buffer(direct, channel::input) == 0 && io.maps == 0 && io.unmaps == 0, "direct input keeps caller buffer ownership");
    const std::vector<unsigned char> direct_expected = {64,65,66,67,72,73,74,75,96,97,98,99};
    expect(io.data == direct_expected, "direct NV12 logging preserves visible plane bytes");
    reset("secure logging rejection");
    venc_dev secure;
    secure.owner.secure = true;
    expect(log_buffer(secure, channel::output) == -1 && log_buffer(secure, channel::input) == -1, "secure input and output reject logging");
    expect(io.opens == 0 && io.maps == 0 && io.writes == 0, "secure rejection precedes I/O");
    reset("disabled extradata");
    venc_dev disabled;
    disabled.m_debug.extradata_log = false;
    expect(log_buffer(disabled, channel::extradata) == 0 && io.opens == 0 && io.writes == 0, "disabled extradata logging remains idle");
    std::printf("checks_failed=%u\n", failures);
    return failures ? 1 : 0;
}
