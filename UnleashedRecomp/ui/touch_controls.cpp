#include "touch_controls.h"

#include <stdafx.h>
#include <app.h>
#include <gpu/video.h>
#include <hid/hid.h>
#include <patches/aspect_ratio_patches.h>
#include <ui/game_window.h>
#include <ui/button_guide.h>
#include <ui/imgui_utils.h>
#include <ui/input_coords.h>
#include <ui/installer_wizard.h>
#include <user/config.h>
#include <sdl_listener.h>

#ifdef UNLEASHED_RECOMP_IOS

namespace
{
    struct VirtualButton
    {
        ImVec2 center{};
        float drawRadius{};
        float hitRadius{};
        uint16_t button{};
        uint8_t trigger{};
        SDL_FingerID fingerId{ -1 };
        bool pressed{};
    };

    struct VirtualStick
    {
        ImVec2 center{};
        float radius{};
        float hitRadius{};
        SDL_FingerID fingerId{ -1 };
        ImVec2 delta{};
    };

    struct ToggleButton
    {
        ImVec2 min{};
        ImVec2 max{};
        SDL_FingerID fingerId{ -1 };
    };

    static XAMINPUT_GAMEPAD g_gamepadState{};
    static VirtualStick g_leftStick{};
    static VirtualStick g_rightStick{};
    static std::vector<VirtualButton> g_buttons;
    static ToggleButton g_toggleButton{};
    static std::mutex g_touchMutex;

    static float ViewportHeight()
    {
        return Video::s_viewportHeight != 0 ? float(Video::s_viewportHeight) : float(GameWindow::s_height);
    }

    static float TouchScale()
    {
        const float width = Video::s_viewportWidth != 0 ? float(Video::s_viewportWidth) : float(GameWindow::s_width);
        return std::max(0.1f, std::min(width / 1280.0f, ViewportHeight() / 720.0f));
    }

    static ImVec2 ScreenPoint(float x, float y)
    {
        return { std::round(x), std::round(y) };
    }

    static ImVec2 LogicalPoint(float x, float y)
    {
        const float width = Video::s_viewportWidth != 0 ? float(Video::s_viewportWidth) : float(GameWindow::s_width);
        const float scale = TouchScale();
        return ScreenPoint((width - 1280.0f * scale) * 0.5f + x * scale,
            (ViewportHeight() - 720.0f * scale) * 0.5f + y * scale);
    }

    static void ResetGamepadState()
    {
        g_gamepadState = {};
        g_leftStick.fingerId = -1;
        g_leftStick.delta = {};
        g_rightStick.fingerId = -1;
        g_rightStick.delta = {};
        g_toggleButton.fingerId = -1;

        for (auto& button : g_buttons)
        {
            button.fingerId = -1;
            button.pressed = false;
        }
    }

    static void SetButtonLayout(size_t index, const ImVec2& center, float drawRadius, uint16_t button, uint8_t trigger = 0)
    {
        if (index >= g_buttons.size())
            g_buttons.resize(index + 1);

        auto& virtualButton = g_buttons[index];
        virtualButton.center = center;
        virtualButton.drawRadius = drawRadius;
        virtualButton.hitRadius = std::max(drawRadius * 1.2f, 38.0f * TouchScale());
        virtualButton.button = button;
        virtualButton.trigger = trigger;
    }

