#include "stdafx.h"
#include "WeaselDetailPanel.h"
#include <ShellScalingAPI.h>
#include <cmath>

#pragma comment(lib, "Shcore.lib")

using namespace weasel;

WeaselDetailPanel::WeaselDetailPanel(weasel::UI& ui)
    : m_ui(ui),
      m_style(ui.style()),
      m_last_candidate_index(-1),
      m_last_dpi(96),
      m_dpiScaleLayout(1.0f),
      m_dpiScaleFontPoint(1.0f),
      m_current_content_width(0),
      m_current_content_height(0) {
  m_current_pos = {0, 0, 0, 0};
}

WeaselDetailPanel::~WeaselDetailPanel() {
  Destroy();
}

LRESULT WeaselDetailPanel::OnCreate(UINT uMsg,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL& bHandled) {
  return TRUE;
}

LRESULT WeaselDetailPanel::OnDestroy(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  m_pRenderTarget.Reset();
  m_pBrush.Reset();
  return 0;
}

LRESULT WeaselDetailPanel::OnMouseActivate(UINT uMsg,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           BOOL& bHandled) {
  return MA_NOACTIVATE;
}

void WeaselDetailPanel::Show() {
  if (IsWindow() && !IsWindowVisible()) {
    ShowWindow(SW_SHOWNA);
  }
}

void WeaselDetailPanel::Hide() {
  if (IsWindow() && IsWindowVisible()) {
    ShowWindow(SW_HIDE);
  }
  m_last_detail_text.clear();
  m_last_candidate_index = -1;
}

void WeaselDetailPanel::Destroy() {
  if (IsWindow()) {
    DestroyWindow();
  }
}

