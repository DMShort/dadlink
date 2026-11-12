// Overlay logic checks (offscreen Qt). Prints one JSON line per check: {"name":..,"ok":..,"detail":..}
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <cstdio>

#include "ui/overlay/talk_overlay.h"

// Expose private state for testing
class TestOverlay : public TalkOverlay {
public:
    using TalkOverlay::TalkOverlay;
};

static void report(const char* name, bool ok, const QString& detail = {}) {
    printf("{\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}\n", name, ok ? "true" : "false",
           detail.toUtf8().replace('"', '\'').constData());
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using K = TalkOverlay::Kind;

    OverlayConfig cfg;
    TestOverlay ov(cfg);

    // 1. Idle: invisible (1x1) unless showIdle
    ov.setRows({});
    QSize idle = ov.contentSize();
    report("overlay_idle_invisible", idle == QSize(1, 1), QString("%1x%2").arg(idle.width()).arg(idle.height()));
    cfg.showIdle = true;
    ov.applyConfig(cfg);
    QSize tag = ov.contentSize();
    report("overlay_idle_tag", tag.width() > 20 && tag.height() > 10, QString("%1x%2").arg(tag.width()).arg(tag.height()));
    cfg.showIdle = false;
    ov.applyConfig(cfg);

    // 2. Height grows one row per speaker, capped at 10 + "more"
    std::vector<TalkOverlay::Row> rows;
    for (int i = 0; i < 3; ++i) rows.push_back({K::Speaker, "Fleet", QString("Pilot%1").arg(i)});
    ov.setRows(rows);
    int h3 = ov.contentSize().height();
    rows.clear();
    for (int i = 0; i < 25; ++i) rows.push_back({K::Speaker, "Fleet", QString("Pilot%1").arg(i)});
    ov.setRows(rows);
    int h25 = ov.contentSize().height();
    // rows are 22px + 16px padding at scale 1
    report("overlay_row_height", h3 == 16 + 3 * 22, QString::number(h3));
    report("overlay_caps_at_10_plus_more", h25 == 16 + 11 * 22, QString::number(h25));

    // 3. Scale doubles size
    cfg.scale = 2.0;
    ov.applyConfig(cfg);
    ov.setRows({{K::Speaker, "Fleet", "A"}});
    int h2x = ov.contentSize().height();
    cfg.scale = 1.0;
    ov.applyConfig(cfg);
    report("overlay_scale", h2x == 2 * (16 + 22), QString::number(h2x));

    // 4. Corner anchoring: bottom-right with offsets stays inside the screen edge
    cfg.corner = "bottom-right";
    cfg.offsetX = 30;
    cfg.offsetY = 40;
    ov.applyConfig(cfg);
    ov.setRows({{K::Speaker, "Command", "Overlord"}, {K::Speaker, "Fleet", "Viper-2"}});
    QRect g = QGuiApplication::primaryScreen()->geometry();
    QRect r = ov.geometry();
    bool anchored = r.right() == g.right() - 30 && r.bottom() == g.bottom() - 40;
    report("overlay_corner_anchor", anchored,
           QString("screen %1x%2 overlay %3,%4 %5x%6").arg(g.width()).arg(g.height()).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));

    // 5. Net colours are stable and Command is red
    report("overlay_net_colours",
           TalkOverlay::netColor("Fleet") == TalkOverlay::netColor("fleet") &&
           TalkOverlay::netColor("Command") == QColor(0xFF, 0x52, 0x52) &&
           TalkOverlay::netColor("Fleet") != TalkOverlay::netColor("Command"));

    // 6. Disabled overlay is hidden
    cfg.enabled = false;
    ov.applyConfig(cfg);
    report("overlay_disable_hides", !ov.isVisible());
    return 0;
}
