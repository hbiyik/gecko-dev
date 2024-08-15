/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#if defined(__NetBSD__) || defined(__OpenBSD__)
#  include <sys/videoio.h>
#elif defined(__sun)
#  include <sys/videodev2.h>
#else
#  include <linux/videodev2.h>
#endif
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <stdint.h>
#include <stdarg.h>

#if defined(MOZ_ASAN) || defined(FUZZING)
#  include <signal.h>
#endif

#include "mozilla/ScopeExit.h"

#ifdef __SUNPRO_CC
#  include <stdio.h>
#endif

#include "mozilla/GfxInfoUtils.h"

// Print test results to stdout and logging to stderr
#define OUTPUT_PIPE 1

using HwAccelCodec = enum {
  CODEC_HW_H264 = 1 << 4,
  CODEC_HW_VP8 = 1 << 5,
  CODEC_HW_VP9 = 1 << 6,
  CODEC_HW_AV1 = 1 << 7,
};

// Enumerate the buffer formats supported on a V4L2 buffer queue.
static int v4l2_supported_fmts(int aFd, int aType) {
  int supported = 0;
  struct v4l2_fmtdesc fmt {};
  fmt.type = aType;
  for (fmt.index = 0;; fmt.index++) {
    int result = ioctl(aFd, VIDIOC_ENUM_FMT, &fmt);
    if (result < 0) {
      supported = result;
      break;
    }
    switch (fmt.pixelformat) {
      case V4L2_PIX_FMT_H264:
        supported |= CODEC_HW_H264;
        break;
      /*
      case V4L2_PIX_FMT_VP8:
        supported |= CODEC_HW_VP8;
        break;
      case V4L2_PIX_FMT_VP9:
        supported |= CODEC_HW_VP9;
        break;
      case V4L2_PIX_FMT_AV1:
      *supported |= CODEC_HW_AV1;
         break;
      */
      case V4L2_PIX_FMT_NV12:
        supported = 1;
        break;
      case V4L2_PIX_FMT_YVU420:
        supported = 1;
    }
  }
  return supported;
}

// Probe a V4L2 device to work out what it supports
static void v4l2_check_device(const char* aVideoDevice) {
  int fd = -1;
  int result = -1;

  log("v4l2test probing device '%s'\n", aVideoDevice);

  auto autoRelease = mozilla::MakeScopeExit([&] {
    if (fd >= 0) {
      close(fd);
    }
  });

  fd = open(aVideoDevice, O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) {
    record_error("V4L2 failed to open device %s: %s", aVideoDevice,
                 strerror(errno));
    return;
  }

  struct v4l2_capability cap {};
  result = ioctl(fd, VIDIOC_QUERYCAP, &cap);
  if (result < 0) {
    record_error("V4L2 device %s failed to query capabilities", aVideoDevice);
    return;
  }
  log("v4l2test driver %s card %s bus_info %s version %d\n", cap.driver,
      cap.card, cap.bus_info, cap.version);

  if (!(cap.capabilities & V4L2_CAP_DEVICE_CAPS)) {
    record_error("V4L2 device %s does not support DEVICE_CAPS", aVideoDevice);
    return;
  }

  if (!(cap.device_caps & V4L2_CAP_STREAMING)) {
    record_error("V4L2 device %s does not support V4L2_CAP_STREAMING",
                 aVideoDevice);
    return;
  }

  // Work out whether the device supports planar or multiplaner bitbuffers and
  // framebuffers
  bool splane = cap.device_caps & V4L2_CAP_VIDEO_M2M;
  bool mplane = cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE;
  if (!splane && !mplane) {
    record_error("V4L2 device %s does not support M2M modes", aVideoDevice);
    // (It's probably a webcam!)
    return;
  }
  // Now check the formats supported for CAPTURE and OUTPUT buffers.
  // For a V4L2-M2M decoder, OUTPUT is actually the bitbuffers we put in and
  // CAPTURE is the framebuffers we get out.
  if (v4l2_supported_fmts(fd, mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                     : V4L2_BUF_TYPE_VIDEO_CAPTURE) <= 0) {
    record_error("V4L2 device %s does not support NV12 or YV12 capture formats",
                 aVideoDevice);
    return;
  }
  int hwcodecs = v4l2_supported_fmts(
      fd,
      mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

  record_value("SUPPORTED\nTRUE\n");
  record_value("HWCODECS\n%s\n", hwcodecs);
}

static void PrintUsage() {
  printf(
      "Firefox V4L2-M2M probe utility\n"
      "\n"
      "usage: v4l2test [options]\n"
      "\n"
      "Options:\n"
      "\n"
      "  -h --help                 show this message\n"
      "  -d --device device        Probe a v4l2 device (e.g. /dev/video10)\n"
      "\n");
}

int main(int argc, char** argv) {
  struct option longOptions[] = {{"help", no_argument, nullptr, 'h'},
                                 {"device", required_argument, nullptr, 'd'},
                                 {nullptr, 0, nullptr, 0}};
  const char* shortOptions = "hd:";
  int c;
  const char* device = nullptr;
  while ((c = getopt_long(argc, argv, shortOptions, longOptions, nullptr)) !=
         -1) {
    switch (c) {
      case 'd':
        device = optarg;
        break;
      case 'h':
      default:
        break;
    }
  }

  if (device) {
#if defined(MOZ_ASAN) || defined(FUZZING)
    // If handle_segv=1 (default), then glxtest crash will print a sanitizer
    // report which can confuse the harness in fuzzing automation.
    signal(SIGSEGV, SIG_DFL);
#endif
    const char* env = getenv("MOZ_GFX_DEBUG");
    enable_logging = env && *env == '1';
    output_pipe = OUTPUT_PIPE;
    if (!enable_logging) {
      close_logging();
    }
    v4l2_check_device(device);
    record_flush();
    return EXIT_SUCCESS;
  }
  PrintUsage();
  return 0;
}