    static void UpdateLayout()
    {
        const float scale = TouchScale();
        const float stickRadius = 78.0f * scale;
        const float rightStickRadius = 68.0f * scale;
        const float buttonRadius = 42.0f * scale;
        const float smallButtonRadius = 26.0f * scale;

        // Use the extra width on phones without putting controls at the screen
        // edge/notch; on tablets this reduces to the centered 16:9 layout.
        const float width = Video::s_viewportWidth != 0 ? float(Video::s_viewportWidth) : float(GameWindow::s_width);
        const float wing = std::min(90.0f * scale, std::max(0.0f, (width - 1280.0f * scale) * 0.5f));
        auto left = [wing](float x, float y) { auto p = LogicalPoint(x, y); p.x -= wing; return p; };
        auto right = [wing](float x, float y) { auto p = LogicalPoint(x, y); p.x += wing; return p; };

        g_leftStick.center = left(145.0f, 430.0f);
        g_leftStick.radius = stickRadius;
        g_leftStick.hitRadius = stickRadius * 1.25f;

        g_rightStick.center = right(965.0f, 585.0f);
        g_rightStick.radius = rightStickRadius;
        g_rightStick.hitRadius = rightStickRadius * 1.25f;

        SetButtonLayout(0, left(300.0f, 473.0f), smallButtonRadius, XAMINPUT_GAMEPAD_DPAD_UP);
        SetButtonLayout(1, left(300.0f, 557.0f), smallButtonRadius, XAMINPUT_GAMEPAD_DPAD_DOWN);
        SetButtonLayout(2, left(258.0f, 515.0f), smallButtonRadius, XAMINPUT_GAMEPAD_DPAD_LEFT);
        SetButtonLayout(3, left(342.0f, 515.0f), smallButtonRadius, XAMINPUT_GAMEPAD_DPAD_RIGHT);

        SetButtonLayout(4, left(145.0f, 280.0f), 34.0f * scale, XAMINPUT_GAMEPAD_LEFT_SHOULDER);
        SetButtonLayout(5, right(1140.0f, 220.0f), 34.0f * scale, XAMINPUT_GAMEPAD_RIGHT_SHOULDER);
        g_buttons[4].hitRadius = g_buttons[5].hitRadius = 60.0f * scale;

        SetButtonLayout(6, right(1140.0f, 500.0f), buttonRadius, XAMINPUT_GAMEPAD_A);
        SetButtonLayout(7, right(1218.0f, 422.0f), buttonRadius, XAMINPUT_GAMEPAD_B);
        SetButtonLayout(8, right(1062.0f, 422.0f), buttonRadius, XAMINPUT_GAMEPAD_X);
        SetButtonLayout(9, right(1140.0f, 344.0f), buttonRadius, XAMINPUT_GAMEPAD_Y);

        SetButtonLayout(10, LogicalPoint(724.0f, 654.0f), smallButtonRadius, XAMINPUT_GAMEPAD_START);
        SetButtonLayout(11, LogicalPoint(556.0f, 654.0f), smallButtonRadius, XAMINPUT_GAMEPAD_BACK);
        SetButtonLayout(12, left(270.0f, 280.0f), 34.0f * scale, 0, 1);
        SetButtonLayout(13, right(1015.0f, 220.0f), 34.0f * scale, 0, 2);

        g_toggleButton.min = LogicalPoint(606.0f, 620.0f);
        g_toggleButton.max = LogicalPoint(674.0f, 688.0f);
    }

    static bool IsTouchMouseEvent(const SDL_Event* event)
    {
        switch (event->type)
        {
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                return event->button.which == SDL_TOUCH_MOUSEID;

            case SDL_MOUSEMOTION:
                return event->motion.which == SDL_TOUCH_MOUSEID;

            default:
                return false;
        }
    }

    static bool IsPointInToggle(const ImVec2& point)
    {
        return point.x >= g_toggleButton.min.x && point.x <= g_toggleButton.max.x &&
            point.y >= g_toggleButton.min.y && point.y <= g_toggleButton.max.y;
    }

    static VirtualButton* FindButtonAtPoint(const ImVec2& point)
    {
        VirtualButton* nearest = nullptr;
        float nearestDistance = FLT_MAX;
        for (auto& button : g_buttons)
        {
            const ImVec2 delta = { point.x - button.center.x, point.y - button.center.y };
            const float distanceSq = delta.x * delta.x + delta.y * delta.y;
            if (distanceSq <= button.hitRadius * button.hitRadius && distanceSq < nearestDistance)
            {
                nearest = &button;
                nearestDistance = distanceSq;
            }
        }

        return nearest;
    }

    static VirtualStick* FindStickAtPoint(const ImVec2& point)
    {
        auto testStick = [&](VirtualStick& stick) -> VirtualStick*
        {
            const ImVec2 delta = { point.x - stick.center.x, point.y - stick.center.y };
            const float distanceSq = delta.x * delta.x + delta.y * delta.y;
            if (distanceSq <= stick.hitRadius * stick.hitRadius)
                return &stick;

            return nullptr;
        };

        if (auto* stick = testStick(g_leftStick))
            return stick;

        return testStick(g_rightStick);
    }

