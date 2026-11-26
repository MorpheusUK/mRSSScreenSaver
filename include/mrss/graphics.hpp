// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include "settings.hpp"
#include <memory>
#include <windows.h>

namespace mrss
{
struct Pixels
{
    unsigned width = 0, height = 0;
    bool hdr = false;
    std::vector<float> rgba;
    std::vector<std::byte> profile;
};
Pixels decodeImage(const std::filesystem::path &path);
std::wstring codecStatus();
class Renderer
{
  public:
    explicit Renderer(HWND window);
    ~Renderer();
    void resize();
    void setImage(const Pixels &image);
    void clear();
    void draw(float progress, Transition transition, const Settings &settings, float motionProgress = 0,
              std::wstring_view caption = {}, const MetadataOptions *metadata = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace mrss
