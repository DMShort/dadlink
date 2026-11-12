#include "talk_overlay.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int kPad = 8;        // panel padding (unscaled)
constexpr int kRowH = 22;      // row height (unscaled)
constexpr int kDot = 8;        // speaking dot diameter
constexpr int kMinW = 150;
constexpr int kMaxRows = 10;   // beyond this: "+N more"
}

TalkOverlay::TalkOverlay(const OverlayConfig& cfg)
    : QWidget(nullptr,
              Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
              Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
              Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_QuitOnClose, false); // never keeps the app alive on its own
    setFocusPolicy(Qt::NoFocus);
    setWindowTitle("DadLink Overlay");

    m_previewRows = {
        {Kind::SelfTx, "Fleet", {}},
        {Kind::Speaker, "Command", "Overlord"},
        {Kind::Speaker, "Fleet", "Viper-2"},
        {Kind::Speaker, "Ground", "Sgt Rook"},
    };

    // Games sometimes push themselves above other topmost windows; re-assert periodically.
    m_topmostTimer.setInterval(2000);
    connect(&m_topmostTimer, &QTimer::timeout, this, [this]() {
#ifdef _WIN32
        if (isVisible()) {
            SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
#endif
    });

    applyConfig(cfg);
}

void TalkOverlay::applyConfig(const OverlayConfig& cfg) {
    m_cfg = cfg;
    m_cfg.scale = std::clamp(m_cfg.scale, 0.75, 2.0);
    m_cfg.opacity = std::clamp(m_cfg.opacity, 0.3, 1.0);
    setWindowOpacity(m_cfg.opacity);

    if (m_cfg.enabled || m_preview) {
        reposition();
        if (!isVisible()) show();
        m_topmostTimer.start();
    } else {
        hide();
        m_topmostTimer.stop();
    }
    update();
}

void TalkOverlay::setRows(std::vector<Row> rows) {
    // Self rows first, then speakers grouped by net, then name
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        auto rank = [](Kind k) { return k == Kind::Speaker ? 1 : 0; };
        if (rank(a.kind) != rank(b.kind)) return rank(a.kind) < rank(b.kind);
        if (a.net != b.net) return a.net.localeAwareCompare(b.net) < 0;
        return a.who.localeAwareCompare(b.who) < 0;
    });
    bool same = rows.size() == m_rows.size() &&
        std::equal(rows.begin(), rows.end(), m_rows.begin(), [](const Row& a, const Row& b) {
            return a.kind == b.kind && a.net == b.net && a.who == b.who;
        });
    if (same) return;
    m_rows = std::move(rows);
    if (!m_preview) {
        reposition();
        update();
    }
}

void TalkOverlay::setPreview(bool on) {
    m_preview = on;
    applyConfig(m_cfg);
}

const std::vector<TalkOverlay::Row>& TalkOverlay::visibleRows() const {
    return m_preview ? m_previewRows : m_rows;
}

QColor TalkOverlay::netColor(const QString& net) {
    // Distinct, readable on dark backgrounds; stable per net name
    static const QColor palette[] = {
        QColor(0x4F, 0xC3, 0xF7), QColor(0xFF, 0xB7, 0x4D), QColor(0x81, 0xC7, 0x84),
        QColor(0xBA, 0x68, 0xC8), QColor(0xF0, 0x62, 0x92), QColor(0xFF, 0xF1, 0x76),
        QColor(0x4D, 0xD0, 0xE1), QColor(0xA1, 0x88, 0x7F),
    };
    const QString key = net.trimmed().toLower();
    if (key == "command") return QColor(0xFF, 0x52, 0x52); // command always red
    uint h = qHash(key);
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

QSize TalkOverlay::contentSize() const {
    const auto& rows = visibleRows();
    const double s = m_cfg.scale;
    QFont f = font();
    f.setPointSizeF(9.5 * s);
    f.setBold(true);
    QFontMetrics fm(f);

    if (rows.empty()) {
        if (!m_cfg.showIdle) return {1, 1};
        return {int((kPad * 2) * s) + fm.horizontalAdvance("DadLink"), int((kRowH + kPad * 2) * s)};
    }

    int shown = std::min<int>(static_cast<int>(rows.size()), kMaxRows);
    int w = int(kMinW * s);
    for (int i = 0; i < shown; ++i) {
        const Row& r = rows[i];
        QString text = r.kind == Kind::Speaker ? QString("%1  %2").arg(r.net, r.who)
                     : r.kind == Kind::SelfTx ? QString("TX  %1").arg(r.net)
                     : r.kind == Kind::SelfListenOnly ? QString("LISTEN ONLY  %1").arg(r.net)
                     : r.net;
        w = std::max(w, int((kPad * 2 + kDot + 10) * s) + fm.horizontalAdvance(text));
    }
    int extra = static_cast<int>(rows.size()) > kMaxRows ? 1 : 0;
    int h = int((kPad * 2 + kRowH * (shown + extra)) * s);
    return {w, h};
}

void TalkOverlay::reposition() {
    QSize sz = contentSize();
    QList<QScreen*> screens = QGuiApplication::screens();
    QScreen* scr = (m_cfg.screen >= 0 && m_cfg.screen < screens.size()) ? screens[m_cfg.screen]
                                                                         : QGuiApplication::primaryScreen();
    if (!scr) return;
    QRect g = scr->geometry();
    const bool right = m_cfg.corner.endsWith("right");
    const bool bottom = m_cfg.corner.startsWith("bottom");
    int x = right ? g.right() - m_cfg.offsetX - sz.width() + 1 : g.left() + m_cfg.offsetX;
    int y = bottom ? g.bottom() - m_cfg.offsetY - sz.height() + 1 : g.top() + m_cfg.offsetY;
    setGeometry(x, y, sz.width(), sz.height());
}

void TalkOverlay::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    makeClickThrough();
}

