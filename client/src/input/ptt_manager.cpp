#include "ptt_manager.h"

#ifdef _WIN32
PttManager* PttManager::s_instance = nullptr;
#endif

namespace {
const QString kMouseMiddle = QStringLiteral("mouse:middle");
const QString kMouseX1     = QStringLiteral("mouse:x1");
const QString kMouseX2     = QStringLiteral("mouse:x2");
constexpr int kVkEscape = 0x1B;
}

PttManager::PttManager(QObject* parent)
    : QObject(parent)
{
#ifdef _WIN32
    s_instance = this;
#endif
    connect(&m_joystick, &JoystickInput::buttonChanged, this,
            [this](const QString& binding, bool pressed) {
                if (pressed) onInputDown(binding);
                else         onInputUp(binding);
            });
}

PttManager::~PttManager() {
    uninstall();
#ifdef _WIN32
    if (s_instance == this) {
        s_instance = nullptr;
    }
#endif
}

bool PttManager::install(void* hwnd) {
#ifdef _WIN32
    if (!m_hook) {
        m_hook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandle(nullptr), 0);
    }
    if (!m_mouseHook) {
        m_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookProc, GetModuleHandle(nullptr), 0);
    }
    if (hwnd) {
        m_joystick.start(hwnd);
    }
    return m_hook != nullptr;
#else
    Q_UNUSED(hwnd);
    return false; // not implemented on non-Windows
#endif
}

void PttManager::uninstall() {
    m_joystick.stop();
#ifdef _WIN32
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
#endif
    // Never leave PTT stuck on
    if (!m_currentDown.isEmpty()) onInputUp(m_currentDown);
}

void PttManager::setBinding(const QString& rawBinding, int channelId) {
    const QString binding = normalizeBinding(rawBinding);
    if (binding.isEmpty() || channelId <= 0) return;
    removeHotkey(channelId);       // one input per channel
    m_bindings.remove(binding);    // one channel per input
    m_bindings[binding] = channelId;
}

void PttManager::removeHotkey(int channelId) {
    for (auto it = m_bindings.begin(); it != m_bindings.end(); ) {
        if (it.value() == channelId) it = m_bindings.erase(it);
        else ++it;
    }
}

void PttManager::startCapture() {
    m_capturing = true;
}

void PttManager::cancelCapture() {
    if (!m_capturing) return;
    m_capturing = false;
    emit captureCancelled();
}

QString PttManager::normalizeBinding(const QString& binding) {
    if (binding == QLatin1String("5")) return kMouseX1; // VK_XBUTTON1 (old config)
    if (binding == QLatin1String("6")) return kMouseX2; // VK_XBUTTON2 (old config)
    if (binding == QLatin1String("4")) return kMouseMiddle; // VK_MBUTTON
    return binding;
}

QString PttManager::displayName(const QString& rawBinding) const {
    const QString binding = normalizeBinding(rawBinding);
    if (binding == kMouseMiddle) return QStringLiteral("Mouse Middle");
    if (binding == kMouseX1)     return QStringLiteral("Mouse X1 (Back)");
    if (binding == kMouseX2)     return QStringLiteral("Mouse X2 (Forward)");

    QString guid;
    int button = 0;
    if (JoystickInput::parseBinding(binding, &guid, &button)) {
        QString dev = m_joystick.deviceName(guid);
        if (dev.isEmpty()) dev = QStringLiteral("Joystick (not connected)");
        return QStringLiteral("%1 — Button %2").arg(dev).arg(button);
    }

    bool ok = false;
    int vk = binding.toInt(&ok);
    if (!ok || vk <= 0) return binding;

#ifdef _WIN32
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    LONG lParam = static_cast<LONG>((scan & 0xFF) << 16);
    if (scan & 0xE000) lParam |= (1 << 24); // extended key (arrows, right ctrl, etc.)
    wchar_t name[64] = {};
    if (scan && GetKeyNameTextW(lParam, name, 64) > 0) {
        return QString::fromWCharArray(name);
    }
#endif
    return QStringLiteral("Key 0x%1").arg(vk, 2, 16, QLatin1Char('0')).toUpper();
}

void PttManager::onInputDown(const QString& binding) {
    if (m_capturing) {
        m_capturing = false;
        if (binding == QString::number(kVkEscape)) emit captureCancelled();
        else emit bindingCaptured(binding);
        return;
    }

    if (!m_currentDown.isEmpty()) return; // another PTT input already held

    int channelId = -1;
    if (m_bindings.contains(binding)) {
        channelId = m_bindings.value(binding);
    } else if (!m_defaultBinding.isEmpty() && binding == m_defaultBinding) {
        channelId = -1; // default key targets current channel (resolved by caller)
    } else {
        return; // not a PTT input
    }

    m_currentDown = binding;
    m_active.store(true, std::memory_order_relaxed);
    m_activeChannel.store(channelId, std::memory_order_relaxed);
    emit pttStateChanged(true, channelId);
}

void PttManager::onInputUp(const QString& binding) {
    if (binding != m_currentDown) return;

    m_currentDown.clear();
    m_active.store(false, std::memory_order_relaxed);
    int channelId = m_activeChannel.load(std::memory_order_relaxed);
    m_activeChannel.store(-1, std::memory_order_relaxed);
    emit pttStateChanged(false, channelId);
}

#ifdef _WIN32
LRESULT CALLBACK PttManager::keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        auto* kbs = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const QString binding = QString::number(static_cast<int>(kbs->vkCode));

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            s_instance->onInputDown(binding); // auto-repeat is ignored: already held
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            s_instance->onInputUp(binding);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK PttManager::mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        switch (wParam) {
        case WM_MBUTTONDOWN: s_instance->onInputDown(kMouseMiddle); break;
        case WM_MBUTTONUP:   s_instance->onInputUp(kMouseMiddle);   break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const QString& b = (HIWORD(ms->mouseData) == XBUTTON1) ? kMouseX1 : kMouseX2;
            if (wParam == WM_XBUTTONDOWN) s_instance->onInputDown(b);
            else                          s_instance->onInputUp(b);
            break;
        }
        default: break; // left/right/move/wheel ignored
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
#endif
