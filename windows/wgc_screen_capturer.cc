#include "wgc_screen_capturer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "rtc_video_frame.h"

namespace flutter_webrtc_plugin {
namespace {

using namespace libwebrtc;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using winrt::check_hresult;
using winrt::com_ptr;

struct MonitorSearch {
  std::wstring device;
  HMONITOR monitor = nullptr;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
  auto* search = reinterpret_cast<MonitorSearch*>(parameter);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info) && search->device == info.szDevice) {
    search->monitor = monitor;
    return FALSE;
  }
  return TRUE;
}

HMONITOR ResolveMonitor(const std::string& id) {
  size_t consumed = 0;
  const auto index = std::stoul(id, &consumed);
  if (consumed != id.size() || index > MAXDWORD) return nullptr;
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  if (!EnumDisplayDevicesW(nullptr, static_cast<DWORD>(index), &device, 0) ||
      !(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
    return nullptr;
  }
  MonitorSearch search{device.DeviceName, nullptr};
  EnumDisplayMonitors(nullptr, nullptr, FindMonitor,
                      reinterpret_cast<LPARAM>(&search));
  return search.monitor;
}

// Keep capture, video processing and readback on the display's own adapter.
com_ptr<IDXGIAdapter1> MonitorAdapter(HMONITOR monitor) {
  com_ptr<IDXGIFactory1> factory;
  check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
  for (UINT adapter_index = 0;; ++adapter_index) {
    com_ptr<IDXGIAdapter1> adapter;
    const auto adapter_result = factory->EnumAdapters1(adapter_index, adapter.put());
    if (adapter_result == DXGI_ERROR_NOT_FOUND) break;
    check_hresult(adapter_result);
    for (UINT output_index = 0;; ++output_index) {
      com_ptr<IDXGIOutput> output;
      const auto output_result = adapter->EnumOutputs(output_index, output.put());
      if (output_result == DXGI_ERROR_NOT_FOUND) break;
      check_hresult(output_result);
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        return adapter;
      }
    }
  }
  return nullptr;
}

struct CaptureResources {
  Direct3D11CaptureFramePool pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  ~CaptureResources() {
    // Close also on device loss or readback errors, before uninitializing COM.
    try { if (session) session.Close(); } catch (...) {}
    try { if (pool) pool.Close(); } catch (...) {}
  }
};

class Nv12Converter {
 public:
  explicit Nv12Converter(com_ptr<ID3D11Device> device) : device_(device) {
    device_->GetImmediateContext(context_.put());
    video_device_ = device_.as<ID3D11VideoDevice>();
    video_context_ = context_.as<ID3D11VideoContext>();
  }

