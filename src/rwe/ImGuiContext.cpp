#include "ImGuiContext.h"
#include <SDL3/SDL_events.h>

namespace rwe
{
    ImGuiContext::~ImGuiContext()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    bool ImGuiContext::processEvent(const SDL_Event& event)
    {
        // Always forward input events to ImGui so its io state stays live:
        // WantCaptureMouse is computed from io.MousePos, which is only
        // updated when motion events actually reach the backend. Gating
        // forwarding on WantCaptureMouse is circular — the mouse could
        // never arrive on a window to trigger capture.
        //
        // Consumption (hiding the event from the game) is a separate
        // decision made per event class.
        switch (event.type)
        {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
                ImGui_ImplSDL3_ProcessEvent(&event);
                return io->WantCaptureMouse;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_TEXT_INPUT:
                ImGui_ImplSDL3_ProcessEvent(&event);
                // Swallow keys only while a text field is actually input-
                // active. WantCaptureKeyboard stays true whenever a window
                // has focus (NavEnableKeyboard) and would deaden every
                // gameplay hotkey while the debug window is merely open.
                return io->WantTextInput;

            default:
                return false;
        }
    }

    ImGuiContext::ImGuiContext(const std::string& iniPath, SDL_Window* window, void* glContext) : iniPath(iniPath)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        io = &ImGui::GetIO();
        io->IniFilename = this->iniPath.data();
        io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForOpenGL(window, glContext);
        ImGui_ImplOpenGL3_Init("#version 150");
    }

    void ImGuiContext::newFrame(SDL_Window* window)
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiContext::render()
    {
        ImGui::Render();
    }

    void ImGuiContext::renderDrawData()
    {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}
