#ifndef FLUTTER_WEBRTC_WGC_SCREEN_CAPTURER_H_
#define FLUTTER_WEBRTC_WGC_SCREEN_CAPTURER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "rtc_video_device.h"
#include "rtc_video_source.h"

namespace flutter_webrtc_plugin {

// Uses the same display-device indices as WebRTC's Windows screen list.
// A null result leaves the caller free to use the bundled desktop capturer.
libwebrtc::scoped_refptr<libwebrtc::RTCVideoCapturer> CreateWgcScreenCapturer(
    const std::string& source_id, int fps, bool cursor,
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> source,
    std::function<void()> stopped = {});

// Distinguish newly captured desktop images from repeated unchanged frames.
uint64_t WgcFreshFrameCount(libwebrtc::RTCVideoCapturer* capture);

struct WgcCaptureStats {
  bool running;
  uint64_t fresh_frames;
  uint64_t delivered_frames;
  int32_t error;
};
bool ReadWgcCaptureStats(libwebrtc::RTCVideoCapturer* capture,
                        WgcCaptureStats* stats);

}  // namespace flutter_webrtc_plugin

#endif
