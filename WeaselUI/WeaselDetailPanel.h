#pragma once

#include <WeaselIPCData.h>
#include <WeaselUI.h>
#include <CandidateDetailPanel.h>
#include "GdiplusBlur.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace weasel {

typedef CWinTraits<WS_POPUP | WS_CLIPSIBLINGS | WS_DISABLED,
                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                       WS_EX_LAYERED>
    CWeaselDetailPanelTraits;

class WeaselDetailPanel
    : public CWindowImpl<WeaselDetailPanel, CWindow, CWeaselDetailPanelTraits> {
 public:
  BEGIN_MSG_MAP(WeaselDetailPanel)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
  END_MSG_MAP()

  LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseActivate(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);

  WeaselDetailPanel(weasel::UI& ui);
  ~WeaselDetailPanel();

  void Update(const std::wstring& detail_text,
              int candidate_index,
              const CRect& rcCandidate);
  void Reposition(const CRect& rcCandidate);
  void Show();
  void Hide();
  void Destroy();

 private:
  template <typename T>
  int DPI_SCALE(T t) const {
    return (int)(t * m_dpiScaleLayout);
  }

  void _UpdateDpi(const CRect& rcCandidate);
  void _Render(const std::wstring& detail_text,
               const DetailPanelRect& pos,
               int content_width,
               int content_height,
               int padding_x,
               int padding_y);

  weasel::UI& m_ui;
  weasel::UIStyle& m_style;

  std::wstring m_last_detail_text;
  int m_last_candidate_index = -1;
  CRect m_last_candidate_rect = {0, 0, 0, 0};
  UINT m_last_dpi = 96;
  float m_dpiScaleLayout = 1.0f;
  float m_dpiScaleFontPoint = 1.0f;

  int m_current_content_width = 0;
  int m_current_content_height = 0;
  DetailPanelRect m_current_pos;

  ComPtr<ID2D1DCRenderTarget> m_pRenderTarget;
  ComPtr<ID2D1SolidColorBrush> m_pBrush;
  ComPtr<ID2D1SolidColorBrush> m_pKeyBrush;
  ComPtr<ID2D1SolidColorBrush> m_pSeparatorBrush;
};

}  // namespace weasel