void WeaselDetailPanel::_UpdateDpi(const CRect& rcCandidate) {
  HMONITOR hMonitor = MonitorFromRect(&rcCandidate, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  if (hMonitor) {
    GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  }
  m_last_dpi = dpiX;
  m_dpiScaleFontPoint = (float)dpiX / 72.0f;
  m_dpiScaleLayout = (float)dpiX / 96.0f;
}

void WeaselDetailPanel::Update(const std::wstring& detail_text,
                               int candidate_index,
                               const CRect& rcCandidate) {
  if (!m_style.detail_enabled || detail_text.empty()) {
    Hide();
    return;
  }

  _UpdateDpi(rcCandidate);

  // If text, candidate, dpi, and candidate position are all identical, no-op
  if (m_last_detail_text == detail_text &&
      m_last_candidate_index == candidate_index && m_last_dpi == m_last_dpi &&
      m_last_candidate_rect == rcCandidate && IsWindowVisible()) {
    return;
  }

  // If text, candidate, and dpi are identical, just reposition
  if (m_last_detail_text == detail_text &&
      m_last_candidate_index == candidate_index && m_last_dpi == m_last_dpi &&
      IsWindowVisible()) {
    Reposition(rcCandidate);
    return;
  }

  auto pdwr = m_ui.pdwr();
  if (!pdwr || !pdwr->pDWFactory) {
    return;
  }

  // Font setup
  const std::wstring font_face = m_style.comment_font_face.empty()
                                     ? m_style.font_face
                                     : m_style.comment_font_face;
  int font_point = m_style.comment_font_point > 0
                       ? m_style.comment_font_point
                       : (m_style.font_point > 0 ? m_style.font_point : 12);

  int configured_width =
      DPI_SCALE(m_style.detail_width > 0 ? m_style.detail_width : 320);
  int padding_x =
      DPI_SCALE(m_style.detail_padding_x > 0 ? m_style.detail_padding_x : 12);
  int padding_y =
      DPI_SCALE(m_style.detail_padding_y > 0 ? m_style.detail_padding_y : 10);

  float max_layout_width = (float)max(20, configured_width - 2 * padding_x);

  ComPtr<IDWriteTextFormat> pTextFormat;
  HR(pdwr->pDWFactory->CreateTextFormat(
      font_face.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
      font_point * m_dpiScaleFontPoint, L"",
      pTextFormat.ReleaseAndGetAddressOf()));

  if (!pTextFormat) {
    return;
  }

  pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

  ComPtr<IDWriteTextLayout> pTextLayout;
  HR(pdwr->pDWFactory->CreateTextLayout(
      detail_text.c_str(), (UINT32)detail_text.length(), pTextFormat.Get(),
      max_layout_width, 10000.0f, pTextLayout.ReleaseAndGetAddressOf()));

  if (!pTextLayout) {
    return;
  }

  DWRITE_TEXT_METRICS metrics;
  HR(pTextLayout->GetMetrics(&metrics));
  float measured_height = metrics.height;

  // Truncate to max_lines if configured
  if (m_style.detail_max_lines > 0 &&
      metrics.lineCount > (UINT32)m_style.detail_max_lines) {
    std::vector<DWRITE_LINE_METRICS> lineMetrics(m_style.detail_max_lines);
    UINT32 actualLineCount = 0;
    pTextLayout->GetLineMetrics(lineMetrics.data(), m_style.detail_max_lines,
                                &actualLineCount);
    float trimmed_height = 0.0f;
    for (UINT32 i = 0; i < actualLineCount; ++i) {
      trimmed_height += lineMetrics[i].height;
    }

    DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> pEllipsis;
    pdwr->pDWFactory->CreateEllipsisTrimmingSign(
        pTextFormat.Get(), pEllipsis.ReleaseAndGetAddressOf());
    pTextLayout->SetTrimming(&trimming, pEllipsis.Get());
    pTextLayout->SetMaxHeight(trimmed_height);
    measured_height = trimmed_height;
  }

  int content_width =
      (int)ceil(metrics.widthIncludingTrailingWhitespace) + 2 * padding_x;
  content_width = max(content_width, configured_width);
  if (m_style.detail_min_width > 0) {
    content_width = max(content_width, DPI_SCALE(m_style.detail_min_width));
  }
  if (m_style.detail_max_width > 0) {
    content_width = min(content_width, DPI_SCALE(m_style.detail_max_width));
  }
  int content_height = (int)ceil(measured_height) + 2 * padding_y;

  // Calculate screen position with work area bounds & flipping
  HMONITOR hMon = MonitorFromRect(&rcCandidate, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  RECT rcWork = {0, 0, 1920, 1080};
  if (hMon && GetMonitorInfo(hMon, &mi)) {
    rcWork = mi.rcWork;
  }

  DetailPanelRect cand_rect = {rcCandidate.left, rcCandidate.top,
                               rcCandidate.right, rcCandidate.bottom};
  DetailPanelRect work_rect = {rcWork.left, rcWork.top, rcWork.right,
                               rcWork.bottom};

  DetailPanelGeometryConfig geom_config;
  geom_config.preferred_position =
      static_cast<DetailPanelPosition>(m_style.detail_position);
  geom_config.gap = DPI_SCALE(m_style.detail_gap > 0 ? m_style.detail_gap : 8);
  geom_config.min_width = DPI_SCALE(m_style.detail_min_width);
  geom_config.max_width = DPI_SCALE(m_style.detail_max_width);
  geom_config.max_lines = m_style.detail_max_lines;
  geom_config.padding_x = padding_x;
  geom_config.padding_y = padding_y;

  DetailPanelRect pos = CalculateDetailPanelPosition(
      cand_rect, work_rect, content_width, content_height, geom_config);

  m_current_content_width = content_width;
  m_current_content_height = content_height;
  m_current_pos = pos;
  m_last_detail_text = detail_text;
  m_last_candidate_index = candidate_index;
  m_last_candidate_rect = rcCandidate;

  _Render(detail_text, pos, content_width, content_height, padding_x,
          padding_y);

  Show();
}

void WeaselDetailPanel::Reposition(const CRect& rcCandidate) {
  if (!IsWindow() || !IsWindowVisible() || m_current_content_width <= 0) {
    return;
  }

  _UpdateDpi(rcCandidate);

  HMONITOR hMon = MonitorFromRect(&rcCandidate, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  RECT rcWork = {0, 0, 1920, 1080};
  if (hMon && GetMonitorInfo(hMon, &mi)) {
    rcWork = mi.rcWork;
  }

  DetailPanelRect cand_rect = {rcCandidate.left, rcCandidate.top,
                               rcCandidate.right, rcCandidate.bottom};
  DetailPanelRect work_rect = {rcWork.left, rcWork.top, rcWork.right,
                               rcWork.bottom};

  DetailPanelGeometryConfig geom_config;
  geom_config.preferred_position =
      static_cast<DetailPanelPosition>(m_style.detail_position);
  geom_config.gap = DPI_SCALE(m_style.detail_gap > 0 ? m_style.detail_gap : 8);
  geom_config.min_width = DPI_SCALE(m_style.detail_min_width);
  geom_config.max_width = DPI_SCALE(m_style.detail_max_width);

  DetailPanelRect pos = CalculateDetailPanelPosition(
      cand_rect, work_rect, m_current_content_width, m_current_content_height,
      geom_config);

  m_current_pos = pos;
  m_last_candidate_rect = rcCandidate;

  int shadow_radius = DPI_SCALE(m_style.shadow_radius);
  int blurMarginX = shadow_radius;
  int blurMarginY = shadow_radius;
  int win_x = pos.left - blurMarginX;
  int win_y = pos.top - blurMarginY;

  SetWindowPos(HWND_TOPMOST, win_x, win_y, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void WeaselDetailPanel::_Render(const std::wstring& detail_text,
                                const DetailPanelRect& pos,
                                int content_width,
                                int content_height,
                                int padding_x,
                                int padding_y) {
  if (!IsWindow()) {
    return;
  }

  int shadow_radius = DPI_SCALE(m_style.shadow_radius);
  int blurMarginX = shadow_radius;
  int blurMarginY = shadow_radius;

  int win_width = content_width + blurMarginX * 2;
  int win_height = content_height + blurMarginY * 2;
  int win_x = pos.left - blurMarginX;
  int win_y = pos.top - blurMarginY;

  // Prepare memory DC and 32-bit PARGB bitmap
  HDC hScreenDC = ::GetDC(NULL);
  HDC hMemDC = ::CreateCompatibleDC(hScreenDC);

  BITMAPINFO bmi = {0};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = win_width;
  bmi.bmiHeader.biHeight = -win_height;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* pBits = nullptr;
  HBITMAP hBitmap =
      ::CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
  HBITMAP hOldBmp = (HBITMAP)::SelectObject(hMemDC, hBitmap);

  // Initialize GDI+ graphics for shadow, background, border
  Gdiplus::Graphics g(hMemDC);
  g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

  CRect rcPanel(blurMarginX, blurMarginY, blurMarginX + content_width,
                blurMarginY + content_height);
  int round_corner = DPI_SCALE(m_style.round_corner_ex ? m_style.round_corner_ex
                                                       : m_style.round_corner);

  COLORREF shadow_color = m_style.shadow_color;
  // Draw shadow if configured
  if (shadow_radius > 0 && COLORNOTTRANSPARENT(shadow_color)) {
    CRect rect(
        blurMarginX + DPI_SCALE(m_style.shadow_offset_x),
        blurMarginY + DPI_SCALE(m_style.shadow_offset_y),
        rcPanel.Width() + blurMarginX + DPI_SCALE(m_style.shadow_offset_x),
        rcPanel.Height() + blurMarginY + DPI_SCALE(m_style.shadow_offset_y));

    BYTE r = GetRValue(shadow_color);
    BYTE g = GetGValue(shadow_color);
    BYTE b = GetBValue(shadow_color);
    BYTE alpha = (BYTE)((shadow_color >> 24) & 255);
    Gdiplus::Color gShadowColor = Gdiplus::Color::MakeARGB(alpha, r, g, b);

    Gdiplus::Bitmap shadowBmp(win_width, win_height, PixelFormat32bppPARGB);
    Gdiplus::Graphics gShadow(&shadowBmp);
    gShadow.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    GraphicsRoundRectPath shadowPath(rect, round_corner);
    Gdiplus::SolidBrush shadowBrush(gShadowColor);
    gShadow.FillPath(&shadowBrush, &shadowPath);

    DoGaussianBlur(&shadowBmp, (float)shadow_radius, (float)shadow_radius);
    g.DrawImage(&shadowBmp, 0, 0);
  }

  // Draw background
  COLORREF back_color_ref =
      m_style.detail_back_color
          ? m_style.detail_back_color
          : (m_style.candidate_back_color ? m_style.candidate_back_color
                                          : m_style.back_color);
  if (COLORNOTTRANSPARENT(back_color_ref)) {
    Gdiplus::Color back_color = GDPCOLOR_FROM_COLORREF(back_color_ref);
    Gdiplus::SolidBrush back_brush(back_color);
    GraphicsRoundRectPath back_path(rcPanel, round_corner);
    g.FillPath(&back_brush, &back_path);
  }

  // Draw border
  COLORREF border_color_ref =
      m_style.detail_border_color
          ? m_style.detail_border_color
          : (m_style.candidate_border_color ? m_style.candidate_border_color
                                            : m_style.border_color);
  int border_width = DPI_SCALE(m_style.border);
  if (border_width > 0 && COLORNOTTRANSPARENT(border_color_ref)) {
    Gdiplus::Color border_color = GDPCOLOR_FROM_COLORREF(border_color_ref);
    Gdiplus::Pen pen(border_color, (Gdiplus::REAL)border_width);
    GraphicsRoundRectPath border_path(rcPanel, round_corner);
    g.DrawPath(&pen, &border_path);
  }

  // Draw text using DirectWrite
  auto pdwr = m_ui.pdwr();
  if (pdwr && pdwr->pDWFactory && pdwr->pD2d1Factory) {
    if (!m_pRenderTarget) {
      D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
          0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
      pdwr->pD2d1Factory->CreateDCRenderTarget(
          &props, m_pRenderTarget.ReleaseAndGetAddressOf());
    }

    if (m_pRenderTarget) {
      RECT rcBind = {0, 0, win_width, win_height};
      if (SUCCEEDED(m_pRenderTarget->BindDC(hMemDC, &rcBind))) {
        const std::wstring font_face = m_style.comment_font_face.empty()
                                           ? m_style.font_face
                                           : m_style.comment_font_face;
        int font_point =
            m_style.comment_font_point > 0
                ? m_style.comment_font_point
                : (m_style.font_point > 0 ? m_style.font_point : 12);

        ComPtr<IDWriteTextFormat> pTextFormat;
        pdwr->pDWFactory->CreateTextFormat(
            font_face.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            font_point * m_dpiScaleFontPoint, L"",
            pTextFormat.ReleaseAndGetAddressOf());

        if (pTextFormat) {
          pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
          pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
          pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

          float max_text_width = (float)max(20, content_width - 2 * padding_x);
          float max_text_height = (float)content_height;

          ComPtr<IDWriteTextLayout> pTextLayout;
          pdwr->pDWFactory->CreateTextLayout(
              detail_text.c_str(), (UINT32)detail_text.length(),
              pTextFormat.Get(), max_text_width, max_text_height,
              pTextLayout.ReleaseAndGetAddressOf());

          if (pTextLayout) {
            if (m_style.detail_max_lines > 0) {
              std::vector<DWRITE_LINE_METRICS> lineMetrics(
                  m_style.detail_max_lines);
              UINT32 actualLineCount = 0;
              pTextLayout->GetLineMetrics(lineMetrics.data(),
                                          m_style.detail_max_lines,
                                          &actualLineCount);
              float trimmed_height = 0.0f;
              for (UINT32 i = 0; i < actualLineCount; ++i) {
                trimmed_height += lineMetrics[i].height;
              }

              DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                                          0, 0};
              ComPtr<IDWriteInlineObject> pEllipsis;
              pdwr->pDWFactory->CreateEllipsisTrimmingSign(
                  pTextFormat.Get(), pEllipsis.ReleaseAndGetAddressOf());
              pTextLayout->SetTrimming(&trimming, pEllipsis.Get());
              pTextLayout->SetMaxHeight(trimmed_height);
            }

            COLORREF text_color_ref =
                m_style.detail_text_color
                    ? m_style.detail_text_color
                    : (m_style.comment_text_color
                           ? m_style.comment_text_color
                           : (m_style.candidate_text_color
                                  ? m_style.candidate_text_color
                                  : m_style.text_color));
            float r = (float)GetRValue(text_color_ref) / 255.0f;
            float g = (float)GetGValue(text_color_ref) / 255.0f;
            float b = (float)GetBValue(text_color_ref) / 255.0f;
            float alpha = (float)((text_color_ref >> 24) & 255) / 255.0f;
            if (alpha <= 0.0f) {
              alpha = 1.0f;
            }

            if (!m_pBrush) {
              m_pRenderTarget->CreateSolidColorBrush(
                  D2D1::ColorF(r, g, b, alpha),
                  m_pBrush.ReleaseAndGetAddressOf());
            } else {
              m_pBrush->SetColor(D2D1::ColorF(r, g, b, alpha));
            }

            m_pRenderTarget->BeginDraw();
            m_pRenderTarget->DrawTextLayout(
                D2D1::Point2F((float)(blurMarginX + padding_x),
                              (float)(blurMarginY + padding_y)),
                pTextLayout.Get(), m_pBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            m_pRenderTarget->EndDraw();
          }
        }
      }
    }
  }

  // UpdateLayeredWindow
  POINT ptDst = {win_x, win_y};
  POINT ptSrc = {0, 0};
  SIZE sz = {win_width, win_height};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0xFF, AC_SRC_ALPHA};

  ::UpdateLayeredWindow(m_hWnd, hScreenDC, &ptDst, &sz, hMemDC, &ptSrc,
                        RGB(0, 0, 0), &bf, ULW_ALPHA);

  ::SelectObject(hMemDC, hOldBmp);
  ::DeleteObject(hBitmap);
  ::DeleteDC(hMemDC);
  ::ReleaseDC(NULL, hScreenDC);
}
