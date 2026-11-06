#pragma once

// Installed identity and persistent storage are selected per toolchain.
// Repository paths and build artifact names intentionally remain unchanged.
#ifdef PSL1GHT
#define CRTUBE_DATA_DIR "/dev_hdd0/tmp/crtube"
#define CRTUBE_LOG_PATH "/dev_hdd0/tmp/dbg_crt.txt"
#else
#define CRTUBE_DATA_DIR "/dev_hdd0/tmp/crtube_sdk"
#define CRTUBE_LOG_PATH "/dev_hdd0/tmp/dbg_crt_sdk.txt"
#endif