void TalkOverlay::makeClickThrough() {
#ifdef _WIN32
    // Belt and braces on top of Qt::WindowTransparentForInput: never activate, never take clicks,
    // and stay out of Alt-Tab.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void TalkOverlay::paintEvent(QPaintEvent*) {
    const auto& rows = visibleRows();
    if (rows.empty() && !m_cfg.showIdle) return;

    const double s = m_cfg.scale;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Panel
    QPainterPath panel;
    panel.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6 * s, 6 * s);
    p.fillPath(panel, QColor(12, 14, 18, 200));
    p.setPen(QPen(QColor(255, 255, 255, 40), 1));
    p.drawPath(panel);

    QFont f = font();
    f.setPointSizeF(9.5 * s);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);

    if (rows.empty()) {
        p.setPen(QColor(180, 180, 180));
        p.drawText(rect(), Qt::AlignCenter, "DadLink");
        return;
    }

    const int shown = std::min<int>(static_cast<int>(rows.size()), kMaxRows);
    int y = int(kPad * s);
    const int rowH = int(kRowH * s);
    const int x0 = int(kPad * s);
    const int dot = int(kDot * s);

    for (int i = 0; i < shown; ++i) {
        const Row& r = rows[i];
        const int cy = y + rowH / 2;
        QColor accent = netColor(r.net);
        QColor dotColor = r.kind == Kind::SelfTx ? QColor(255, 60, 60)
                        : r.kind == Kind::SelfListenOnly ? QColor(255, 165, 0)
                        : QColor(80, 220, 120);

        p.setPen(Qt::NoPen);
        p.setBrush(dotColor);
        p.drawEllipse(QPointF(x0 + dot / 2.0, cy), dot / 2.0, dot / 2.0);

        int x = x0 + dot + int(10 * s);
        auto draw = [&](const QString& text, const QColor& c) {
            p.setPen(c);
            p.drawText(QRect(x, y, width() - x, rowH), Qt::AlignVCenter | Qt::AlignLeft, text);
            x += fm.horizontalAdvance(text);
        };

        switch (r.kind) {
        case Kind::SelfTx:
            draw("TX  ", QColor(255, 90, 90));
            draw(r.net, accent);
            break;
        case Kind::SelfListenOnly:
            draw("LISTEN ONLY  ", QColor(255, 165, 0));
            draw(r.net, accent);
            break;
        case Kind::Info:
            draw(r.net, QColor(200, 200, 200));
            break;
        case Kind::Speaker:
            draw(r.net, accent);
            draw("  ", Qt::white);
            draw(r.who, QColor(240, 240, 240));
            break;
        }
        y += rowH;
    }
    if (static_cast<int>(rows.size()) > kMaxRows) {
        p.setPen(QColor(170, 170, 170));
        p.drawText(QRect(x0, y, width() - x0, rowH), Qt::AlignVCenter | Qt::AlignLeft,
                   QString("+%1 more").arg(rows.size() - kMaxRows));
    }
}
