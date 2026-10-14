// Encodes numbered 1280x720 PNG frames into a .wmv laid out like the game's movWW/frpg_title.wmv: WMV9 (WMV3)
// video at 30000/1001 fps and a silent 48 kHz stereo WMA2 track, written with Windows' own Media Foundation
// encoders and ASF sink. The game plays the file through DirectShow (WM ASF Reader, WMVideo Decoder DMO), so a
// file from Microsoft's encoder keeps to the original's decoder path; ffmpeg's ASF files lack the frame rate in
// the stream header, and a stream copy of the original's audio is refused by the reader. Built and run by
// tools/build_title_movie.bat.
// usage: wmvenc <frame pattern, e.g. dir\f%04d.png (numbered from 1)> <frame count> <out.wmv>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

#define CHECK(x)                                                                             \
    do {                                                                                     \
        const HRESULT hr_ = (x);                                                             \
        if (FAILED(hr_)) {                                                                   \
            std::printf("wmvenc: 0x%08lX at line %d: %s\n", (unsigned long)hr_, __LINE__, #x); \
            return 1;                                                                        \
        }                                                                                    \
    } while (0)

namespace {

constexpr UINT32 kWidth = 1280, kHeight = 720, kFpsNum = 30000, kFpsDen = 1001;
constexpr UINT32 kVideoBitrate = 10000000;  // the original's nominal rate; the encoder spends far less on this
constexpr UINT32 kAudioRate = 48000, kChannels = 2, kAudioBytesPerSec = 24000;  // 192 kb/s like the original

bool load_frame(IWICImagingFactory* wic, const wchar_t* path, std::vector<BYTE>* bgrx) {
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w == kWidth && h == kHeight;
    if (ok) {
        bgrx->resize(kWidth * kHeight * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, kWidth * 4, static_cast<UINT>(bgrx->size()), bgrx->data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    dec->Release();
    return ok;
}

HRESULT write_buffer(IMFSinkWriter* sw, DWORD stream, const void* data, DWORD size, LONGLONG time, LONGLONG duration) {
    IMFMediaBuffer* buf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(size, &buf);
    if (FAILED(hr)) return hr;
    BYTE* p = nullptr;
    hr = buf->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        if (data) std::memcpy(p, data, size);
        else std::memset(p, 0, size);
        buf->Unlock();
        hr = buf->SetCurrentLength(size);
    }
    IMFSample* sample = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) hr = sw->WriteSample(stream, sample);
    if (sample) sample->Release();
    buf->Release();
    return hr;
}

// The WMA2 encoder's 48 kHz stereo type at 192 kb/s.
IMFMediaType* pick_audio_type() {
    IMFCollection* offers = nullptr;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_WMAudioV8, MFT_ENUM_FLAG_ALL, nullptr, &offers))) return nullptr;
    DWORD n = 0;
    offers->GetElementCount(&n);
    IMFMediaType* found = nullptr;
    for (DWORD i = 0; i < n && !found; ++i) {
        IUnknown* u = nullptr;
        if (FAILED(offers->GetElement(i, &u))) continue;
        IMFMediaType* t = nullptr;
        u->QueryInterface(IID_PPV_ARGS(&t));
        u->Release();
        if (!t) continue;
        UINT32 rate = 0, ch = 0, bytes = 0;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
        if (rate == kAudioRate && ch == kChannels && bytes == kAudioBytesPerSec) found = t;
        else t->Release();
    }
    offers->Release();
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: wmvenc <frame pattern> <frame count> <out.wmv>\n");
        return 2;
    }
    const int count = std::atoi(argv[2]);
    wchar_t pattern[MAX_PATH], out[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, pattern, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, out, MAX_PATH);
    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK(MFStartup(MF_VERSION));
    IWICImagingFactory* wic = nullptr;
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));

    IMFAttributes* attrs = nullptr;
    CHECK(MFCreateAttributes(&attrs, 2));
    CHECK(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    CHECK(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE));
    DeleteFileW(out);
    IMFSinkWriter* sw = nullptr;
    CHECK(MFCreateSinkWriterFromURL(out, nullptr, attrs, &sw));  // .wmv: the ASF sink

    IMFMediaType* vout = nullptr;
    CHECK(MFCreateMediaType(&vout));
    CHECK(vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK(vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_WMV3));
    CHECK(vout->SetUINT32(MF_MT_AVG_BITRATE, kVideoBitrate));
    CHECK(vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK(MFSetAttributeSize(vout, MF_MT_FRAME_SIZE, kWidth, kHeight));
    CHECK(MFSetAttributeRatio(vout, MF_MT_FRAME_RATE, kFpsNum, kFpsDen));
    CHECK(MFSetAttributeRatio(vout, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    DWORD video = 0;
    CHECK(sw->AddStream(vout, &video));
    IMFMediaType* vin = nullptr;
    CHECK(MFCreateMediaType(&vin));
    CHECK(vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK(vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    CHECK(vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK(MFSetAttributeSize(vin, MF_MT_FRAME_SIZE, kWidth, kHeight));
    CHECK(MFSetAttributeRatio(vin, MF_MT_FRAME_RATE, kFpsNum, kFpsDen));
    CHECK(MFSetAttributeRatio(vin, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    CHECK(vin->SetUINT32(MF_MT_DEFAULT_STRIDE, kWidth * 4));  // rows top to bottom, as WIC gives them
    CHECK(sw->SetInputMediaType(video, vin, nullptr));

    IMFMediaType* aout = pick_audio_type();
    if (!aout) {
        std::printf("wmvenc: the WMA2 encoder offers no 48 kHz stereo 192 kb/s type\n");
        return 1;
    }
    DWORD audio = 0;
    CHECK(sw->AddStream(aout, &audio));
    IMFMediaType* ain = nullptr;
    CHECK(MFCreateMediaType(&ain));
    CHECK(ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK(ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK(ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate));
    CHECK(ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels));
    CHECK(ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    CHECK(ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2));
    CHECK(ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * kChannels * 2));
    CHECK(ain->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK(sw->SetInputMediaType(audio, ain, nullptr));

    CHECK(sw->BeginWriting());
    std::vector<BYTE> px;
    LONGLONG samples = 0;  // silent audio written so far
    for (int i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, pattern, i + 1);
        if (!load_frame(wic, path, &px)) {
            std::printf("wmvenc: frame %d could not be read as a %ux%u image\n", i + 1, kWidth, kHeight);
            return 1;
        }
        const LONGLONG t0 = 10000000LL * i * kFpsDen / kFpsNum, t1 = 10000000LL * (i + 1) * kFpsDen / kFpsNum;
        CHECK(write_buffer(sw, video, px.data(), static_cast<DWORD>(px.size()), t0, t1 - t0));
        const LONGLONG until = static_cast<LONGLONG>(kAudioRate) * (i + 1) * kFpsDen / kFpsNum;
        while (samples < until) {
            const LONGLONG n = min(until - samples, static_cast<LONGLONG>(kAudioRate / 10));
            CHECK(write_buffer(sw, audio, nullptr, static_cast<DWORD>(n * kChannels * 2), samples * 10000000LL / kAudioRate,
                               n * 10000000LL / kAudioRate));
            samples += n;
        }
    }
    CHECK(sw->Finalize());
    sw->Release();
    MFShutdown();
    std::printf("wmvenc: wrote %d frames and %.3f s of silent audio to %s\n", count, samples / double(kAudioRate), argv[3]);
    return 0;
}