    static ImVec2 ClampDelta(const ImVec2& delta, float radius)
    {
        const float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
        if (length <= radius || length <= 0.0f)
            return delta;

        const float scale = radius / length;
        return { delta.x * scale, delta.y * scale };
    }

    static void SetStickPoint(VirtualStick& stick, const ImVec2& point)
    {
        stick.delta = ClampDelta({ point.x - stick.center.x, point.y - stick.center.y }, stick.radius);
    }

    static void UpdateStickValue(VirtualStick& stick)
    {
        float normX = stick.delta.x / stick.radius;
        float normY = stick.delta.y / stick.radius;
        const float length = sqrtf(normX * normX + normY * normY);
        if (length > 1.0f)
        {
            normX /= length;
            normY /= length;
        }

        const int16_t axisX = int16_t(normX * 32767.0f);
        const int16_t axisY = int16_t(-normY * 32767.0f);

        if (&stick == &g_leftStick)
        {
            g_gamepadState.sThumbLX = axisX;
            g_gamepadState.sThumbLY = axisY;
        }
        else
        {
            g_gamepadState.sThumbRX = axisX;
            g_gamepadState.sThumbRY = axisY;
        }
    }

    static void ResetStick(VirtualStick& stick)
    {
        stick.fingerId = -1;
        stick.delta = {};

        if (&stick == &g_leftStick)
        {
            g_gamepadState.sThumbLX = 0;
            g_gamepadState.sThumbLY = 0;
        }
        else
        {
            g_gamepadState.sThumbRX = 0;
            g_gamepadState.sThumbRY = 0;
        }
    }

    static void RebuildGamepadButtons()
    {
        g_gamepadState.wButtons = 0;
        g_gamepadState.bLeftTrigger = 0;
        g_gamepadState.bRightTrigger = 0;

        for (const auto& button : g_buttons)
        {
            if (button.pressed)
            {
                g_gamepadState.wButtons |= button.button;
                if (button.trigger == 1)
                    g_gamepadState.bLeftTrigger = 255;
                else if (button.trigger == 2)
                    g_gamepadState.bRightTrigger = 255;
            }
        }
    }

    static bool HandleTogglePress(const ImVec2& point, SDL_FingerID fingerId, bool pressed)
    {
        if (pressed)
        {
            if (!IsPointInToggle(point))
                return false;

            if (g_toggleButton.fingerId < 0)
                g_toggleButton.fingerId = fingerId;

            return true;
        }

        if (g_toggleButton.fingerId != fingerId)
            return false;

        if (IsPointInToggle(point))
        {
            Config::TouchControls = !Config::TouchControls;
            Config::Save();
            ResetGamepadState();
        }

        g_toggleButton.fingerId = -1;
        return true;
    }

    static bool HandlePointerDown(const ImVec2& point, SDL_FingerID fingerId)
    {
        if (HandleTogglePress(point, fingerId, true))
            return true;

        if (!TouchControls::IsEnabled())
            return false;

        if (VirtualButton* button = FindButtonAtPoint(point))
        {
            if (button->fingerId < 0)
            {
                button->fingerId = fingerId;
                button->pressed = true;
                RebuildGamepadButtons();
            }

            return true;
        }

        if (VirtualStick* stick = FindStickAtPoint(point))
        {
            if (stick->fingerId < 0)
            {
                stick->fingerId = fingerId;
                SetStickPoint(*stick, point);
                UpdateStickValue(*stick);
            }

            return true;
        }

        return false;
    }

