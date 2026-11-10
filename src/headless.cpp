// the ui with no window: frames of the real App drawn into memory and written as a picture.
// IMCODE_SHOT=<file.ppm> runs it (see main). for ui checks where no window can be seen or
// captured: a locked screen, a build machine
#include "app.hpp"

#include "markdown.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

namespace {
    struct SoftTexture {
        int width = 0, height = 0, bytesPerPixel = 4;
        const unsigned char* pixels = nullptr; // the atlas's own, or `owned`
        std::vector<unsigned char> owned;
    };

    struct Canvas {
        int width = 0, height = 0;
        std::vector<unsigned char> rgb;
    };

    // imgui's texture requests, answered by pointing at the pixels it already holds
    void updateTextures(ImDrawData* draw) {
        for (ImTextureData* tex : draw->Textures ? *draw->Textures : ImVector<ImTextureData*>()) {
            if (tex->Status == ImTextureStatus_WantCreate) {
                auto* soft = new SoftTexture{tex->Width, tex->Height, tex->BytesPerPixel,
                                             tex->Pixels, {}};
                tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(soft)));
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantUpdates) {
                tex->SetStatus(ImTextureStatus_OK); // same buffer
            } else if (tex->Status == ImTextureStatus_WantDestroy) {
                delete reinterpret_cast<SoftTexture*>(static_cast<uintptr_t>(tex->GetTexID()));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    // nearest texel as premultiplied-free rgba in 0..1
    void sample(const SoftTexture* tex, float u, float v, float out[4]) {
        if (!tex || !tex->pixels) {
            out[0] = out[1] = out[2] = out[3] = 1;
            return;
        }
        const int x = std::clamp(static_cast<int>(u * tex->width), 0, tex->width - 1);
        const int y = std::clamp(static_cast<int>(v * tex->height), 0, tex->height - 1);
        const unsigned char* p = tex->pixels + (y * tex->width + x) * tex->bytesPerPixel;
        if (tex->bytesPerPixel == 1) {
            out[0] = out[1] = out[2] = 1;
            out[3] = p[0] / 255.0f;
        } else {
            for (int i = 0; i < 4; ++i) {
                out[i] = p[i] / 255.0f;
            }
        }
    }

    float edge(const ImVec2& a, const ImVec2& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    }

    // every triangle of the frame, clipped, textured, blended over the canvas
    void rasterize(const ImDrawData* draw, float scale, Canvas& canvas) {
        for (const ImDrawList* list : draw->CmdLists) {
            for (const ImDrawCmd& cmd : list->CmdBuffer) {
                if (cmd.UserCallback) {
                    continue;
                }
                const auto* tex =
                    reinterpret_cast<const SoftTexture*>(static_cast<uintptr_t>(cmd.GetTexID()));
                const int clipX0 = std::max(0, static_cast<int>(cmd.ClipRect.x * scale));
                const int clipY0 = std::max(0, static_cast<int>(cmd.ClipRect.y * scale));
                const int clipX1 = std::min(canvas.width, static_cast<int>(cmd.ClipRect.z * scale));
                const int clipY1 = std::min(canvas.height, static_cast<int>(cmd.ClipRect.w * scale));
                for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                    const ImDrawVert* v[3];
                    ImVec2 p[3];
                    for (int k = 0; k < 3; ++k) {
                        v[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                        p[k] = ImVec2(v[k]->pos.x * scale, v[k]->pos.y * scale);
                    }
                    const float area = edge(p[0], p[1], p[2].x, p[2].y);
                    if (area == 0) {
                        continue;
                    }
                    const int x0 = std::max(clipX0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
                    const int y0 = std::max(clipY0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
                    const int x1 = std::min(clipX1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
                    const int y1 = std::min(clipY1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
                    for (int y = y0; y < y1; ++y) {
                        for (int x = x0; x < x1; ++x) {
                            const float px = x + 0.5f, py = y + 0.5f;
                            const float w0 = edge(p[1], p[2], px, py) / area;
                            const float w1 = edge(p[2], p[0], px, py) / area;
                            const float w2 = 1 - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) {
                                continue;
                            }
                            float texel[4];
                            sample(tex, w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x,
                                   w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y, texel);
                            float color[4];
                            for (int c = 0; c < 4; ++c) {
                                const float vc = w0 * ((v[0]->col >> (8 * c)) & 0xFF) +
                                                 w1 * ((v[1]->col >> (8 * c)) & 0xFF) +
                                                 w2 * ((v[2]->col >> (8 * c)) & 0xFF);
                                color[c] = vc / 255.0f * texel[c];
                            }
                            unsigned char* dst = &canvas.rgb[(y * canvas.width + x) * 3];
                            for (int c = 0; c < 3; ++c) {
                                dst[c] = static_cast<unsigned char>(
                                    std::clamp(color[c] * color[3] * 255.0f + dst[c] * (1 - color[3]),
                                               0.0f, 255.0f));
                            }
                        }
                    }
                }
            }
        }
    }
} // namespace

// `frames` frames at 60 a second (agents and hooks get real time), the last one to `out`
int runHeadless(App& app, const char* out, int width, int height, int frames) {
    const float scale = 2; // as a retina window: crisp text
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DeltaTime = 1.0f / 60;
    app.headless = true;
    app.upload = [](const uint8_t* rgba, int w, int h) {
        auto* soft = new SoftTexture{w, h, 4, nullptr, {rgba, rgba + static_cast<size_t>(w) * h * 4}};
        soft->pixels = soft->owned.data();
        return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(soft));
    };
    app.setup();

    Canvas canvas{static_cast<int>(width * scale), static_cast<int>(height * scale), {}};
    for (int i = 0; i < frames; ++i) {
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        ImDrawData* draw = ImGui::GetDrawData();
        updateTextures(draw);
        if (i == frames - 1) {
            canvas.rgb.assign(static_cast<size_t>(canvas.width) * canvas.height * 3, 0);
            rasterize(draw, scale, canvas);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::ofstream file(out, std::ios::binary);
    file << "P6\n" << canvas.width << " " << canvas.height << "\n255\n";
    file.write(reinterpret_cast<const char*>(canvas.rgb.data()),
               static_cast<std::streamsize>(canvas.rgb.size()));
    std::printf("%s: %dx%d\n", out, canvas.width, canvas.height);
    return file ? 0 : 1;
}
