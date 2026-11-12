#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QMap>
#include <vector>
#include <memory>

/// Background joystick / HOTAS button reader (Windows DirectInput 8).
///
/// Opens every attached game controller in non-exclusive background mode, so
/// buttons are read while Star Citizen (or anything else) has focus, without
/// taking the device away from the game. Polls at ~100 Hz and emits a signal
/// on each button edge. Supports up to 128 buttons per device, which covers
/// VKB, Virpil, Thrustmaster and similar sticks/throttles.
///
/// Binding ids look like "joy:{instance-guid}:b<N>" (N is 1-based).
class JoystickInput : public QObject {
    Q_OBJECT

public:
    explicit JoystickInput(QObject* parent = nullptr);
    ~JoystickInput() override;

    JoystickInput(const JoystickInput&) = delete;
    JoystickInput& operator=(const JoystickInput&) = delete;

    /// Initialise DirectInput and open attached devices. hwnd is a top-level
    /// window handle (required by DirectInput, even for background reads).
    bool start(void* hwnd);
    void stop();

    /// Re-enumerate devices (call after plugging in a new stick).
    void rescan();

    /// Friendly product name for a device instance GUID string, if known.
    QString deviceName(const QString& guid) const;

    /// Number of devices currently open.
    int deviceCount() const;

    /// Parse "joy:{guid}:b12" into parts. Returns false if not a joystick binding.
    static bool parseBinding(const QString& binding, QString* guid, int* button);
    static QString makeBinding(const QString& guid, int button);

signals:
    /// binding is "joy:{guid}:b<N>"
    void buttonChanged(const QString& binding, bool pressed);
    void devicesChanged();

private slots:
    void poll();

private:
    struct Device;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    QTimer m_pollTimer;
    QTimer m_reacquireTimer;
    QMap<QString, QString> m_names; // guid -> product name (survives unplug for display)
};
