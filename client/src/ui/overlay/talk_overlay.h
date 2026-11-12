#pragma once

#include <QColor>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "config/config.h"

/// In-game "who's talking" overlay.
///
/// A frameless, translucent, always-on-top window that ignores mouse and keyboard
/// (click-through, never takes focus), so it can sit over Star Citizen in
/// Borderless / Windowed mode. Shows your own transmit state and everyone
/// currently speaking, labelled with the net they're on.
///
/// Note: exclusive fullscreen games draw over every normal window; use Borderless.
class TalkOverlay : public QWidget {
    Q_OBJECT

public:
    enum class Kind { Speaker, SelfTx, SelfListenOnly, Info };

    struct Row {
        Kind kind = Kind::Speaker;
        QString net;   // e.g. "Fleet"
        QString who;   // e.g. "Viper-2" (empty for self rows)
    };

    explicit TalkOverlay(const OverlayConfig& cfg);

    void applyConfig(const OverlayConfig& cfg);
    void setRows(std::vector<Row> rows);

    /// Show sample rows (used while the Overlay settings tab is open).
    void setPreview(bool on);

    /// Stable colour for a net name, shared with the rest of the UI.
    static QColor netColor(const QString& net);

    /// Size the overlay wants for its current rows (used for corner anchoring and tests).
    QSize contentSize() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    void reposition();
    void makeClickThrough();
    const std::vector<Row>& visibleRows() const;

    OverlayConfig m_cfg;
    std::vector<Row> m_rows;
    std::vector<Row> m_previewRows;
    bool m_preview = false;
    QTimer m_topmostTimer;
};
