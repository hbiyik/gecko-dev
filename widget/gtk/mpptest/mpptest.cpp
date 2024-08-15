/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cstdlib>
#include <fcntl.h>
#include <dlfcn.h>

#if defined(MOZ_ASAN) || defined(FUZZING)
#  include <signal.h>
#endif
#include <unistd.h>
#include <stdarg.h>
#ifdef __SUNPRO_CC
#  include <stdio.h>
#endif

#include "mozilla/GfxInfoUtils.h"

#define OUTPUT_PIPE 1

using MppCtx = void *;

using MppApi = struct MppApi_t {
    uint  size;
    uint  version;
    int *decode;
    int *decode_put_packet;
    int *decode_get_frame;
    int *encode;
    int *encode_put_frame;
    int *encode_get_packet;
    int *isp;
    int *isp_put_frame;
    int *isp_get_frame;
    int *poll;
    int *dequeue;
    int *enqueue;
    int (*reset)(MppCtx ctx);
    int *control;
    uint reserv[16];
};

using MppCtxType = enum {
  MPP_CTX_DEC,
  MPP_CTX_ENC,
};

using MppCodingType = enum {
  MPP_VIDEO_CodingAVC = 0x7,
  MPP_VIDEO_CodingVP8 = 0x9,
  MPP_VIDEO_CodingVP9 = 0xa,
  MPP_VIDEO_CodingAV1 = 0x01000008,
};

using HwAccelCodec = enum {
  CODEC_HW_H264 = 1 << 4,
  CODEC_HW_VP8 = 1 << 5,
  CODEC_HW_VP9 = 1 << 6,
  CODEC_HW_AV1 = 1 << 7,
};

using create_handle = int (*)(MppCtx *ctx, MppApi**);
using check_handle = int (*)(MppCtxType, MppCodingType);
using init_handle = int (*)(MppCtx, MppCtxType, MppCodingType);
using destroy_handle = int (*)(MppCtx);

void* Mpp;
create_handle MppCreate;
check_handle MppCheck;
destroy_handle MppDestroy;
init_handle MppInit;


int fail(const char* msg){
    record_value("ERROR\n%s\n", msg);
    if (Mpp){
      dlclose(Mpp);
    }
    record_flush();
    return EXIT_FAILURE;
}


int main(int argc, char** argv) {
  int supported = 0;
  const char* env = getenv("MOZ_GFX_DEBUG");
  enable_logging = env && *env == '1';
  output_pipe = OUTPUT_PIPE;
  HwAccelCodec hwaccels[] = {CODEC_HW_H264,
                             CODEC_HW_VP8,
                             CODEC_HW_VP9,
                             CODEC_HW_AV1};
  MppCodingType mppcodings[] = {MPP_VIDEO_CodingAVC,
                                MPP_VIDEO_CodingVP8,
                                MPP_VIDEO_CodingVP9,
                                MPP_VIDEO_CodingAV1};

  if (!enable_logging) {
    close_logging();
  }

  log("Testing Mpp\n");

#if defined(MOZ_ASAN) || defined(FUZZING)
  // If handle_segv=1 (default), then glxtest crash will print a sanitizer
  // report which can confuse the harness in fuzzing automation.
  signal(SIGSEGV, SIG_DFL);
#endif

  Mpp = dlopen("librockchip_mpp.so", RTLD_LAZY | RTLD_DEEPBIND);
  if(!Mpp){
    return fail("Can not load mpp library");
  }
  MppCreate = reinterpret_cast<create_handle>(dlsym(Mpp, "mpp_create"));
  if(!MppCreate){
    return fail("Can not bind MppCreate");
  }
  MppCheck = reinterpret_cast<check_handle>(dlsym(Mpp, "mpp_check_support_format"));
  if(!MppCheck){
    return fail("Can not bind MppCheck");
  }
  MppInit = reinterpret_cast<init_handle>(dlsym(Mpp, "mpp_init"));
  if(!MppInit){
    return fail("Can not bind MppInit");
  }
  MppDestroy = reinterpret_cast<destroy_handle>(dlsym(Mpp, "mpp_destroy"));
  if(!MppDestroy){
    return fail("Can not bind MppDestroy");
  }

  log("Mpp Library Loaded\n");

  for (uint i=0; i < sizeof(mppcodings) / sizeof(MppCodingType); i++) {
    MppCtx mppctx = nullptr;
    MppApi *mpi = nullptr;

    if (MppCreate(&mppctx, &mpi)) {
      log("Can not create mpp context for codec id %d", mppcodings[i]);
      continue;
    }

    if(MppCheck(MPP_CTX_DEC, mppcodings[i])){
      log("Mpp does not support codec id %d\n", mppcodings[i]);
      continue;
    }

    if(MppInit(mppctx, MPP_CTX_DEC, mppcodings[i])){
      log("Mpp can not init codec id %d\n", mppcodings[i]);
      continue;
    }

    supported |= hwaccels[i];
    mpi->reset(mppctx);
    MppDestroy(mppctx);
  }

  record_value("SUPPORTED\n%s\nHWCODECS\n%d\n", supported ? "TRUE" : "FALSE", supported);
  dlclose(Mpp);
  record_flush();
  return EXIT_SUCCESS;
}
