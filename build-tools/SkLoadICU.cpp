// Copyright 2026 Lightpanda authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "SkLoadICU.h"

#if defined(_WIN32) && defined(SK_USING_THIRD_PARTY_ICU)

#include <cstdio>
#include <mutex>

#include "unicode/udata.h"

extern "C" const unsigned char lp_skia_icudtl_dat[];

static bool init_icu(const void* data) {
    UErrorCode error = U_ZERO_ERROR;
    udata_setCommonData(data, &error);
    if (error != U_ZERO_ERROR) {
        std::fprintf(stderr, "udata_setCommonData() returned %d.\n", (int)error);
        return false;
    }

    udata_setFileAccess(UDATA_NO_FILES, &error);
    if (error != U_ZERO_ERROR) {
        std::fprintf(stderr, "udata_setFileAccess() returned %d.\n", (int)error);
        return false;
    }
    return true;
}

bool SkLoadICU() {
    static bool good = false;
    static std::once_flag flag;
    std::call_once(flag, []() { good = init_icu(lp_skia_icudtl_dat); });
    return good;
}

#endif  // defined(_WIN32) && defined(SK_USING_THIRD_PARTY_ICU)