    static bool HandlePointerMove(const ImVec2& point, SDL_FingerID fingerId)
    {
        if (!TouchControls::IsEnabled())
            return false;

        VirtualButton* ownedButton = nullptr;
        for (auto& button : g_buttons)
        {
            if (button.fingerId == fingerId)
            {
                ownedButton = &button;
                break;
            }
        }

        if (ownedButton)
        {
            VirtualButton* hoveredButton = FindButtonAtPoint(point);
            if (hoveredButton != ownedButton)
            {
                ownedButton->fingerId = -1;
                ownedButton->pressed = false;

                if (hoveredButton && hoveredButton->fingerId < 0)
                {
                    hoveredButton->fingerId = fingerId;
                    hoveredButton->pressed = true;
                }

                RebuildGamepadButtons();
            }

            return true;
        }

        for (VirtualStick* stick : { &g_leftStick, &g_rightStick })
        {
            if (stick->fingerId != fingerId)
                continue;

            SetStickPoint(*stick, point);
            UpdateStickValue(*stick);
            return true;
        }

        return false;
    }

    static bool HandlePointerUp(const ImVec2& point, SDL_FingerID fingerId)
    {
        if (g_toggleButton.fingerId == fingerId)
        {
            HandleTogglePress(point, fingerId, false);
            return true;
        }

        if (!TouchControls::IsEnabled())
            return false;

        for (VirtualStick* stick : { &g_leftStick, &g_rightStick })
        {
            if (stick->fingerId != fingerId)
                continue;

            ResetStick(*stick);
            return true;
        }

        bool releasedButton = false;
        for (auto& button : g_buttons)
        {
            if (button.fingerId == fingerId)
            {
                button.fingerId = -1;
                button.pressed = false;
                releasedButton = true;
            }
        }

        if (releasedButton)
        {
            RebuildGamepadButtons();
            return true;
        }

        return false;
    }

    static void DrawStick(const VirtualStick& stick)
    {
        auto* drawList = ImGui::GetBackgroundDrawList();
        const float scale = TouchScale();
        const bool held = stick.fingerId >= 0;
        drawList->AddCircleFilled(stick.center, stick.radius + 3.0f * scale, IM_COL32(15, 17, 18, 150), 64);
        drawList->AddCircle(stick.center, stick.radius, IM_COL32(197, 203, 202, 185), 64, 2.0f * scale);
        drawList->AddCircle(stick.center, stick.radius - 5.0f * scale, IM_COL32(70, 74, 73, 160), 64, scale);

        // Limit visual travel so the cap stays inside its socket. Input still
        // uses the full radius, independent of this cosmetic movement.
        const ImVec2 knob = { stick.center.x + stick.delta.x * 0.42f, stick.center.y + stick.delta.y * 0.42f };
        const float cap = stick.radius * 0.53f;
        drawList->AddCircleFilled({ knob.x, knob.y + 4.0f * scale }, cap + 2.0f * scale, IM_COL32(0, 0, 0, 115), 48);
        drawList->AddCircleFilled(knob, cap, IM_COL32(39, 43, 42, 220), 48);
        drawList->AddCircle(knob, cap, held ? IM_COL32(146, 207, 65, 255) : IM_COL32(166, 173, 171, 220), 48, 2.0f * scale);
        drawList->AddCircle(knob, cap * 0.76f, IM_COL32(12, 15, 14, 180), 48, 2.0f * scale);
        for (int i = 0; i < 4; ++i)
        {
            const float angle = float(i) * IM_PI * 0.5f;
            drawList->AddCircleFilled({ knob.x + cosf(angle) * cap * 0.86f, knob.y + sinf(angle) * cap * 0.86f }, 1.6f * scale, IM_COL32(189, 195, 193, 200), 8);
        }
    }

