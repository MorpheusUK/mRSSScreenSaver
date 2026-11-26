// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/graphics.hpp"
#include <algorithm>
#include <cmath>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace mrss
{
namespace
{
using Microsoft::WRL::ComPtr;
void hr(HRESULT h)
{
    if (FAILED(h))
        throw std::runtime_error("Image/graphics error " + std::to_string(static_cast<unsigned long>(h)) +
                                 ". HEIF/WebP may require Windows image extensions.");
}
CLSID decoderId(ImageFormat f)
{
    switch (f)
    {
    case ImageFormat::jpeg:
        return CLSID_WICJpegDecoder;
    case ImageFormat::png:
        return CLSID_WICPngDecoder;
    case ImageFormat::gif:
        return CLSID_WICGifDecoder;
    case ImageFormat::bmp:
        return CLSID_WICBmpDecoder;
    case ImageFormat::tiff:
        return CLSID_WICTiffDecoder;
    case ImageFormat::webp:
        return CLSID_WICWebpDecoder;
    case ImageFormat::heif:
        return CLSID_WICHeifDecoder;
    }
    throw std::runtime_error("Unsupported image type");
}
} // namespace
Pixels decodeImage(const std::filesystem::path &path)
{
    auto bytes = readFile(path, 25 * 1024 * 1024);
    const auto type = identifyImage(bytes);
    if (!type)
        throw std::runtime_error("Unsupported image content");
    ComPtr<IWICImagingFactory2> wic;
    hr(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    ComPtr<IWICStream> stream;
    hr(wic->CreateStream(&stream));
    hr(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(bytes.data()),
                                    static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    const auto id = decoderId(*type);
    // Instantiate only explicitly allowlisted Microsoft decoders.
    hr(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder)));
    hr(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand));
    ComPtr<IWICBitmapFrameDecode> frame;
    hr(decoder->GetFrame(0, &frame));
    UINT w = 0, h = 0;
    hr(frame->GetSize(&w, &h));
    if (!w || !h || w > 32768 || h > 32768 || static_cast<std::uint64_t>(w) * h > 100 * 1024 * 1024)
        throw std::runtime_error("Image exceeds 100-megapixel / 32768-dimension source limit");
    unsigned orientation = 1;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata)))
    {
        for (auto key : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/ifd/{ushort=274}"})
        {
            PROPVARIANT v{};
            if (SUCCEEDED(metadata->GetMetadataByName(key, &v)))
            {
                if (v.vt == VT_UI2)
                    orientation = v.uiVal;
                PropVariantClear(&v);
                break;
            }
        }
    }
    static constexpr WICBitmapTransformOptions transforms[] = {
        WICBitmapTransformRotate0,
        WICBitmapTransformRotate0,
        WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180,
        WICBitmapTransformFlipVertical,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                               WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270};
    ComPtr<IWICBitmapSource> source = frame;
    if (orientation > 1 && orientation <= 8)
    {
        ComPtr<IWICBitmapFlipRotator> rotated;
        hr(wic->CreateBitmapFlipRotator(&rotated));
        hr(rotated->Initialize(frame.Get(), transforms[orientation]));
        source = rotated;
        hr(source->GetSize(&w, &h));
    }
    const double reduction = (std::min)(
        {1.0, 4096.0 / w, 4096.0 / h, std::sqrt(8.0 * 1024 * 1024 / (static_cast<double>(w) * h))});
    if (reduction < 1)
    {
        w = (std::max)(1u, static_cast<unsigned>(w * reduction));
        h = (std::max)(1u, static_cast<unsigned>(h * reduction));
        ComPtr<IWICBitmapScaler> scaler;
        hr(wic->CreateBitmapScaler(&scaler));
        hr(scaler->Initialize(source.Get(), w, h, WICBitmapInterpolationModeFant));
        source = scaler;
    }
    WICPixelFormatGUID format{};
    hr(frame->GetPixelFormat(&format));
    const bool hdr = format == GUID_WICPixelFormat64bppRGBAHalf ||
                     format == GUID_WICPixelFormat128bppRGBAFloat ||
                     format == GUID_WICPixelFormat96bppRGBFloat || format == GUID_WICPixelFormat64bppRGBHalf;
    ComPtr<IWICFormatConverter> convert;
    hr(wic->CreateFormatConverter(&convert));
    hr(convert->Initialize(source.Get(), GUID_WICPixelFormat128bppPRGBAFloat, WICBitmapDitherTypeNone,
                           nullptr, 0, WICBitmapPaletteTypeCustom));
    Pixels result;
    result.width = w;
    result.height = h;
    result.hdr = hdr;
    result.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    hr(convert->CopyPixels(nullptr, w * 16, static_cast<UINT>(result.rgba.size() * sizeof(float)),
                           reinterpret_cast<BYTE *>(result.rgba.data())));
    // D2D color management consumes embedded profiles; ordinary unprofiled
    // images are interpreted as sRGB. HDR float pixels are already linear.
    UINT count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count)
    {
        ComPtr<IWICColorContext> context;
        hr(wic->CreateColorContext(&context));
        IWICColorContext *ptr = context.Get();
        if (SUCCEEDED(frame->GetColorContexts(1, &ptr, &count)))
        {
            UINT size = 0;
            if (SUCCEEDED(context->GetProfileBytes(0, nullptr, &size)) && size && size <= 1024 * 1024)
            {
                result.profile.resize(size);
                hr(context->GetProfileBytes(size, reinterpret_cast<BYTE *>(result.profile.data()), &size));
            }
        }
    }
    for (auto &v : result.rgba)
        if (!std::isfinite(v))
            v = 0;
    return result;
}
std::wstring codecStatus()
{
    std::wstring s = L"JPEG, PNG, GIF, BMP, TIFF";
    for (const auto f : {ImageFormat::webp, ImageFormat::heif})
    {
        ComPtr<IWICBitmapDecoder> d;
        const auto ok =
            SUCCEEDED(CoCreateInstance(decoderId(f), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)));
        s += f == ImageFormat::webp ? L"\nWebP: " : L"\nHEIC/HEIF: ";
        s += ok ? L"decoder installed" : L"Windows image extension required";
    }
    return s;
}
struct Renderer::Impl
{
    HWND hwnd;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> target, current, previous;
    ComPtr<ID2D1ColorContext> currentColor, previousColor;
    ComPtr<IDWriteFactory> textFactory;
    ComPtr<IDWriteTextLayout> captionLayout;
    std::wstring captionText, captionFont;
    float captionWidth = 0, captionSize = 0;
    bool captionBold = false, captionItalic = false;
    bool currentHdr = false, previousHdr = false;
    bool hdrDisplay = false;
    float displayMax = 80;
    explicit Impl(HWND h) : hwnd(h)
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL level{};
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                        D3D11_SDK_VERSION, &d3d, &level, &d3dContext);
        if (FAILED(result))
            hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 &d3d, &level, &d3dContext));
        hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIDevice> dxgi;
        hr(d3d.As(&dxgi));
        hr(factory->CreateDevice(dxgi.Get(), &device));
        hr(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        ComPtr<IDXGIAdapter> adapter;
        hr(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> dxgiFactory;
        hr(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr(dxgiFactory->CreateSwapChainForHwnd(d3d.Get(), h, &desc, nullptr, nullptr, &swap));
        dxgiFactory->MakeWindowAssociation(h, DXGI_MWA_NO_ALT_ENTER);
        resize();
    }
    void caption(std::wstring_view text, const MetadataOptions &options)
    {
        const auto size = context->GetSize();
        if (text.empty() || !current || size.width < 32 || size.height < 32)
            return;
        const float points = (std::min)(options.points * GetDpiForWindow(hwnd) / 72.f, size.height / 8);
        const float padding = (std::max)(4.f, points * .35f);
        const float width = size.width * .9f - 2 * padding;
        if (!textFactory)
            hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(textFactory.GetAddressOf())));
        if (!captionLayout || captionText != text || captionFont != options.font || captionWidth != width ||
            captionSize != points || captionBold != options.bold || captionItalic != options.italic)
        {
            captionLayout.Reset();
            ComPtr<IDWriteTextFormat> format;
            hr(textFactory->CreateTextFormat(
                options.font.c_str(), nullptr,
                options.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                options.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, points, L"", &format));
            hr(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
            hr(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            hr(textFactory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
            hr(format->SetTrimming(&trim, ellipsis.Get()));
            const float lines = text.find(L'\n') == text.npos ? 1.f : 2.f;
            hr(textFactory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
                                             width, points * 1.5f * lines, &captionLayout));
            captionText = text;
            captionFont = options.font;
            captionWidth = width;
            captionSize = points;
            captionBold = options.bold;
            captionItalic = options.italic;
        }
        DWRITE_TEXT_METRICS metrics{};
        hr(captionLayout->GetMetrics(&metrics));
        const float height = metrics.height + 2 * padding;
        const float margin = (std::max)(8.f, size.height * .03f);
        const float top = options.top ? margin : size.height - margin - height;
        const float panelWidth =
            (std::min)(size.width * .9f, metrics.widthIncludingTrailingWhitespace + 2 * padding);
        ComPtr<ID2D1SolidColorBrush> background, white;
        hr(context->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, .78f), &background));
        hr(context->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white));
        context->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF((size.width - panelWidth) / 2, top, (size.width + panelWidth) / 2, top + height),
                padding * .5f, padding * .5f),
            background.Get());
        context->DrawTextLayout(D2D1::Point2F((size.width - width) / 2, top + padding), captionLayout.Get(),
                                white.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void resize()
    {
        context->SetTarget(nullptr);
        target.Reset();
        RECT rect{};
        GetClientRect(hwnd, &rect);
        hr(swap->ResizeBuffers(0, static_cast<UINT>((std::max)(1L, rect.right)),
                               static_cast<UINT>((std::max)(1L, rect.bottom)), DXGI_FORMAT_UNKNOWN, 0));
        ComPtr<IDXGISurface> surface;
        hr(swap->GetBuffer(0, IID_PPV_ARGS(&surface)));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        hr(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target));
        context->SetTarget(target.Get());
        hdrDisplay = false;
        ComPtr<IDXGIOutput> output;
        if (SUCCEEDED(swap->GetContainingOutput(&output)))
        {
            ComPtr<IDXGIOutput6> out;
            if (SUCCEEDED(output.As(&out)))
            {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(out->GetDesc1(&d)))
                {
                    hdrDisplay = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    displayMax = hdrDisplay ? (std::max)(80.f, d.MaxLuminance) : 80.f;
                }
            }
        }
        ComPtr<IDXGISwapChain3> s3;
        if (SUCCEEDED(swap.As(&s3)))
        {
            UINT support = 0;
            if (SUCCEEDED(s3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
                hr(s3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
        }
    }
    void image(const Pixels &p)
    {
        previous = current;
        previousColor = currentColor;
        previousHdr = currentHdr;
        current.Reset();
        currentColor.Reset();
        currentHdr = p.hdr;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        hr(context->CreateBitmap(D2D1::SizeU(p.width, p.height), p.rgba.data(), p.width * 16, &props,
                                 &current));
        const auto space = p.hdr ? D2D1_COLOR_SPACE_SCRGB : D2D1_COLOR_SPACE_SRGB;
        if (p.profile.empty() ||
            FAILED(context->CreateColorContext(D2D1_COLOR_SPACE_CUSTOM,
                                               reinterpret_cast<const BYTE *>(p.profile.data()),
                                               static_cast<UINT>(p.profile.size()), &currentColor)))
            hr(context->CreateColorContext(space, nullptr, 0, &currentColor));
    }
    void paint(ID2D1Bitmap1 *bitmap, ID2D1ColorContext *sourceColor, bool isHdr, float opacity, float x,
               float scale, const Settings &settings, const D2D1_MATRIX_4X4_F *projection = nullptr,
               float zoom = 1, float y = 0)
    {
        if (!bitmap || opacity <= 0 || scale <= 0.001f)
            return;
        const auto size = context->GetSize(), src = bitmap->GetSize();
        const float ratio = settings.fill ? (std::max)(size.width / src.width, size.height / src.height)
                                          : (std::min)(size.width / src.width, size.height / src.height);
        const float w = src.width * ratio * scale * zoom, h = src.height * ratio * zoom;
        const auto rect = D2D1::RectF((size.width - w) / 2 + x, (size.height - h) / 2 + y,
                                      (size.width + w) / 2 + x, (size.height + h) / 2 + y);
        ComPtr<ID2D1Effect> color, tone, affine;
        hr(context->CreateEffect(CLSID_D2D1ColorManagement, &color));
        color->SetInput(0, bitmap);
        hr(color->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST));
        hr(color->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, sourceColor));
        ComPtr<ID2D1ColorContext> targetColor;
        hr(context->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &targetColor));
        hr(color->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, targetColor.Get()));
        ComPtr<ID2D1Image> image;
        color->GetOutput(&image);
        if (isHdr)
        {
            hr(context->CreateEffect(CLSID_D2D1HdrToneMap, &tone));
            tone->SetInput(0, image.Get());
            hr(tone->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE, 1000.f));
            hr(tone->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE,
                              hdrDisplay ? (std::min)(displayMax, static_cast<float>(settings.hdrNits))
                                         : 80.f));
            hr(tone->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE, hdrDisplay
                                                                     ? D2D1_HDRTONEMAP_DISPLAY_MODE_HDR
                                                                     : D2D1_HDRTONEMAP_DISPLAY_MODE_SDR));
            tone->GetOutput(&image);
        }
        hr(context->CreateEffect(CLSID_D2D12DAffineTransform, &affine));
        affine->SetInput(0, image.Get());
        hr(affine->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Scale(w / src.width, h / src.height) *
                                D2D1::Matrix3x2F::Translation(rect.left, rect.top)));
        context->PushAxisAlignedClip(D2D1::RectF(0, 0, size.width, size.height), D2D1_ANTIALIAS_MODE_ALIASED);
        if (opacity < 1)
        {
            D2D1_LAYER_PARAMETERS1 parameters = D2D1::LayerParameters1();
            parameters.opacity = opacity;
            context->PushLayer(parameters, nullptr);
        }
        if (projection)
        {
            ComPtr<ID2D1Effect> perspective;
            hr(context->CreateEffect(CLSID_D2D13DTransform, &perspective));
            perspective->SetInputEffect(0, affine.Get());
            hr(perspective->SetValue(D2D1_3DTRANSFORM_PROP_TRANSFORM_MATRIX, *projection));
            hr(perspective->SetValue(D2D1_3DTRANSFORM_PROP_INTERPOLATION_MODE,
                                     D2D1_3DTRANSFORM_INTERPOLATION_MODE_LINEAR));
            context->DrawImage(perspective.Get());
        }
        else
            context->DrawImage(affine.Get());
        if (opacity < 1)
            context->PopLayer();
        context->PopAxisAlignedClip();
    }
};
Renderer::Renderer(HWND w) : p_(std::make_unique<Impl>(w)) {}
Renderer::~Renderer() = default;
void Renderer::resize()
{
    p_->resize();
}
void Renderer::setImage(const Pixels &image)
{
    p_->image(image);
}
void Renderer::clear()
{
    p_->current.Reset();
    p_->previous.Reset();
    p_->currentColor.Reset();
    p_->previousColor.Reset();
}
void Renderer::draw(float p, Transition transition, const Settings &s, float motionProgress,
                    std::wstring_view caption, const MetadataOptions *metadata)
{
    auto &r = *p_;
    r.context->BeginDraw();
    r.context->SetTransform(D2D1::Matrix3x2F::Identity());
    r.context->Clear(D2D1::ColorF((s.background >> 16 & 255) / 255.f, (s.background >> 8 & 255) / 255.f,
                                  (s.background & 255) / 255.f));
    const auto t = (std::clamp)(p, 0.f, 1.f), width = r.context->GetSize().width;
    auto old = [&](float opacity = 1, float x = 0, float scale = 1) {
        r.paint(r.previous.Get(), r.previousColor.Get(), r.previousHdr, opacity, x, scale, s);
    };
    auto next = [&](float opacity = 1, float x = 0, float scale = 1) {
        r.paint(r.current.Get(), r.currentColor.Get(), r.currentHdr, opacity, x, scale, s);
    };
    switch (transition)
    {
    case Transition::fade:
        if (t < .5f)
            old(1 - 2 * t);
        else
            next(2 * t - 1);
        break;
    case Transition::slide:
        old(1, -width * t);
        next(1, width * (1 - t));
        break;
    case Transition::crossFade:
        old();
        next(t);
        break;
    case Transition::flip:
        if (t < .5f)
            old(1, 0, 1 - 2 * t);
        else
            next(1, 0, 2 * t - 1);
        break;
    case Transition::cube: {
        if (t <= 0 || t >= 1)
        {
            if (t <= 0 && r.previous)
                old();
            else
                next();
            break;
        }
        const auto size = r.context->GetSize();
        const auto center = D2D1::Matrix4x4F::Translation(-size.width / 2, -size.height / 2, 0);
        const auto rotation = D2D1::Matrix4x4F::RotationY(90 * t);
        // Positive depth recedes from the viewer. Adjacent faces share an edge.
        const D2D1::Matrix4x4F perspective(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1 / (2.5f * size.width), 0, 0, 0,
                                           1);
        const auto camera = rotation * D2D1::Matrix4x4F::Translation(0, 0, size.width / 2) * perspective *
                            D2D1::Matrix4x4F::Translation(size.width / 2, size.height / 2, 0);
        const auto outgoing = center * D2D1::Matrix4x4F::Translation(0, 0, -size.width / 2) * camera;
        const auto incoming = center * D2D1::Matrix4x4F::RotationY(-90) *
                              D2D1::Matrix4x4F::Translation(size.width / 2, 0, 0) * camera;
        r.paint(r.previous.Get(), r.previousColor.Get(), r.previousHdr, 1, 0, 1, s, &outgoing);
        r.paint(r.current.Get(), r.currentColor.Get(), r.currentHdr, 1, 0, 1, s, &incoming);
        break;
    }
    case Transition::panZoom: {
        const auto motion = (std::clamp)(motionProgress, 0.f, 1.f);
        const auto height = r.context->GetSize().height;
        if (t < 1)
            r.paint(r.previous.Get(), r.previousColor.Get(), r.previousHdr, 1, width * .02f, 1, s, nullptr,
                    1.14f, -height * .01f);
        r.paint(r.current.Get(), r.currentColor.Get(), r.currentHdr, t, width * (-.02f + .04f * motion), 1, s,
                nullptr, 1.04f + .1f * motion, height * (.01f - .02f * motion));
        break;
    }
    case Transition::collage: {
        if (t >= 1)
        {
            next();
            break;
        }
        old();
        const auto size = r.context->GetSize();
        // Six clipped pieces use the same decoded bitmap: no extra downloads.
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
            {
                const float delay = static_cast<float>((row * 3 + col) * 5 % 6) * .05f;
                const float phase = (std::clamp)((t - delay) / .75f, 0.f, 1.f);
                const float ease = phase * phase * (3 - 2 * phase);
                const float x = (col - 1) * size.width * .2f * (1 - ease);
                const float y = (row == 0 ? -1.f : 1.f) * size.height * .2f * (1 - ease);
                const auto tile =
                    D2D1::RectF(col * size.width / 3 + x, row * size.height / 2 + y,
                                (col + 1) * size.width / 3 + x, (row + 1) * size.height / 2 + y);
                r.context->PushAxisAlignedClip(tile, D2D1_ANTIALIAS_MODE_ALIASED);
                r.paint(r.current.Get(), r.currentColor.Get(), r.currentHdr, ease, x, 1, s, nullptr, 1, y);
                r.context->PopAxisAlignedClip();
            }
        break;
    }
    default:
        next();
        break;
    }
    if (metadata)
        r.caption(caption, *metadata);
    hr(r.context->EndDraw());
    const auto status = r.swap->Present(1, 0);
    if (status != DXGI_STATUS_OCCLUDED)
        hr(status);
}
} // namespace mrss
