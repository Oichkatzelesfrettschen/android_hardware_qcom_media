# Q6 and C2D buffer ownership

The decoder sends input/output and firmware allocations to the Q6 ADSP heap.
C2D imports its separate destination through KGSL, so the destination uses the
same system/IOMMU heap as the non-Q6 conversion path. The allocation-consumer
argument defaults to the decoder; only the C2D destination opts into the GPU
consumer. Secure-heap precedence, flags, alignment and firmware buffer counts
retain their existing rules.

Conversion slots follow driver-header indexes. A middle-slot free therefore
allows allocation into the same free slot while later slots remain live. Each
slot records its CPU mapping length. A driver-registration bitmap distinguishes
client-header ownership from the kernel's persistent static FD/offset
registration. Reusing a slot preserves that registration and avoids a duplicate
`PREPARE_BUF`. Capture starts when every required slot has client ownership,
including when the last repopulated slot has a middle index. Reconfiguration can
change the next allocation's requirements while earlier mappings retain their
original lengths.
The final converted-buffer free closes C2D and resets requirements; the next
allocation queries fresh conversion requirements. Final release and rollback
record the V4L2 requirement reset. The next allocation re-establishes the
existing driver requirements before acquiring another slot; a failed setup
keeps the retry pending.

Destination allocation failures release the acquired driver slot. A failed
STREAMOFF or final REQBUFS release returns `OMX_ErrorHardware` before releasing
pool mappings, FDs, ION handles or extradata. C2D destinations survive until the
driver release succeeds. Normal `FreeBuffer` restores the client-ownership bit
on failure. Allocation retries finish pending cleanup before restoring driver
requirements and acquiring another slot. The final successful driver-slot
release unmaps and closes the shared output pool using its allocation length;
surviving slots keep the pool.

Forced component teardown closes the message pipes, joins the message thread,
requests decoder STOP, joins the asynchronous thread and closes the driver FD
before freeing input/output storage. The destructor shares the idempotent
`shutdown_driver` helper. Conversion cleanup selects the client-header array.
Imported/native buffers retain the existing storage-release branch after the
shutdown boundary. Unexpected driver-close failure records a terminal error and
returns before freeing buffer storage; retrying close could target a recycled
FD. The OMX core deletes the component after deinit returns, so the error path
provides storage retention rather than a retryable component instance.

## Host gate

Run from the media checkout with the intended Python interpreter selected:

```sh
PYTHON=$(command -v python3)
"$PYTHON" mm-video-v4l2/vidc/vdec/tests/q6-c2d-ownership/run.py \
  --cxx clang++ --output /absolute/build-output/q6-c2d-ownership-clang
"$PYTHON" mm-video-v4l2/vidc/vdec/tests/q6-c2d-ownership/run.py \
  --cxx g++ --output /absolute/build-output/q6-c2d-ownership-gcc
```

`run.py` extracts the production conversion class declaration, allocation,
cleanup, requirement refresh, static registration, capture start, header
translation, output-release and driver-shutdown functions. The fixture also
executes the exact normal output `FreeBuffer` branch and the shutdown/output
cleanup prefix from `component_deinit`. The extraction leaves the function bodies unchanged. The host boundary models
Android types, driver-slot allocation, C2D size queries and ION/ioctl/mmap calls.
Its static-driver ledger rejects a duplicate FD/offset registration, preserves
registrations across individual frees, and clears registrations on successful
REQBUFS release. The fixture executes the production prepare helper against
that boundary; the helper preserves registration across a STREAMON failure.
Every modeled FD, ION handle and CPU mapping has an ownership ledger. Duplicate
releases, mismatched unmap lengths and resources left after teardown fail the
gate. The host models `OMX_U32` as pointer-sized to hold Android platform-private
pointers; the fixture supplies semantic ownership evidence rather than ARM ABI
or binary-layout evidence.

Both compiler commands use `-Wall -Wextra -Werror` and AddressSanitizer plus
UndefinedBehaviorSanitizer. Each output directory retains the generated source,
executable and a receipt with compiler version, exact command and source hashes.