    static void DrawDPad()
    {
        auto* drawList = ImGui::GetBackgroundDrawList();
        const float scale = TouchScale();
        const ImVec2 c = { g_buttons[0].center.x, g_buttons[2].center.y };
        drawList->AddCircleFilled(c, 72.0f * scale, IM_COL32(14, 17, 16, 110), 64);
        drawList->AddCircle(c, 72.0f * scale, IM_COL32(171, 179, 176, 125), 64, 2.0f * scale);
        const float arm = 65.0f * scale, neck = 22.0f * scale;
        const ImU32 face = IM_COL32(92, 99, 96, 215);
        drawList->AddRectFilled({ c.x - neck, c.y - arm }, { c.x + neck, c.y + arm }, face, 4.0f * scale);
        drawList->AddRectFilled({ c.x - arm, c.y - neck }, { c.x + arm, c.y + neck }, face, 4.0f * scale);
        const ImVec2 outline[] = {
            { c.x - neck, c.y - arm }, { c.x + neck, c.y - arm },
            { c.x + neck, c.y - neck }, { c.x + arm, c.y - neck },
            { c.x + arm, c.y + neck }, { c.x + neck, c.y + neck },
            { c.x + neck, c.y + arm }, { c.x - neck, c.y + arm },
            { c.x - neck, c.y + neck }, { c.x - arm, c.y + neck },
            { c.x - arm, c.y - neck }, { c.x - neck, c.y - neck }
        };
        drawList->AddPolyline(outline, std::size(outline), IM_COL32(204, 211, 207, 210), ImDrawFlags_Closed, 2.0f * scale);
        for (size_t i = 0; i < 4; ++i)
        {
            const auto& button = g_buttons[i];
            if (button.pressed)
                drawList->AddRectFilled({ button.center.x - 18.0f * scale, button.center.y - 18.0f * scale }, { button.center.x + 18.0f * scale, button.center.y + 18.0f * scale }, IM_COL32(135, 187, 65, 215), 3.0f * scale);
            const ImVec2 direction = { (button.center.x - c.x) / (42.0f * scale), (button.center.y - c.y) / (42.0f * scale) };
            const ImVec2 tangent = { -direction.y, direction.x };
            const float r = 7.0f * scale;
            drawList->AddTriangleFilled(
                { button.center.x + direction.x * r, button.center.y + direction.y * r },
                { button.center.x - direction.x * r + tangent.x * r, button.center.y - direction.y * r + tangent.y * r },
                { button.center.x - direction.x * r - tangent.x * r, button.center.y - direction.y * r - tangent.y * r },
                IM_COL32(24, 29, 26, 230));
        }
        drawList->AddCircleFilled(c, 10.0f * scale, IM_COL32(61, 68, 64, 200), 24);
    }

    static void DrawButton(const VirtualButton& button, EButtonIcon icon)
    {
        auto* drawList = ImGui::GetBackgroundDrawList();
        const float scale = TouchScale();
        const bool bumper = icon == EButtonIcon::LB || icon == EButtonIcon::RB;
        const float radius = button.drawRadius * (button.pressed ? 0.93f : 1.0f);
        const float halfWidth = radius * (bumper ? 1.75f : 1.0f);
        if (!bumper)
        {
            drawList->AddCircleFilled(button.center, button.drawRadius + 3.0f * scale, IM_COL32(12, 15, 13, 150), 48);
            drawList->AddCircle(button.center, button.drawRadius + 3.0f * scale,
                button.pressed ? IM_COL32(233, 255, 208, 255) : IM_COL32(180, 189, 182, 120), 48, 2.0f * scale);
        }
        const auto [uv, texture] = GetButtonIcon(icon, true);
        drawList->AddImage(texture,
            { button.center.x - halfWidth, button.center.y - radius },
            { button.center.x + halfWidth, button.center.y + radius },
            GET_UV_COORDS(uv), IM_COL32(255, 255, 255, button.pressed ? 255 : 210));
    }