  void Convert(ID3D11Texture2D* input, int width, int height,
               std::vector<uint8_t>& pixels) {
    // NV12 requires even dimensions. Discard at most one edge pixel.
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) winrt::throw_hresult(E_INVALIDARG);
    if (width != width_ || height != height_) Configure(width, height);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    com_ptr<ID3D11VideoProcessorInputView> input_view;
    check_hresult(video_device_->CreateVideoProcessorInputView(
        input, enumerator_.get(), &input_desc, input_view.put()));
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.get();
    check_hresult(video_context_->VideoProcessorBlt(
        processor_.get(), output_view_.get(), 0, 1, &stream));
    context_->CopyResource(staging_.get(), output_.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto* y = static_cast<const uint8_t*>(mapped.pData);
    const auto* uv = y + static_cast<size_t>(mapped.RowPitch) * height;
    const size_t y_size = static_cast<size_t>(width) * height;
    // The caller allocates before Map, so allocation failure cannot leave a
    // resource mapped. The caller keeps this buffer across frames.
    for (int row = 0; row < height; ++row) {
      std::memcpy(pixels.data() + static_cast<size_t>(row) * width,
                  y + static_cast<size_t>(row) * mapped.RowPitch, width);
    }
    auto* u = pixels.data() + y_size;
    auto* v = u + y_size / 4;
    for (int row = 0; row < height / 2; ++row) {
      const auto* line = uv + static_cast<size_t>(row) * mapped.RowPitch;
      const size_t offset = static_cast<size_t>(row) * (width / 2);
      for (int column = 0; column < width / 2; ++column) {
        u[offset + column] = line[column * 2];
        v[offset + column] = line[column * 2 + 1];
      }
    }
    context_->Unmap(staging_.get(), 0);
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  void Configure(int width, int height) {
    output_view_ = nullptr;
    staging_ = nullptr;
    output_ = nullptr;
    processor_ = nullptr;
    enumerator_ = nullptr;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.InputWidth = content.OutputWidth = width;
    content.InputHeight = content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check_hresult(video_device_->CreateVideoProcessorEnumerator(
        &content, enumerator_.put()));
    check_hresult(video_device_->CreateVideoProcessor(
        enumerator_.get(), 0, processor_.put()));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check_hresult(device_->CreateTexture2D(&desc, nullptr, output_.put()));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
    view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    check_hresult(video_device_->CreateVideoProcessorOutputView(
        output_.get(), enumerator_.get(), &view_desc, output_view_.put()));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check_hresult(device_->CreateTexture2D(&desc, nullptr, staging_.put()));
    video_context_->VideoProcessorSetStreamAutoProcessingMode(
        processor_.get(), 0, FALSE);
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.Nominal_Range = 2;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
    // RTCVideoFrame's public API carries no color-space metadata. Match the
    // SDK's libyuv I420 converters (BT.601) rather than silently mixing matrices.
    yuv.YCbCr_Matrix = 0;
    yuv.Nominal_Range = 1;
    video_context_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &rgb);
    video_context_->VideoProcessorSetOutputColorSpace(processor_.get(), &yuv);
    width_ = width;
    height_ = height;
  }

  int width_ = 0;
  int height_ = 0;
  com_ptr<ID3D11Device> device_;
  com_ptr<ID3D11DeviceContext> context_;
  com_ptr<ID3D11VideoDevice> video_device_;
  com_ptr<ID3D11VideoContext> video_context_;
  com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
  com_ptr<ID3D11VideoProcessor> processor_;
  com_ptr<ID3D11Texture2D> output_;
  com_ptr<ID3D11Texture2D> staging_;
  com_ptr<ID3D11VideoProcessorOutputView> output_view_;
};

class WgcScreenCapturer : public RTCVideoCapturer {
 public:
  WgcScreenCapturer(HMONITOR monitor, int fps, bool cursor,
                    scoped_refptr<RTCVideoSource> source,
                    std::function<void()> stopped)
      : monitor_(monitor), fps_(fps), cursor_(cursor), source_(source),
        stopped_(std::move(stopped)) {}
  ~WgcScreenCapturer() override { StopCapture(); }

  bool StartCapture() override {
    if (running_) return true;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    std::promise<bool> started;
    auto result = started.get_future();
    worker_ = std::thread([this, promise = std::move(started)]() mutable {
      Run(std::move(promise));
    });
    const bool success = result.get();
    started_success_ = success;
    if (!success) StopCapture();
    return success;
  }
  bool CaptureStarted() override { return running_; }
  uint64_t FreshFrameCount() const { return fresh_frames_; }
  WgcCaptureStats Stats() const {
    return {running_.load(), fresh_frames_.load(), delivered_frames_.load(),
            error_.load()};
  }
  void StopCapture() override {
    running_ = false;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (started_success_ && stopped_) {
      auto callback = std::move(stopped_);
      callback();
    }
  }

