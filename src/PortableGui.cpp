#include "LevelGenPortable.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <string>
#include <vector>

struct GuiState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::string image, output, prefix = "myworld_", status;
    int focused = -1;
    bool links = true;
};

static GuiState g_gui;

static void SetColor(Uint8 r, Uint8 g, Uint8 b) { SDL_SetRenderDrawColor(g_gui.renderer, r, g, b, 255); }
static void Fill(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b) { SDL_FRect rect{x, y, w, h}; SetColor(r, g, b); SDL_RenderFillRect(g_gui.renderer, &rect); }
static void Outline(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b) { SDL_FRect rect{x, y, w, h}; SetColor(r, g, b); SDL_RenderRect(g_gui.renderer, &rect); }

static void Text(float x, float y, const std::string& value, Uint8 r = 24, Uint8 g = 24, Uint8 b = 24)
{
    if (value.empty()) return;
    SetColor(r, g, b); SDL_RenderDebugText(g_gui.renderer, x, y, value.c_str());
}

static std::string& Field(int index) { return index == 0 ? g_gui.image : index == 1 ? g_gui.output : g_gui.prefix; }

static void Generate()
{
    if (g_gui.image.empty() || g_gui.output.empty() || g_gui.prefix.empty()) { g_gui.status = "Map image, output folder, and filename prefix are required."; return; }
    std::vector<std::string> values{"LevelGen", "--cli", "--image", g_gui.image, "--output", g_gui.output, "--prefix", g_gui.prefix};
    if (!g_gui.links) values.push_back("--no-links");
    std::vector<char*> args; for (std::string& value : values) args.push_back(value.data());
    g_gui.status = RunLevelGenCli((int)args.size(), args.data()) == 0 ? "World generated successfully." : "Generation failed. Check the image and output path.";
}

static void Draw()
{
    Fill(0, 0, 560, 235, 240, 240, 240);
    Text(20, 16, "Filenames");
    const char* labels[] = {"Map image:", "Output folder:", "Filenames start with:"};
    for (int i = 0; i < 3; ++i) {
        float y = 44.0f + i * 36.0f; Text(28, y + 4, labels[i]); Fill(155, y, 378, 25, 255, 255, 255); Outline(155, y, 378, 25, g_gui.focused == i ? 0 : 110, g_gui.focused == i ? 120 : 110, g_gui.focused == i ? 215 : 110); Text(160, y + 4, Field(i));
    }
    Fill(28, 158, 14, 14, 255, 255, 255); Outline(28, 158, 14, 14, 80, 80, 80); if (g_gui.links) { Fill(31, 161, 8, 8, 0, 110, 0); } Text(48, 157, "Create level links");
    Fill(390, 157, 143, 29, 235, 235, 235); Outline(390, 157, 143, 29, 80, 80, 80); Text(417, 163, "Generate World");
    Text(20, 202, g_gui.status, g_gui.status.find("success") != std::string::npos ? 0 : 150, g_gui.status.find("success") != std::string::npos ? 100 : 0, 0);
    SDL_RenderPresent(g_gui.renderer);
}

int main(int argc, char** argv)
{
    if (argc > 1) return RunLevelGenCli(argc, argv);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return 1;
    g_gui.window = SDL_CreateWindow("Level Generator", 560, 235, SDL_WINDOW_NOT_RESIZABLE);
    g_gui.renderer = g_gui.window ? SDL_CreateRenderer(g_gui.window, nullptr) : nullptr;
    if (!g_gui.renderer) return 1;
    std::filesystem::path base(SDL_GetBasePath()); base = base.parent_path();
    g_gui.image = (base / "examples" / "levelgen.png").string(); g_gui.output = (base / "levels").string();
    bool running = true; SDL_StartTextInput(g_gui.window);
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                float x = event.button.x, y = event.button.y;
                g_gui.focused = y >= 44 && y < 141 && x >= 155 ? (int)((y - 44) / 36) : -1;
                if (y >= 158 && y < 178 && x >= 28 && x < 210) g_gui.links = !g_gui.links;
                if (y >= 157 && y < 187 && x >= 390 && x < 533) Generate();
            } else if (event.type == SDL_EVENT_TEXT_INPUT && g_gui.focused >= 0) Field(g_gui.focused) += event.text.text;
            else if (event.type == SDL_EVENT_KEY_DOWN && g_gui.focused >= 0 && event.key.key == SDLK_BACKSPACE && !Field(g_gui.focused).empty()) Field(g_gui.focused).pop_back();
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN) Generate();
        }
        Draw(); SDL_Delay(16);
    }
    SDL_DestroyRenderer(g_gui.renderer); SDL_DestroyWindow(g_gui.window); SDL_Quit(); return 0;
}