    static void DrawToggle()
    {
        auto* drawList = ImGui::GetBackgroundDrawList();
        const bool enabled = TouchControls::IsEnabled();
        const ImVec2 c = { (g_toggleButton.min.x + g_toggleButton.max.x) * 0.5f,
            (g_toggleButton.min.y + g_toggleButton.max.y) * 0.5f };
        const float scale = TouchScale();
        drawList->AddCircleFilled(c, 31.0f * scale, IM_COL32(27, 33, 29, 210), 48);
        drawList->AddCircle(c, 31.0f * scale, enabled ? IM_COL32(137, 211, 44, 235) : IM_COL32(135, 145, 136, 180), 48, 3.0f * scale);
        drawList->AddCircleFilled(c, 24.0f * scale, IM_COL32(202, 210, 202, 230), 48);
        drawList->AddCircleFilled({ c.x, c.y + 3.0f * scale }, 19.0f * scale, IM_COL32(153, 166, 155, 200), 48);
        const ImU32 ink = enabled ? IM_COL32(65, 123, 24, 255) : IM_COL32(75, 86, 78, 255);
        drawList->AddLine({ c.x - 11.0f * scale, c.y - 12.0f * scale }, { c.x + 11.0f * scale, c.y + 12.0f * scale }, ink, 5.0f * scale);
        drawList->AddLine({ c.x + 11.0f * scale, c.y - 12.0f * scale }, { c.x - 11.0f * scale, c.y + 12.0f * scale }, ink, 5.0f * scale);
        if (ImGui::IsMouseHoveringRect(g_toggleButton.min, g_toggleButton.max) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ImGui::SetTooltip("Touch controls");
    }

    class SDLEventListenerForTouchControls : public SDLEventListener
    {
    public:
        bool OnSDLEvent(SDL_Event* event) override
        {
            std::lock_guard lock(g_touchMutex);
            if (event->type == SDL_APP_WILLENTERBACKGROUND || event->type == SDL_APP_DIDENTERFOREGROUND ||
                (event->type == SDL_WINDOWEVENT && (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST || event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)))
            {
                ResetGamepadState();
                return false;
            }
            if (!TouchControls::IsActive())
                return false;

            UpdateLayout();

            switch (event->type)
            {
                case SDL_FINGERDOWN:
                {
                    const ImVec2 point = GetViewportPointFromSDLEvent(event);
                    return HandlePointerDown(point, event->tfinger.fingerId);
                }

                case SDL_FINGERMOTION:
                {
                    const ImVec2 point = GetViewportPointFromSDLEvent(event);
                    return HandlePointerMove(point, event->tfinger.fingerId);
                }

                case SDL_FINGERUP:
                {
                    const ImVec2 point = GetViewportPointFromSDLEvent(event);
                    return HandlePointerUp(point, event->tfinger.fingerId);
                }

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                {
                    // Finger events already handled touch-generated mouse events.
                    if (IsTouchMouseEvent(event) || event->button.button != SDL_BUTTON_LEFT)
                        break;

                    const ImVec2 point = GetViewportPointFromSDLEvent(event);
                    const SDL_FingerID fingerId = 0;
                    if (event->type == SDL_MOUSEBUTTONDOWN)
                        return HandlePointerDown(point, fingerId);

                    return HandlePointerUp(point, fingerId);
                }

                case SDL_MOUSEMOTION:
                {
                    if (IsTouchMouseEvent(event))
                        break;

                    const ImVec2 point = GetViewportPointFromSDLEvent(event);
                    return HandlePointerMove(point, 0);
                }

            }

            return false;
        }
    };

    static SDLEventListenerForTouchControls g_touchControlsListener;
}

void TouchControls::Init()
{
    std::lock_guard lock(g_touchMutex);
    UpdateLayout();
    ResetGamepadState();
}

bool TouchControls::IsActive()
{
    return App::s_isInit && !InstallerWizard::s_isVisible && GameWindow::s_isActive && GameWindow::s_isFocused;
}

bool TouchControls::IsEnabled()
{
    return Config::TouchControls;
}

XAMINPUT_GAMEPAD TouchControls::GetState()
{
    std::lock_guard lock(g_touchMutex);
    return g_gamepadState;
}

void TouchControls::Draw()
{
    std::lock_guard lock(g_touchMutex);
    if (!IsActive())
        return;

    UpdateLayout();
    DrawToggle();

    if (!IsEnabled())
        return;

    DrawStick(g_leftStick);
    DrawStick(g_rightStick);
    DrawDPad();

    static const EButtonIcon icons[] = { EButtonIcon::LB, EButtonIcon::RB, EButtonIcon::A, EButtonIcon::B,
        EButtonIcon::X, EButtonIcon::Y, EButtonIcon::Start, EButtonIcon::Back, EButtonIcon::LT, EButtonIcon::RT };
    for (size_t i = 0; i < std::size(icons); ++i)
        DrawButton(g_buttons[i + 4], icons[i]);
}

#else

void TouchControls::Init() {}
bool TouchControls::IsActive() { return false; }
bool TouchControls::IsEnabled() { return false; }
XAMINPUT_GAMEPAD TouchControls::GetState() { return {}; }
void TouchControls::Draw() {}

#endif
