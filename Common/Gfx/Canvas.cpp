/* Copyright (C) 2013 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include "StdAfx.h"
#include "Canvas.h"
#include "TextFormatD2D.h"
#include "D2DBitmap.h"
#include "RenderTexture.h"
#include "Util/D2DUtil.h"
#include "Util/DWriteFontCollectionLoader.h"

#include <dxgidebug.h>

namespace Gfx {

UINT Canvas::c_Instances = 0;
D3D_FEATURE_LEVEL Canvas::c_FeatureLevel;
Microsoft::WRL::ComPtr<ID3D11Device> Canvas::c_D3DDevice;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> Canvas::c_D3DContext;
Microsoft::WRL::ComPtr<ID2D1Device> Canvas::c_D2DDevice;
Microsoft::WRL::ComPtr<IDXGIDevice1> Canvas::c_DxgiDevice;
Microsoft::WRL::ComPtr<ID2D1Factory1> Canvas::c_D2DFactory;
Microsoft::WRL::ComPtr<IDWriteFactory1> Canvas::c_DWFactory;
Microsoft::WRL::ComPtr<IWICImagingFactory> Canvas::c_WICFactory;

Canvas::Canvas() :
	m_W(0),
	m_H(0),
	m_MaxBitmapSize(0U),
	m_IsDrawing(false),
	m_EnableDrawAfterGdi(false),
	m_TextAntiAliasing(false),
	m_CanUseAxisAlignClip(true)
{
	Initialize(true);
}

Canvas::~Canvas()
{
	Finalize();
}

bool Canvas::Initialize(bool hardwareAccelerated)
{
	++c_Instances;
	if (c_Instances == 1U)
	{
		// Required for Direct2D interopability.
		UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		auto tryCreateContext = [&](D3D_DRIVER_TYPE driverType,
			const D3D_FEATURE_LEVEL* levels, UINT numLevels)
		{
			return D3D11CreateDevice(
				nullptr,
				driverType,
				nullptr,
				creationFlags,
				levels,
				numLevels,
				D3D11_SDK_VERSION,
				c_D3DDevice.GetAddressOf(),
				&c_FeatureLevel,
				c_D3DContext.GetAddressOf());
		};

		// D3D selects the best feature level automatically and sets it
		// to |c_FeatureLevel|. First, we try to use the hardware driver
		// and if that fails, we try the WARP rasterizer for cases
		// where there is no graphics card or other failures.
		const D3D_FEATURE_LEVEL levels[] = 
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1
		};

		HRESULT hr = E_FAIL;
		if (hardwareAccelerated)
		{
			hr = tryCreateContext(D3D_DRIVER_TYPE_HARDWARE, levels, _countof(levels));
			if (hr == E_INVALIDARG)
			{
				hr = tryCreateContext(D3D_DRIVER_TYPE_HARDWARE, &levels[1], _countof(levels) - 1);
			}
		}

		if (FAILED(hr))
		{
			hr = tryCreateContext(D3D_DRIVER_TYPE_WARP, nullptr, 0U);
			if (FAILED(hr)) return false;
		}

		hr = c_D3DDevice.As(&c_DxgiDevice);
		if (FAILED(hr)) return false;

		D2D1_FACTORY_OPTIONS fo = {};
#ifdef _DEBUG
		fo.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

		hr = D2D1CreateFactory(
			D2D1_FACTORY_TYPE_SINGLE_THREADED,
			fo,
			c_D2DFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = c_D2DFactory->CreateDevice(
			c_DxgiDevice.Get(),
			c_D2DDevice.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = CoCreateInstance(
			CLSID_WICImagingFactory,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_IWICImagingFactory,
			(LPVOID*)c_WICFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = DWriteCreateFactory(
			DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(c_DWFactory),
			(IUnknown**)c_DWFactory.GetAddressOf());
		if (FAILED(hr)) return false;

		hr = c_DWFactory->RegisterFontCollectionLoader(Util::DWriteFontCollectionLoader::GetInstance());
		if (FAILED(hr)) return false;
	}

	return true;
}

bool Canvas::EnumerateInstalledFontFamilies(UINT32 & familyCount, std::wstring & families)
{
	bool success = false;
	FontCollectionD2D * collection = new FontCollectionD2D();
	collection->InitializeCollection();

	success = collection->GetSystemFontFamilies(familyCount, families);

	if (collection)
	{
		delete collection;
		collection = nullptr;
	}

	return success;
}

void Canvas::Finalize()
{
	--c_Instances;
	if (c_Instances == 0U)
	{

// Dump extra dxgi debugging information (if needed)
// On the following line, change |FALSE| to |TRUE|
#if defined(_DEBUG) && FALSE
		// More info: https://docs.microsoft.com/en-us/windows/win32/api/dxgidebug/nf-dxgidebug-dxgigetdebuginterface
		typedef HRESULT(__stdcall* fDebugInterface)(const IID&, void**);
		HMODULE hDll = GetModuleHandle(L"Dxgidebug.dll");
		if (hDll)
		{
			fDebugInterface DXGIGetDebugInterface = (fDebugInterface)GetProcAddress(hDll, "DXGIGetDebugInterface");
			IDXGIDebug* pDxgiDebug = nullptr;
			HRESULT hr = DXGIGetDebugInterface(__uuidof(IDXGIDebug), (void**)&pDxgiDebug);
			if (SUCCEEDED(hr))
			{
				pDxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);  // Use |DXGI_DEBUG_RLO_SUMMARY| if needed
				pDxgiDebug->Release();
			}
		}
#endif

		c_D3DDevice.Reset();
		c_D3DContext.Reset();
		c_D2DDevice.Reset();
		c_DxgiDevice.Reset();
		c_D2DFactory.Reset();
		c_WICFactory.Reset();

		if (c_DWFactory)
		{
			c_DWFactory->UnregisterFontCollectionLoader(Util::DWriteFontCollectionLoader::GetInstance());
			c_DWFactory.Reset();
		}
	}
}

bool Canvas::InitializeRenderTarget(HWND hwnd, LONG* errCode)
{
	HRESULT hr = E_FAIL;

	auto cleanUp = [&]() -> bool
	{
		*errCode = hr;
		return false;
	};

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = { 0 };
	swapChainDesc.Width = 1U;
	swapChainDesc.Height = 1U;
	swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.Stereo = false;
	swapChainDesc.SampleDesc.Count = 1U;
	swapChainDesc.SampleDesc.Quality = 0U;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2U;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
	hr = c_DxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
	if (FAILED(hr)) return cleanUp();

	// Ensure that DXGI does not queue more than one frame at a time.
	hr = c_DxgiDevice->SetMaximumFrameLatency(1U);
	if (FAILED(hr)) return cleanUp();

	Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
	hr = dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
	if (FAILED(hr)) return cleanUp();

	hr = dxgiFactory->CreateSwapChainForHwnd(
		c_DxgiDevice.Get(),
		hwnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		m_SwapChain.ReleaseAndGetAddressOf());
	if (FAILED(hr)) return cleanUp();

	// Prevent DXGI from monitoring window changes through "alt + enter" (full screen mode)
	hr = dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr))
	{
		*errCode = hr;  // Non-fatal error
	}

	hr = CreateRenderTarget();
	if (FAILED(hr)) return cleanUp();

	return CreateTargetBitmap(0U, 0U, errCode);
}

void Canvas::Resize(int w, int h)
{
	// Truncate the size of the skin if it's too big.
	if (w > (int)m_MaxBitmapSize) w = (int)m_MaxBitmapSize;
	if (h > (int)m_MaxBitmapSize) h = (int)m_MaxBitmapSize;

	m_W = w;
	m_H = h;

	// Check if target, targetbitmap, backbuffer, swap chain are valid?

	// Unmap all resources tied to the swap chain.
	m_Target->SetTarget(nullptr);
	m_TargetBitmap.Reset();
	m_BackBuffer.Reset();

	// Resize swap chain.
	HRESULT hr = m_SwapChain->ResizeBuffers(
		0U,
		(UINT)w,
		(UINT)h,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
	if (FAILED(hr)) return;

	CreateTargetBitmap((UINT32)w, (UINT32)h);
}

bool Canvas::BeginDraw()
{
	if (!m_Target)
	{
		HRESULT hr = CreateRenderTarget();
		if (FAILED(hr))
		{
			m_IsDrawing = false;
			return false;
		}

		// Recreate target bitmap
		Resize(m_W, m_H);
	}

	m_Target->BeginDraw();
	m_IsDrawing = true;
	return true;
}

void Canvas::EndDraw()
{
	HRESULT hr = m_Target->EndDraw();
	if (FAILED(hr))
	{
		m_Target.Reset();
	}

	m_IsDrawing = false;
}

HDC Canvas::GetDC()
{
	if (m_IsDrawing)
	{
		m_EnableDrawAfterGdi = true;
		m_IsDrawing = false;
		EndDraw();
	}

	HDC hdc;
	HRESULT hr = m_BackBuffer->GetDC(FALSE, &hdc);
	if (FAILED(hr)) return nullptr;

	return hdc;
}

void Canvas::ReleaseDC()
{
	m_BackBuffer->ReleaseDC(nullptr);

	if (m_EnableDrawAfterGdi)
	{
		m_EnableDrawAfterGdi = false;
		m_IsDrawing = true;
		BeginDraw();
	}
}

bool Canvas::IsTransparentPixel(int x, int y)
{
	if (!(x >= 0 && y >= 0 && x < m_W && y < m_H)) return false;

	auto pixel = GetPixel(GetDC(), x, y);
	ReleaseDC();

	return (pixel & 0xFF000000) == 0;
}

void Canvas::GetTransform(D2D1_MATRIX_3X2_F* matrix)
{
	if (m_Target)
	{
		m_Target->GetTransform(matrix);
	}
}

void Canvas::SetTransform(const D2D1_MATRIX_3X2_F& matrix)
{
	m_Target->SetTransform(matrix);

	m_CanUseAxisAlignClip =
		(matrix.m11 ==  1.0f && matrix.m12 ==  0.0f && matrix.m21 ==  0.0f && matrix.m22 ==  1.0f) ||	// Angle: 0
		(matrix.m11 ==  0.0f && matrix.m12 ==  1.0f && matrix.m21 == -1.0f && matrix.m22 ==  0.0f) ||	// Angle: 90
		(matrix.m11 == -1.0f && matrix.m12 ==  0.0f && matrix.m21 ==  0.0f && matrix.m22 == -1.0f) ||	// Angle: 180
		(matrix.m11 ==  0.0f && matrix.m12 == -1.0f && matrix.m21 ==  1.0f && matrix.m22 ==  0.0f);		// Angle: 270
}

void Canvas::ResetTransform()
{
	m_Target->SetTransform(D2D1::Matrix3x2F::Identity());
}

bool Canvas::SetTarget(RenderTexture* texture)
{
	auto bitmap = texture->GetBitmap();
	if (bitmap->m_Segments.size() == 0) return false;

	auto image = bitmap->m_Segments[0].GetBitmap();
	m_Target->SetTarget(image);
	return true;
}

void Canvas::ResetTarget()
{
	m_Target->SetTarget(m_TargetBitmap.Get());
}

void Canvas::SetAntiAliasing(bool enable)
{
	m_Target->SetAntialiasMode(enable ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::SetTextAntiAliasing(bool enable)
{
	// TODO: Add support for D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE?
	m_TextAntiAliasing = enable;
	m_Target->SetTextAntialiasMode(enable ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
}

void Canvas::Clear(const D2D1_COLOR_F& color)
{
	if (!m_Target) return;

	m_Target->Clear(color);
}

void Canvas::DrawTextW(const std::wstring& srcStr, const TextFormat& format, const D2D1_RECT_F& rect,
	const D2D1_COLOR_F& color, bool applyInlineFormatting)
{
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(color, solidBrush.GetAddressOf());
	if (FAILED(hr)) return;

	TextFormatD2D& formatD2D = (TextFormatD2D&)format;

	static std::wstring str;
	str = srcStr;
	formatD2D.ApplyInlineCase(str);

	if (!formatD2D.CreateLayout(
		m_Target.Get(),
		str,
		rect.right - rect.left,
		rect.bottom - rect.top,
		!m_AccurateText && m_TextAntiAliasing)) return;

	D2D1_POINT_2F drawPosition = D2D1::Point2F();
	drawPosition.x = [&]()
	{
		if (!m_AccurateText)
		{
			const float xOffset = formatD2D.m_TextFormat->GetFontSize() / 6.0f;
			switch (formatD2D.GetHorizontalAlignment())
			{
			case HorizontalAlignment::Left: return rect.left + xOffset;
			case HorizontalAlignment::Right: return rect.left - xOffset;
			}
		}

		return rect.left;
	} ();

	drawPosition.y = [&]()
	{
		// GDI+ compatibility.
		float yPos = rect.top - formatD2D.m_LineGap;
		switch (formatD2D.GetVerticalAlignment())
		{
		case VerticalAlignment::Bottom: yPos -= formatD2D.m_ExtraHeight; break;
		case VerticalAlignment::Center: yPos -= formatD2D.m_ExtraHeight / 2.0f; break;
		}

		return yPos;
	} ();

	// When different "effects" are used with inline coloring options, we need to
	// remove the previous inline coloring, then reapply them (if needed) - instead
	// of destroying/recreating the text layout.
	UINT32 strLen = (UINT32)str.length();
	formatD2D.ResetInlineColoring(solidBrush.Get(), strLen);
	if (applyInlineFormatting)
	{
		formatD2D.ApplyInlineColoring(m_Target.Get(), &drawPosition);

		// Draw any 'shadow' effects
		const D2D1_RECT_F drawRect = D2D1::RectF(
			drawPosition.x, drawPosition.y, rect.right - rect.left, rect.bottom - rect.top);
		formatD2D.ApplyInlineShadow(m_Target.Get(), solidBrush.Get(), strLen, drawRect);
	}

	if (formatD2D.m_Trimming)
	{
		D2D1_RECT_F clipRect = rect;

		if (m_CanUseAxisAlignClip)
		{
			m_Target->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_ALIASED);
		}
		else
		{
			const D2D1_LAYER_PARAMETERS1 layerParams =
				D2D1::LayerParameters1(clipRect, nullptr, D2D1_ANTIALIAS_MODE_ALIASED);
			m_Target->PushLayer(layerParams, nullptr);
		}
	}

	m_Target->DrawTextLayout(drawPosition, formatD2D.m_TextLayout.Get(), solidBrush.Get());

	if (formatD2D.m_Trimming)
	{
		if (m_CanUseAxisAlignClip)
		{
			m_Target->PopAxisAlignedClip();
		}
		else
		{
			m_Target->PopLayer();
		}
	}

	if (applyInlineFormatting)
	{
		// Inline gradients require the drawing position, so in case that position
		// changes, we need a way to reset it after drawing time so on the next
		// iteration it will know the correct position.
		formatD2D.ResetGradientPosition(&drawPosition);
	}
}

bool Canvas::MeasureTextW(const std::wstring& str, const TextFormat& format, D2D1_SIZE_F& size)
{
	TextFormatD2D& formatD2D = (TextFormatD2D&)format;

	static std::wstring formatStr;
	formatStr = str;
	formatD2D.ApplyInlineCase(formatStr);

	const DWRITE_TEXT_METRICS metrics = formatD2D.GetMetrics(formatStr, !m_AccurateText);
	size.width = metrics.width;
	size.height = metrics.height;
	return true;
}

bool Canvas::MeasureTextLinesW(const std::wstring& str, const TextFormat& format, D2D1_SIZE_F& size, UINT32& lines)
{
	TextFormatD2D& formatD2D = (TextFormatD2D&)format;
	formatD2D.m_TextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

	static std::wstring formatStr;
	formatStr = str;
	formatD2D.ApplyInlineCase(formatStr);

	const DWRITE_TEXT_METRICS metrics = formatD2D.GetMetrics(formatStr, !m_AccurateText, size.width);
	size.width = metrics.width;
	size.height = metrics.height;
	lines = metrics.lineCount;

	if (size.height > 0.0f)
	{
		// GDI+ draws multi-line text even though the last line may be clipped slightly at the
		// bottom. This is a workaround to emulate that behaviour.
		size.height += 1.0f;
	}
	else
	{
		// GDI+ compatibility: Zero height text has no visible lines.
		lines = 0U;
	}
	return true;
}

void Canvas::DrawBitmap(D2DBitmap* bitmap, const D2D1_RECT_F& dstRect, const D2D1_RECT_F& srcRect)
{
	for (auto& segment : bitmap->m_Segments)
	{
		const auto& rSeg = segment.GetRect();
		D2D1_RECT_F rSrc = (rSeg.left < rSeg.right && rSeg.top < rSeg.bottom) ?
			D2D1::RectF(
				max(rSeg.left, srcRect.left),
				max(rSeg.top, srcRect.top),
				min(rSeg.right + rSeg.left, srcRect.right),
				min(rSeg.bottom + rSeg.top, srcRect.bottom)) :
			D2D1::RectF();
		if (rSrc.left == rSrc.right || rSrc.top == rSrc.bottom) continue;

		const D2D1_RECT_F rDst = D2D1::RectF(
			(rSrc.left   - srcRect.left) / (srcRect.right  - srcRect.left) * (dstRect.right  - dstRect.left) + dstRect.left,
			(rSrc.top    - srcRect.top)  / (srcRect.bottom - srcRect.top)  * (dstRect.bottom - dstRect.top)  + dstRect.top,
			(rSrc.right  - srcRect.left) / (srcRect.right  - srcRect.left) * (dstRect.right  - dstRect.left) + dstRect.left,
			(rSrc.bottom - srcRect.top)  / (srcRect.bottom - srcRect.top)  * (dstRect.bottom - dstRect.top)  + dstRect.top);

		while (rSrc.top >= m_MaxBitmapSize)
		{
			rSrc.bottom -= m_MaxBitmapSize;
			rSrc.top -= m_MaxBitmapSize;
		}

		while (rSrc.left >= m_MaxBitmapSize)
		{
			rSrc.right -= m_MaxBitmapSize;
			rSrc.left -= m_MaxBitmapSize;
		}

		m_Target->DrawBitmap(segment.GetBitmap(), rDst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &rSrc);
	}
}

void Canvas::DrawTiledBitmap(D2DBitmap* bitmap, const D2D1_RECT_F& dstRect, const D2D1_RECT_F& srcRect)
{
	const FLOAT width = (FLOAT)bitmap->m_Width;
	const FLOAT height = (FLOAT)bitmap->m_Height;

	FLOAT x = dstRect.left;
	FLOAT y = dstRect.top;

	while (y < dstRect.bottom)
	{
		const FLOAT w = (dstRect.right - x) > width ? width : (dstRect.right - x);
		const FLOAT h = (dstRect.bottom - y) > height ? height : (dstRect.bottom - y);

		const auto dst = D2D1::RectF(x, y, x + w, y + h);
		const auto src = D2D1::RectF(0.0f, 0.0f, w, h);
		DrawBitmap(bitmap, dst, src);

		x += width;
		if (x >= dstRect.right && y < dstRect.bottom)
		{
			x = dstRect.left;
			y += height;
		}
	}
}

void Canvas::DrawMaskedBitmap(D2DBitmap* bitmap, D2DBitmap* maskBitmap, const D2D1_RECT_F& dstRect,
	const D2D1_RECT_F& srcRect, const D2D1_RECT_F& srcRect2)
{
	if (!bitmap || !maskBitmap) return;

	// Create bitmap brush from original |bitmap|.
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> brush;
	D2D1_BITMAP_BRUSH_PROPERTIES1 propertiesXClampYClamp = D2D1::BitmapBrushProperties1(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);

	const FLOAT width = (FLOAT)bitmap->m_Width;
	const FLOAT height = (FLOAT)bitmap->m_Height;

	auto getRectSubRegion = [&width, &height](const D2D1_RECT_F& r1, const D2D1_RECT_F& r2) -> D2D1_RECT_F
	{
		return D2D1::RectF(
			r1.left / width * r2.right + r2.left,
			r1.top / height * r2.bottom + r2.top,
			(r1.right - r1.left) / width * r2.right,
			(r1.bottom - r1.top) / height * r2.bottom);
	};

	for (auto& bseg : bitmap->m_Segments)
	{
		const auto rSeg = bseg.GetRect();
		const auto rDst = getRectSubRegion(rSeg, dstRect);
		const auto rSrc = getRectSubRegion(rSeg, srcRect);

		FLOAT s2Width = srcRect2.right - srcRect2.left;
		FLOAT s2Height = srcRect2.bottom - srcRect2.top;

		// "Move" and "scale" the |bitmap| to match the destination.
		D2D1_MATRIX_3X2_F translateMask = D2D1::Matrix3x2F::Translation(-srcRect2.left, -srcRect2.top);
		D2D1_MATRIX_3X2_F translate = D2D1::Matrix3x2F::Translation(rDst.left, rDst.top);
		D2D1_MATRIX_3X2_F scale = D2D1::Matrix3x2F::Scale(
			D2D1::SizeF((rDst.right - rDst.left) / s2Width, (rDst.bottom - rDst.top) / s2Height));
		D2D1_BRUSH_PROPERTIES brushProps = D2D1::BrushProperties(1.0f, translateMask * scale * translate);

		HRESULT hr = m_Target->CreateBitmapBrush(
			bseg.GetBitmap(),
			propertiesXClampYClamp,
			brushProps,
			brush.ReleaseAndGetAddressOf());
		if (FAILED(hr)) return;

		const auto aaMode = m_Target->GetAntialiasMode();
		m_Target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // required

		for (auto& mseg : maskBitmap->m_Segments)
		{
			const auto rmSeg = mseg.GetRect();
			const auto rmDst = getRectSubRegion(rmSeg, dstRect);
			const auto rmSrc = getRectSubRegion(rmSeg, srcRect);

			// If no overlap, don't draw
			if ((rmDst.left < (rDst.left + rDst.right) &&
				(rmDst.right + rmDst.left) > rDst.left &&
				rmDst.top > (rmDst.top + rmDst.bottom) &&
				(rmDst.top + rmDst.bottom) < rmDst.top)) continue;

			m_Target->FillOpacityMask(
				mseg.GetBitmap(),
				brush.Get(),
				D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
				&rDst,
				&rSrc);
		}

		m_Target->SetAntialiasMode(aaMode);
	}
}

void Canvas::FillRectangle(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
{
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(color, solidBrush.GetAddressOf());
	if (SUCCEEDED(hr))
	{
		m_Target->FillRectangle(rect, solidBrush.Get());
	}
}

void Canvas::FillGradientRectangle(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color1, const D2D1_COLOR_F& color2, const FLOAT& angle)
{
	// D2D requires 2 points to draw the gradient along where GDI+ just requires a rectangle. To
	// mimic GDI+ for SolidColor2, we have to find and swap the starting and ending points of where
	// the gradient touches edge of the bounding rectangle. Normally we would offset the ending
	// point by 180, but we do this on starting point for SolidColor2. This differs from the other
	// D2D gradient options below:
	//  Gfx::TextInlineFormat_GradientColor::BuildGradientBrushes
	//  Gfx::Shape::CreateLinearGradient
	D2D1_POINT_2F start = Util::FindEdgePoint(angle + 180.0f, rect.left, rect.top, rect.right, rect.bottom);
	D2D1_POINT_2F end = Util::FindEdgePoint(angle, rect.left, rect.top, rect.right, rect.bottom);

	Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> pGradientStops;

	D2D1_GRADIENT_STOP gradientStops[2] = { 0 };
	gradientStops[0].color = color1;
	gradientStops[0].position = 0.0f;
	gradientStops[1].color = color2;
	gradientStops[1].position = 1.0f;

	HRESULT hr = m_Target->CreateGradientStopCollection(
		gradientStops,
		2U,
		D2D1_GAMMA_2_2,
		D2D1_EXTEND_MODE_CLAMP,
		pGradientStops.GetAddressOf());
	if (FAILED(hr)) return;

	Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
	hr = m_Target->CreateLinearGradientBrush(
		D2D1::LinearGradientBrushProperties(start, end),
		pGradientStops.Get(),
		brush.GetAddressOf());
	if (FAILED(hr)) return;

	m_Target->FillRectangle(rect, brush.Get());
}

void Canvas::DrawLine(const D2D1_COLOR_F& color, FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2, FLOAT strokeWidth)
{
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
	HRESULT hr = m_Target->CreateSolidColorBrush(color, solidBrush.GetAddressOf());
	if (FAILED(hr)) return;

	m_Target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), solidBrush.Get(), strokeWidth);
}

void Canvas::DrawGeometry(Shape& shape, int xPos, int yPos)
{
	D2D1_MATRIX_3X2_F worldTransform;
	m_Target->GetTransform(&worldTransform);
	m_Target->SetTransform(
		shape.GetShapeMatrix() *
		D2D1::Matrix3x2F::Translation((FLOAT)xPos, (FLOAT)yPos) *
		worldTransform);

	auto fill = shape.GetFillBrush(m_Target.Get());
	if (fill)
	{
		m_Target->FillGeometry(shape.m_Shape.Get(), fill.Get());
	}

	auto stroke = shape.GetStrokeFillBrush(m_Target.Get());
	if (stroke)
	{
		m_Target->DrawGeometry(
			shape.m_Shape.Get(),
			stroke.Get(),
			shape.m_StrokeWidth,
			shape.m_StrokeStyle.Get());
	}

	m_Target->SetTransform(worldTransform);
}

HRESULT Canvas::CreateRenderTarget()
{
	HRESULT hr = E_FAIL;
	if (c_D2DDevice)
	{
		c_D2DDevice->ClearResources();

		hr = c_D2DDevice->CreateDeviceContext(
			D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,
			m_Target.ReleaseAndGetAddressOf());
		if (FAILED(hr))
		{
			hr = c_D2DDevice->CreateDeviceContext(
				D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
				m_Target.ReleaseAndGetAddressOf());
		}
	}

	// Hardware accelerated targets have a hard limit to the size of bitmaps they can support.
	// The size will depend on the D3D feature level of the driver used. The WARP software
	// renderer has a limit of 16MP (16*1024*1024 = 16777216).

	// https://docs.microsoft.com/en-us/windows/desktop/direct3d11/overviews-direct3d-11-devices-downlevel-intro#overview-for-each-feature-level
	// Max Texture Dimension
	// D3D_FEATURE_LEVEL_11_1 = 16348
	// D3D_FEATURE_LEVEL_11_0 = 16348
	// D3D_FEATURE_LEVEL_10_1 = 8192
	// D3D_FEATURE_LEVEL_10_0 = 8192
	// D3D_FEATURE_LEVEL_9_3  = 4096
	// D3D_FEATURE_LEVEL_9_2  = 2048
	// D3D_FEATURE_LEVEL_9_1  = 2048

	if (SUCCEEDED(hr))
	{
		m_MaxBitmapSize = m_Target->GetMaximumBitmapSize();
	}

	return hr;
}

bool Canvas::CreateTargetBitmap(UINT32 width, UINT32 height, LONG* errCode)
{
	HRESULT hr = m_SwapChain->GetBuffer(0U, IID_PPV_ARGS(m_BackBuffer.GetAddressOf()));
	if (FAILED(hr))
	{
		if (errCode) *errCode = hr;
		return false;
	}

	D2D1_BITMAP_PROPERTIES1 bProps = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

	hr = m_Target->CreateBitmapFromDxgiSurface(
		m_BackBuffer.Get(),
		&bProps,
		m_TargetBitmap.GetAddressOf());
	if (FAILED(hr))
	{
		if (errCode) *errCode = hr;
		return false;
	}

	m_Target->SetTarget(m_TargetBitmap.Get());
	return true;
}

}  // namespace Gfx