The fixture covers:

- 24 combinations of Q6/non-Q6, secure/nonsecure, cached/secure/default flags,
  and decoder/C2D consumer, including alignment rounding and allocation length.
- Three live destination slots, free-middle/reallocate, driver/client header
  translation, pool lifetime and independent destination storage.
- Requirement changes with old and new mappings live together, final release,
  fresh requirement discovery and another allocation cycle.
- Twelve destination failure/retry combinations: ION open, allocation, export
  and CPU mmap failure with zero, one or two surviving driver slots. Four
  additional checks inject failure while restoring released driver requirements
  and require a later successful retry before allocating a slot.
- Descriptor zero, duplicate free, incorrect header domain, failed requirement
  query, driver allocation failure, null driver header, secure-conversion
  rejection and forced teardown with conversion enabled or disabled.
- Partial pool repopulation, middle-index capture restart, STREAMON failure and
  retry without duplicate static registration.
- STREAMOFF failure with surviving or final slots, final REQBUFS failure,
  complete storage retention and pending cleanup retries before allocation.
- Normal `FreeBuffer` ownership restoration after failed driver cleanup.
- Forced teardown ordering: message-thread join before asynchronous-thread join,
  driver close before unmap, pending rollback cleanup, and idempotent shutdown.
- Unexpected driver-close failure with error propagation and retained storage.
  A fixture-only reset releases test resources after checking the terminal error;
  the reset supplies test cleanup rather than production recovery.

The driver allocation boundary deliberately isolates destination rollback from
pre-existing allocation/prepare/stream-on failures inside
`omx_vdec::allocate_output_buffer`. The fixture does not validate that function's
pool-allocation failures. Its extracted prepare helper exercises registration
and STREAMON success/failure, while kernel failures after an import enters
`registered_bufs` remain outside the host model. In the device kernel,
`msm_vidc_prepare_buf` invokes `msm_vdec_prepare_buf` directly; subsequent
`msm_vdec_qbuf` invokes `vb2_qbuf`, which needs the restored REQBUFS slots.
`msm_v4l2_reqbufs` returns the queue-reset result after logging any firmware
release error, so an ioctl success alone cannot prove firmware deregistration.
The fixture also excludes V4L2 firmware execution, KGSL import, GPU drawing,
pixels, fences and concurrent callback scheduling.

The shutdown helper preserves the inherited STOP/join sequence. The asynchronous
poll uses `POLL_TIMEOUT = 0x7fffffff`; failed STOP can leave the join waiting for a
long interval. The host models completed joins and therefore leaves STOP-failure
wakeup and bounded teardown latency to a separate kernel/device gate. Supported
kernel V4L2 file operations omit a `flush` method, so a valid exclusively owned
driver FD follows the successful close path. An invalid FD or unexpected close
error remains a terminal contract violation with retained storage.

## Android and device gates

Compile the actual msm8226 decoder translation unit with the product headers,
32-bit ABI and warning policy before packaging. Run matched base/fix Q6 decoding
with the same firmware, media sample and transport. Capture the full kernel log,
component allocation-consumer logs, all firmware buffer requirements and
`/sys/kernel/debug/ion/heaps/adsp` at configure, stream-on, first output and close.
Compare frame pixels, stride/crop, order, timestamps and EOS. Exercise close/open,
flush, early stop, allocation failure, output reconfiguration and camera/audio
coexistence. Compare ByteBuffer and Surface paths separately because imported
native output can bypass C2D allocation.

The ADSP reduction equals the sum of live, page-rounded C2D destination
allocations when conversion is active. Initial stream-on precedes allocation of
the final C2D destination; the simultaneous live set at scratch allocation can
therefore contain fewer destinations than the steady-state pool. The source
repair alone proves neither 720p decoding nor sufficient contiguous space for the retained 17,993,728-byte
scratch request. A run without C2D destinations falsifies C2D allocation as the
cause of that run's ADSP pressure. Pixel, import, fence or lifecycle regressions
reject the candidate even when frame counts and EOS pass.
