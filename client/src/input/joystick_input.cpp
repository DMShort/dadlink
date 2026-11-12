#include "joystick_input.h"

#include <QDebug>
#include <QRegularExpression>
#include <QUuid>

#ifdef _WIN32
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>
#endif

namespace {
constexpr int PollIntervalMs = 10;       // 100 Hz — well under one 20ms Opus frame
constexpr int ReacquireIntervalMs = 2000;
constexpr int MaxButtons = 128;          // DIJOYSTATE2 limit
}

#ifdef _WIN32

struct JoystickInput::Device {
    IDirectInputDevice8W* dev = nullptr;
    QString guid;
    QString name;
    int buttonCount = 0;
    bool acquired = false;
    bool pressed[MaxButtons] = {};
};

struct JoystickInput::Impl {
    IDirectInput8W* di = nullptr;
    HWND hwnd = nullptr;
    std::vector<std::unique_ptr<Device>> devices;

    ~Impl() { releaseAll(); if (di) { di->Release(); di = nullptr; } }

    void releaseAll() {
        for (auto& d : devices) {
            if (d->dev) {
                d->dev->Unacquire();
                d->dev->Release();
                d->dev = nullptr;
            }
        }
        devices.clear();
    }

    static BOOL CALLBACK enumCallback(LPCDIDEVICEINSTANCEW inst, LPVOID ctx) {
        auto* self = static_cast<Impl*>(ctx);
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(self->di->CreateDevice(inst->guidInstance, &dev, nullptr)) || !dev) {
            return DIENUM_CONTINUE;
        }
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(dev->SetCooperativeLevel(self->hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            dev->Release();
            return DIENUM_CONTINUE;
        }

        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        int buttons = MaxButtons;
        if (SUCCEEDED(dev->GetCapabilities(&caps))) {
            buttons = static_cast<int>(caps.dwButtons);
            if (buttons > MaxButtons) buttons = MaxButtons;
        }
        if (buttons <= 0) { // axis-only device (e.g. pedals) — nothing to bind
            dev->Release();
            return DIENUM_CONTINUE;
        }

        auto d = std::make_unique<Device>();
        d->dev = dev;
        d->guid = QUuid(inst->guidInstance).toString(QUuid::WithBraces).toUpper();
        d->name = QString::fromWCharArray(inst->tszProductName).trimmed();
        d->buttonCount = buttons;
        d->acquired = SUCCEEDED(dev->Acquire());
        self->devices.push_back(std::move(d));
        return DIENUM_CONTINUE;
    }
};

#else

struct JoystickInput::Device {};
struct JoystickInput::Impl {};

#endif

JoystickInput::JoystickInput(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    m_pollTimer.setInterval(PollIntervalMs);
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &JoystickInput::poll);

    m_reacquireTimer.setInterval(ReacquireIntervalMs);
    connect(&m_reacquireTimer, &QTimer::timeout, this, [this]() {
#ifdef _WIN32
        for (auto& d : m_impl->devices) {
            if (!d->acquired && d->dev) d->acquired = SUCCEEDED(d->dev->Acquire());
        }
#endif
    });
}

JoystickInput::~JoystickInput() {
    stop();
}

bool JoystickInput::start(void* hwnd) {
#ifdef _WIN32
    if (!m_impl->di) {
        HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                        IID_IDirectInput8W,
                                        reinterpret_cast<void**>(&m_impl->di), nullptr);
        if (FAILED(hr) || !m_impl->di) {
            qWarning() << "[JOY] DirectInput8Create failed" << Qt::hex << hr;
            m_impl->di = nullptr;
            return false;
        }
    }
    m_impl->hwnd = static_cast<HWND>(hwnd);
    rescan();
    m_pollTimer.start();
    m_reacquireTimer.start();
    return true;
#else
    Q_UNUSED(hwnd);
    return false;
#endif
}

void JoystickInput::stop() {
    m_pollTimer.stop();
    m_reacquireTimer.stop();
#ifdef _WIN32
    // Release any held buttons so PTT can't stick on
    for (auto& d : m_impl->devices) {
        for (int b = 0; b < d->buttonCount; ++b) {
            if (d->pressed[b]) {
                d->pressed[b] = false;
                emit buttonChanged(makeBinding(d->guid, b + 1), false);
            }
        }
    }
    m_impl->releaseAll();
#endif
}

void JoystickInput::rescan() {
#ifdef _WIN32
    if (!m_impl->di) return;

    // Release held buttons before devices are rebuilt
    for (auto& d : m_impl->devices) {
        for (int b = 0; b < d->buttonCount; ++b) {
            if (d->pressed[b]) emit buttonChanged(makeBinding(d->guid, b + 1), false);
        }
    }
    m_impl->releaseAll();
    m_impl->di->EnumDevices(DI8DEVCLASS_GAMECTRL, &Impl::enumCallback, m_impl.get(),
                            DIEDFL_ATTACHEDONLY);

    for (auto& d : m_impl->devices) {
        m_names[d->guid] = d->name;
        qDebug() << "[JOY] Opened" << d->name << d->guid << "buttons:" << d->buttonCount
                 << (d->acquired ? "" : "(not yet acquired)");
    }
    emit devicesChanged();
#endif
}

void JoystickInput::poll() {
#ifdef _WIN32
    for (auto& d : m_impl->devices) {
        if (!d->dev || !d->acquired) continue;

        d->dev->Poll(); // required for some devices; harmless for others
        DIJOYSTATE2 st{};
        HRESULT hr = d->dev->GetDeviceState(sizeof(st), &st);
        if (FAILED(hr)) {
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                d->acquired = SUCCEEDED(d->dev->Acquire());
            } else {
                d->acquired = false;
            }
            if (!d->acquired) {
                // Device unplugged or lost: release everything it was holding
                for (int b = 0; b < d->buttonCount; ++b) {
                    if (d->pressed[b]) {
                        d->pressed[b] = false;
                        emit buttonChanged(makeBinding(d->guid, b + 1), false);
                    }
                }
            }
            continue;
        }

        for (int b = 0; b < d->buttonCount; ++b) {
            bool down = (st.rgbButtons[b] & 0x80) != 0;
            if (down != d->pressed[b]) {
                d->pressed[b] = down;
                emit buttonChanged(makeBinding(d->guid, b + 1), down);
            }
        }
    }
#endif
}

QString JoystickInput::deviceName(const QString& guid) const {
    return m_names.value(guid.toUpper());
}

int JoystickInput::deviceCount() const {
#ifdef _WIN32
    return static_cast<int>(m_impl->devices.size());
#else
    return 0;
#endif
}

bool JoystickInput::parseBinding(const QString& binding, QString* guid, int* button) {
    static const QRegularExpression re(QStringLiteral("^joy:(\\{[0-9A-Fa-f-]{36}\\}):b(\\d+)$"));
    auto m = re.match(binding);
    if (!m.hasMatch()) return false;
    if (guid) *guid = m.captured(1).toUpper();
    if (button) *button = m.captured(2).toInt();
    return true;
}

QString JoystickInput::makeBinding(const QString& guid, int button) {
    return QStringLiteral("joy:%1:b%2").arg(guid.toUpper()).arg(button);
}