 private:
  void Run(std::promise<bool> started) noexcept {
    bool reported = false;
    bool apartment = false;
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      apartment = true;
      {
        if (!GraphicsCaptureSession::IsSupported()) {
          winrt::throw_hresult(E_NOTIMPL);
        }
        auto interop = winrt::get_activation_factory<
            GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        check_hresult(interop->CreateForMonitor(
            monitor_, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        auto adapter = MonitorAdapter(monitor_);
        com_ptr<ID3D11Device> device;
        check_hresult(D3D11CreateDevice(
            adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
        auto dxgi = device.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.get(), inspectable.put()));
        auto runtime_device = inspectable.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        auto size = item.Size();
        CaptureResources resources;
        auto& pool = resources.pool;
        auto& session = resources.session;
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            runtime_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession",
                L"IsCursorCaptureEnabled")) {
          session.IsCursorCaptureEnabled(cursor_);
        } else if (!cursor_) {
          winrt::throw_hresult(E_NOTIMPL);
        }
        session.StartCapture();
        Nv12Converter converter(device);
        std::vector<uint8_t> pixels;
        const auto period = std::chrono::microseconds(1000000 / fps_);
        auto deadline = std::chrono::steady_clock::now();
        const auto first_deadline = deadline + std::chrono::seconds(3);
        while (running_) {
          auto frame = pool.TryGetNextFrame();
          if (frame) {
            auto content = frame.ContentSize();
            if (content.Width != size.Width || content.Height != size.Height) {
              size = content;
              frame.Close();
              pixels.clear();
              pool.Recreate(runtime_device,
                            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
              continue;
            }
            auto access = frame.Surface().as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            com_ptr<ID3D11Texture2D> texture;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),
                                                texture.put_void()));
            const size_t count = static_cast<size_t>(size.Width & ~1) *
                                 (size.Height & ~1) * 3 / 2;
            pixels.resize(count);
            converter.Convert(texture.get(), size.Width, size.Height, pixels);
            ++fresh_frames_;
            frame.Close();
          }
          if (!pixels.empty()) {
            if (!reported) {
              started.set_value(true);
              reported = true;
            }
            // Create copies the planes, so the next GPU readback cannot mutate
            // a frame still being encoded or displayed by another thread.
            source_->OnCapturedFrame(RTCVideoFrame::Create(
                converter.width(), converter.height(), pixels.data(),
                static_cast<int>(pixels.size())));
            ++delivered_frames_;
          } else if (std::chrono::steady_clock::now() >= first_deadline) {
            winrt::throw_hresult(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
          }
          deadline += period;
          deadline = (std::max)(deadline, std::chrono::steady_clock::now());
          std::unique_lock<std::mutex> lock(wait_mutex_);
          wake_.wait_until(lock, deadline, [this] { return !running_; });
        }
      }
    } catch (const winrt::hresult_error& error) {
      error_ = error.code().value;
      std::fprintf(stderr, "WGC capture failed: HRESULT=0x%08lx\n",
                    static_cast<unsigned long>(error.code().value));
    } catch (...) {
      error_ = E_FAIL;
      // Start failures fall back to the bundled backend. Runtime failures stop
      // producing frames; track disposal still joins this worker safely.
    }
    if (apartment) winrt::uninit_apartment();
    running_ = false;
    if (!reported) started.set_value(false);
  }

  HMONITOR monitor_;
  int fps_;
  bool cursor_;
  scoped_refptr<RTCVideoSource> source_;
  std::function<void()> stopped_;
  bool started_success_ = false;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> fresh_frames_{0};
  std::atomic<uint64_t> delivered_frames_{0};
  std::atomic<int32_t> error_{0};
  std::thread worker_;
  std::mutex wait_mutex_;
  std::condition_variable wake_;
};

}  // namespace

uint64_t WgcFreshFrameCount(RTCVideoCapturer* capture) {
  auto* wgc = dynamic_cast<WgcScreenCapturer*>(capture);
  return wgc ? wgc->FreshFrameCount() : 0;
}

bool ReadWgcCaptureStats(RTCVideoCapturer* capture, WgcCaptureStats* stats) {
  auto* wgc = dynamic_cast<WgcScreenCapturer*>(capture);
  if (!wgc || !stats) return false;
  *stats = wgc->Stats();
  return true;
}

scoped_refptr<RTCVideoCapturer> CreateWgcScreenCapturer(
    const std::string& source_id, int fps, bool cursor,
    scoped_refptr<RTCVideoSource> source, std::function<void()> stopped) {
  try {
    const auto monitor = ResolveMonitor(source_id);
    if (!monitor || fps <= 0 || !source) return nullptr;
    auto capture = scoped_refptr<RTCVideoCapturer>(
        new RefCountedObject<WgcScreenCapturer>(
            monitor, (std::min)(fps, 60), cursor, source, std::move(stopped)));
    if (capture->StartCapture()) return capture;
  } catch (...) {
    // Invalid source ids and unavailable WinRT/D3D devices use the old path.
  }
  return nullptr;
}

}  // namespace flutter_webrtc_plugin
