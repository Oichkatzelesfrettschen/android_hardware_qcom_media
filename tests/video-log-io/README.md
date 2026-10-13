# Encoder logging I/O contracts

The fixture extracts the complete output, extradata, and input logging methods
from `video_encoder_device_v4l2.cpp`. The compiler builds those method bodies
unchanged against controlled libc I/O endpoints. The tests reject filenames
whose formatting fails or reaches the array capacity before file creation,
reject unsupported output/extradata filename codecs, and reject MAP_FAILED
before buffer writes or unmapping.

The fixture also checks successful retry, existing-stream reuse, four codec
extensions, visible NV12 row bytes, direct-buffer ownership, secure input/output
rejection, and disabled extradata logging. `snprintf` delegates to host
`vsnprintf` for normal and actual long-directory cases. Injected return values
cover negative, exact-capacity, capacity-minus-one, and larger results. A
negative result fills the destination with unterminated bytes; the rejection
checks require the retained filename to clear before file I/O.

Run from the media repository with a fresh host-local output directory:

```sh
PYTHON=${PYTHON:-python3}
"$PYTHON" tests/video-log-io/run.py --source "$PWD" --output "$VIDEO_LOG_TEST_OUTPUT"
```

Pass `--sanitize` for ASan and UBSan. The runner builds and executes every case
under both GCC and Clang. The receipt binds the source and extracted-function
hashes, compile commands, and execution results. The runner retains failed
compiler and runtime output and marks a receipt passed only after both runs
succeed.

The host fixture models the required class fields and uses a small padded NV12
layout with hand-derived expected bytes. The fixture does not establish the
Android ABI, device mapping behavior, or hardware layout. The Android encoder
translation-unit gate complements the host checks. The host compile retains
`-Wno-sign-compare` from the production build because the extracted input row
loops compare signed counters with unsigned dimensions; all other enabled
warnings remain errors. Stubbed file and mapping operations carry their real
ownership boundaries without touching devices or creating logging files.
