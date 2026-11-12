#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <atomic>

#include "input/joystick_input.h"

#ifdef _WIN32
#include <windows.h>
#endif

/// Push-to-Talk manager. Global (background) input from:
///   - keyboard      (low-level keyboard hook)      binding: "<vk>"        e.g. "112" = F1
///   - mouse buttons (low-level mouse hook)         binding: "mouse:middle" | "mouse:x1" | "mouse:x2"
///   - joystick/HOTAS (DirectInput, see JoystickInput) binding: "joy:{guid}:b<N>"
///
/// Keyboard bindings keep the plain number format so existing config files load unchanged.
/// Left/right mouse buttons are deliberately not bindable (they'd fire on every UI click).
class PttManager : public QObject {
    Q_OBJECT

public:
    explicit PttManager(QObject* parent = nullptr);
    ~PttManager() override;

    PttManager(const PttManager&) = delete;
    PttManager& operator=(const PttManager&) = delete;

    /// Install global keyboard + mouse hooks and open joysticks.
    /// hwnd: top-level window handle (needed by DirectInput). Returns true if the keyboard hook installed.
    bool install(void* hwnd = nullptr);

    /// Uninstall hooks and close joysticks.
    void uninstall();

    /// Bind an input to a channel. Replaces any existing binding for that input or that channel.
    void setBinding(const QString& binding, int channelId);
    /// Legacy helper: bind a Windows virtual-key code.
    void setHotkey(int channelId, int vkCode) { setBinding(QString::number(vkCode), channelId); }

    /// Remove the binding for a channel.
    void removeHotkey(int channelId);

    /// Set a default PTT key (used when no channel-specific key is set).
    void setDefaultKey(int vkCode) { m_defaultBinding = vkCode > 0 ? QString::number(vkCode) : QString(); }

    /// Capture mode: the next button/key pressed on any device is reported via
    /// bindingCaptured() instead of triggering PTT. Escape cancels.
    void startCapture();
    void cancelCapture();
    bool isCapturing() const { return m_capturing; }

    /// Re-scan for joysticks (after plugging one in).
    void rescanJoysticks() { m_joystick.rescan(); }
    int joystickCount() const { return m_joystick.deviceCount(); }

    /// Human-readable name, e.g. "F1", "Mouse X1", "VKB Gladiator — Button 12".
    QString displayName(const QString& binding) const;

    /// Convert legacy bindings ("5"/"6" = VK_XBUTTON1/2) to the current format.
    static QString normalizeBinding(const QString& binding);

    /// Whether PTT is currently active (any key held).
    bool isActive() const { return m_active.load(std::memory_order_relaxed); }

    /// The channel ID that PTT is currently targeting (-1 if none).
    int activeChannelId() const { return m_activeChannel.load(std::memory_order_relaxed); }

signals:
    /// Emitted when PTT state changes.
    void pttStateChanged(bool active, int channelId);
    /// Emitted in capture mode with the pressed input's binding string.
    void bindingCaptured(const QString& binding);
    void captureCancelled();

private:
#ifdef _WIN32
    static LRESULT CALLBACK keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    HHOOK m_hook = nullptr;
    HHOOK m_mouseHook = nullptr;
    static PttManager* s_instance;
#endif

    void onInputDown(const QString& binding);
    void onInputUp(const QString& binding);

    JoystickInput m_joystick;
    QMap<QString, int> m_bindings;  // binding -> channelId
    QString m_defaultBinding;       // default PTT input (empty = none)
    QString m_currentDown;          // input currently holding PTT
    bool m_capturing = false;
    std::atomic<bool> m_active{false};
    std::atomic<int> m_activeChannel{-1};
};
