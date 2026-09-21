#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace weasel {

enum class DetailPanelPosition {
  Right = 0,
  Left = 1,
  Top = 2,
  Bottom = 3,
  Auto = 4
};

struct DetailPanelRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }

  bool operator==(const DetailPanelRect& other) const {
    return left == other.left && top == other.top && right == other.right &&
           bottom == other.bottom;
  }
  bool operator!=(const DetailPanelRect& other) const {
    return !(*this == other);
  }
};

struct DetailPanelGeometryConfig {
  DetailPanelPosition preferred_position = DetailPanelPosition::Right;
  int gap = 8;
  int width = 320;
  int min_width = 0;
  int max_width = 0;
  int max_lines = 0;
  int padding_x = 12;
  int padding_y = 10;
};

inline DetailPanelPosition ParseDetailPanelPosition(const std::string& pos) {
  if (pos == "left")
    return DetailPanelPosition::Left;
  if (pos == "top")
    return DetailPanelPosition::Top;
  if (pos == "bottom")
    return DetailPanelPosition::Bottom;
  if (pos == "auto")
    return DetailPanelPosition::Auto;
  return DetailPanelPosition::Right;
}

inline DetailPanelPosition ResolveDetailPanelPosition(
    DetailPanelPosition preferred_position,
    bool is_vertical_layout) {
  if (preferred_position != DetailPanelPosition::Auto)
    return preferred_position;
  return is_vertical_layout ? DetailPanelPosition::Right
                            : DetailPanelPosition::Bottom;
}

inline const char* DetailPanelPositionToString(DetailPanelPosition pos) {
  switch (pos) {
    case DetailPanelPosition::Left:
      return "left";
    case DetailPanelPosition::Top:
      return "top";
    case DetailPanelPosition::Bottom:
      return "bottom";
    case DetailPanelPosition::Auto:
      return "auto";
    case DetailPanelPosition::Right:
    default:
      return "right";
  }
}

/**
 * Calculate the screen placement rect for candidate detail panel with respect
 * to the candidate window rect, display work area, preferred position and
 * boundary flipping.
 */
inline DetailPanelRect CalculateDetailPanelPosition(
    const DetailPanelRect& candidate_window_rect,
    const DetailPanelRect& work_area_rect,
    int detail_width,
    int detail_height,
    const DetailPanelGeometryConfig& config) {
  if (config.min_width > 0 && detail_width < config.min_width) {
    detail_width = config.min_width;
  }
  if (config.max_width > 0 && detail_width > config.max_width) {
    detail_width = config.max_width;
  }

  // Bound to work area dimensions
  if (detail_width > work_area_rect.width()) {
    detail_width = (std::max)(0, work_area_rect.width());
  }
  if (detail_height > work_area_rect.height()) {
    detail_height = (std::max)(0, work_area_rect.height());
  }

  int gap = (std::max)(0, config.gap);
  int x = candidate_window_rect.right + gap;
  int y = candidate_window_rect.top;

  if (config.preferred_position == DetailPanelPosition::Right) {
    int right_space =
        work_area_rect.right - (candidate_window_rect.right + gap);
    int left_space = (candidate_window_rect.left - gap) - work_area_rect.left;

    if (right_space >= detail_width) {
      x = candidate_window_rect.right + gap;
    } else if (left_space >= detail_width) {
      // Auto flip to left side
      x = candidate_window_rect.left - gap - detail_width;
    } else {
      // Not enough space on either side, choose the one with more room and
      // clamp
      if (right_space >= left_space) {
        x = work_area_rect.right - detail_width;
      } else {
        x = work_area_rect.left;
      }
    }
  } else if (config.preferred_position == DetailPanelPosition::Left) {
    int left_space = (candidate_window_rect.left - gap) - work_area_rect.left;
    int right_space =
        work_area_rect.right - (candidate_window_rect.right + gap);

    if (left_space >= detail_width) {
      x = candidate_window_rect.left - gap - detail_width;
    } else if (right_space >= detail_width) {
      // Auto flip to right side
      x = candidate_window_rect.right + gap;
    } else {
      if (left_space >= right_space) {
        x = work_area_rect.left;
      } else {
        x = work_area_rect.right - detail_width;
      }
    }
  } else if (config.preferred_position == DetailPanelPosition::Bottom) {
    y = candidate_window_rect.bottom + gap;
    x = candidate_window_rect.left;

    int bottom_space =
        work_area_rect.bottom - (candidate_window_rect.bottom + gap);
    int top_space = (candidate_window_rect.top - gap) - work_area_rect.top;
    if (bottom_space < detail_height && top_space >= detail_height) {
      y = candidate_window_rect.top - gap - detail_height;
    }
  } else if (config.preferred_position == DetailPanelPosition::Top) {
    y = candidate_window_rect.top - gap - detail_height;
    x = candidate_window_rect.left;

    int top_space = (candidate_window_rect.top - gap) - work_area_rect.top;
    int bottom_space =
        work_area_rect.bottom - (candidate_window_rect.bottom + gap);
    if (top_space < detail_height && bottom_space >= detail_height) {
      y = candidate_window_rect.bottom + gap;
    }
  }

  // Clamping for Left/Right vertical alignment
  if (config.preferred_position == DetailPanelPosition::Right ||
      config.preferred_position == DetailPanelPosition::Left) {
    if (y + detail_height > work_area_rect.bottom) {
      y = work_area_rect.bottom - detail_height;
    }
    if (y < work_area_rect.top) {
      y = work_area_rect.top;
    }
  }

  // Safety clamping for X coordinate
  if (x + detail_width > work_area_rect.right) {
    x = work_area_rect.right - detail_width;
  }
  if (x < work_area_rect.left) {
    x = work_area_rect.left;
  }

  // Safety clamping for Y coordinate
  if (y + detail_height > work_area_rect.bottom) {
    y = work_area_rect.bottom - detail_height;
  }
  if (y < work_area_rect.top) {
    y = work_area_rect.top;
  }

  DetailPanelRect result;
  result.left = x;
  result.top = y;
  result.right = x + detail_width;
  result.bottom = y + detail_height;
  return result;
}

}  // namespace weasel
