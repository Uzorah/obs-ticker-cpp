#include "ticker-dock.hpp"

#include <QColorDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <obs.h>
#include <cstring>


// ─────────────────────────────────────────────────────────────────────────────
// Dark-theme stylesheet constants
// ─────────────────────────────────────────────────────────────────────────────
static const char *S_ROOT =
    "background-color:#0f172a; color:#e2e8f0;";

static const char *S_INPUT =
    "QLineEdit { background:#1e293b; border:1px solid #475569;"
    "  border-radius:4px; color:#e2e8f0; padding:4px 8px; }"
    "QLineEdit:focus { border-color:#3b82f6; }";

static const char *S_COMBO =
    "QComboBox { background:#1e293b; border:1px solid #475569;"
    "  border-radius:4px; color:#e2e8f0; padding:4px 8px; }"
    "QComboBox QAbstractItemView { background:#1e293b; color:#e2e8f0; }";

static const char *S_BTN_PRIMARY =
    "QPushButton { background:#2563eb; color:#fff; border-radius:4px;"
    "  padding:5px 12px; font-weight:600; border:none; }"
    "QPushButton:hover { background:#3b82f6; }"
    "QPushButton:disabled { background:#334155; color:#64748b; }";

static const char *S_BTN_GREEN =
    "QPushButton { background:#166534; color:#86efac; border-radius:4px;"
    "  padding:5px 12px; font-weight:600; border:none; }"
    "QPushButton:hover { background:#15803d; }";

static const char *S_BTN_DANGER =
    "QPushButton { background:#7f1d1d; color:#fca5a5; border-radius:4px;"
    "  padding:4px 8px; border:none; }"
    "QPushButton:hover { background:#991b1b; }";

static const char *S_BTN_NEUTRAL =
    "QPushButton { background:#334155; color:#cbd5e1; border-radius:4px;"
    "  padding:4px 10px; border:none; }"
    "QPushButton:hover { background:#475569; }";

static const char *S_SECTION_HDR =
    "QPushButton { background:#1e293b; color:#e2e8f0; font-weight:600;"
    "  font-size:12px; text-align:left; padding:8px 10px;"
    "  border:1px solid #334155; border-radius:4px; }"
    "QPushButton:hover { background:#263347; }";

static const char *S_CARD =
    "background:#1e293b; border:1px solid #334155; border-radius:6px;";

static const char *S_LABEL_MUTED =
    "color:#94a3b8; font-size:11px;";

// ─────────────────────────────────────────────────────────────────────────────
// OBS source helpers
// ─────────────────────────────────────────────────────────────────────────────

obs_source_t *ticker_find_source()
{
    char name[512] = {};
    obs_enum_sources([](void *param, obs_source_t *src) -> bool {
        if (strcmp(obs_source_get_id(src), "obs_ticker_cpp") == 0) {
            const char *n = obs_source_get_name(src);
            if (n) strncpy(static_cast<char *>(param), n, 511);
            return false;
        }
        return true;
    }, name);

    return (name[0] != '\0') ? obs_get_source_by_name(name) : nullptr;
}

void ticker_push_settings(const TickerSettings &s, const QString &messagesStr)
{
    obs_source_t *src = ticker_find_source();
    if (!src) return;

    obs_data_t *data = obs_source_get_settings(src);

    obs_data_set_bool  (data, "is_live",    s.isLive);
    obs_data_set_string(data, "messages",   messagesStr.toUtf8().constData());
    obs_data_set_int   (data, "speed",      s.speed);
    obs_data_set_int   (data, "bar_height", s.height);

    // Background color → ABGR
    auto toAbgr = [](const QString &hex) -> uint32_t {
        QColor c(hex);
        return (0xFF000000u)
             | ((uint32_t)c.blue()  << 16)
             | ((uint32_t)c.green() <<  8)
             | ((uint32_t)c.red());
    };
    obs_data_set_int(data, "bg_color",   (long long)toAbgr(s.backgroundColor));
    obs_data_set_int(data, "text_color", (long long)toAbgr(s.color));

    obs_data_set_string(data, "font_face",  s.fontFamily.toUtf8().constData());
    obs_data_set_string(data, "font_style", s.fontStyleName.toUtf8().constData());
    obs_data_set_int   (data, "font_size",  s.fontSize);
    obs_data_set_double(data, "scale_x",    s.fontScaleX);
    obs_data_set_double(data, "scale_y",    s.fontScaleY);

    // Separator - automatically add 4 spaces before and after user's input
    QString sep_with_spaces = "    " + s.itemSeparator + "    ";
    obs_data_set_string(data, "sep_text",  sep_with_spaces.toUtf8().constData());

    // Clock
    obs_data_set_bool  (data, "show_clock",      s.showClock);
    obs_data_set_bool  (data, "clock_24h",       s.clockFormat == "24h");
    obs_data_set_int   (data, "clock_font_size", s.clockFontSize);
    obs_data_set_double(data, "clock_scale_x",   s.clockScaleX);
    obs_data_set_double(data, "clock_scale_y",   s.clockScaleY);
    obs_data_set_int   (data, "clock_zone_width", s.clockZoneWidth);
    obs_data_set_int   (data, "clock_sep_color", (long long)toAbgr(s.clockSepColor));
    // Clock divider is fixed at 6px in source

    obs_source_update(src, data);
    obs_data_release(data);
    obs_source_release(src);
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility: ControlRow helper (label left, control right)
// ─────────────────────────────────────────────────────────────────────────────
static QHBoxLayout *ctrlRow(const QString &labelText, QWidget *control)
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto *lbl = new QLabel(labelText);
    lbl->setStyleSheet(S_LABEL_MUTED);
    lbl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);  // Minimum allows shrinking
    
    // Allow control to scale down to very small size
    control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    control->setMinimumWidth(20);  // Very small minimum, allows dock to be thin
    
    row->addWidget(lbl);
    row->addWidget(control, 1);
    return row;
}

// ─────────────────────────────────────────────────────────────────────────────
// CollapsibleSection
// ─────────────────────────────────────────────────────────────────────────────

CollapsibleSection::CollapsibleSection(const QString &title,
                                       bool startOpen, QWidget *parent)
    : QWidget(parent), m_open(startOpen)
{
    setStyleSheet("background:transparent;");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 6);
    outer->setSpacing(0);

    m_header = new QPushButton(
        QString("%1  %2").arg(m_open ? "▾" : "▸").arg(title));
    m_header->setStyleSheet(S_SECTION_HDR);
    m_header->setCursor(Qt::PointingHandCursor);
    connect(m_header, &QPushButton::clicked, this, &CollapsibleSection::toggle);
    outer->addWidget(m_header);

    m_body = new QWidget;
    m_body->setStyleSheet(
        "background:#0a0f1a; border:1px solid #334155;"
        "border-top:none; border-radius:0 0 4px 4px;");
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(10, 10, 10, 10);
    m_bodyLayout->setSpacing(8);
    m_body->setVisible(m_open);
    outer->addWidget(m_body);
}

void CollapsibleSection::toggle()
{
    m_open = !m_open;
    m_body->setVisible(m_open);
    QString t = m_header->text();
    if (m_open) t.replace("▸", "▾"); else t.replace("▾", "▸");
    m_header->setText(t);
}

// ═══════════════════════════════════════════════════════════════════════════
// TickerDock — static instance
// ═══════════════════════════════════════════════════════════════════════════
TickerDock *TickerDock::s_instance = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// TickerDock — constructor
// ─────────────────────────────────────────────────────────────────────────────
TickerDock::TickerDock(QWidget *parent)
    : QWidget(parent)
{
    s_instance = this;
    setObjectName("ticker_control_dock");

    // Default messages matching the React sample data
    m_messages = {
        { QUuid::createUuid().toString(QUuid::WithoutBraces),
          "Welcome to the stream!", true, false },
        { QUuid::createUuid().toString(QUuid::WithoutBraces),
          "Don't forget to follow!", true, false },
        { QUuid::createUuid().toString(QUuid::WithoutBraces),
          "New video coming soon!", false, false },
    };

    setStyleSheet(S_ROOT);
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    setMinimumWidth(200);  // Fixed minimum, everything must fit in 200px
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);  // Allow scaling

    // Tabs
    m_tabs = new QTabWidget(this);
    m_tabs->setStyleSheet(
        "QTabWidget::pane { border:0; background:#0f172a; }"
        "QTabBar::tab { background:#1e293b; color:#94a3b8; padding:5px 8px;"
        "  border:1px solid #334155; border-bottom:none;"
        "  border-radius:4px 4px 0 0; font-size:11px; }"
        "QTabBar::tab:selected { background:#0f172a; color:#e2e8f0; }"
        "QTabBar::tab:hover { color:#e2e8f0; }"
        "QTabBar { font-size:11px; }");

    m_goLiveTab     = new GoLiveTab    (this, m_tabs);
    m_libraryTab    = new LibraryTab   (this, m_tabs);
    m_formattingTab = new FormattingTab(this, m_tabs);

    m_tabs->addTab(m_goLiveTab,     "Live");
    m_tabs->addTab(m_libraryTab,    "Library");
    m_tabs->addTab(m_formattingTab, "Format");

    rootLayout->addWidget(m_tabs);

    // Wire signals so all tabs stay in sync
    connect(this, &TickerDock::messagesChanged, m_goLiveTab,  &GoLiveTab::refresh);
    connect(this, &TickerDock::messagesChanged, m_libraryTab, &LibraryTab::refresh);
    connect(this, &TickerDock::settingsChanged, m_goLiveTab,  &GoLiveTab::refresh);
    connect(this, &TickerDock::presetsChanged,  m_formattingTab, &FormattingTab::refreshPresets);

    // Load saved state (overrides defaults)
    loadState();
    m_formattingTab->loadFromSettings();

    // Push settings to the OBS source once it's been placed in a scene.
    // We retry every 500 ms; the timer stops itself once the source is found.
    auto *startupTimer = new QTimer(this);
    startupTimer->setInterval(50);
    connect(startupTimer, &QTimer::timeout, this, [this, startupTimer]() {
        obs_source_t *src = ticker_find_source();
        if (src) {
            obs_source_release(src); // only needed the check
            pushToSource();
            startupTimer->stop();
        }
    });
    startupTimer->start();
}

TickerDock::~TickerDock()
{
    saveState();
    s_instance = nullptr;
}

TickerDock *TickerDock::instance()
{
    return s_instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Message operations
// ─────────────────────────────────────────────────────────────────────────────

void TickerDock::addMessage(const QString &text, bool temporary)
{
    TickerMessage msg;
    msg.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.text      = text;
    msg.active    = true;
    msg.temporary = temporary;
    m_messages.append(msg);
    emit messagesChanged();
    pushToSource();
}

void TickerDock::removeMessage(const QString &id)
{
    m_messages.erase(
        std::remove_if(m_messages.begin(), m_messages.end(),
                       [&](const TickerMessage &m){ return m.id == id; }),
        m_messages.end());

    // Auto-stop if no active messages remain
    bool anyActive = std::any_of(m_messages.begin(), m_messages.end(),
                                 [](const TickerMessage &m){ return m.active; });
    if (!anyActive) m_settings.isLive = false;

    emit messagesChanged();
    pushToSource();
}

void TickerDock::toggleMessage(const QString &id)
{
    int idx = -1;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == id) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        // Toggle active state
        m_messages[idx].active = !m_messages[idx].active;

        // If activating, move to bottom of the list (end of queue)
        if (m_messages[idx].active) {
            TickerMessage msg = m_messages[idx];
            m_messages.removeAt(idx);
            m_messages.append(msg);
        }
    }

    // Auto-stop if no active messages remain
    bool anyActive = std::any_of(m_messages.begin(), m_messages.end(),
                                 [](const TickerMessage &m){ return m.active; });
    if (!anyActive) m_settings.isLive = false;

    emit messagesChanged();
    pushToSource();
}

void TickerDock::updateMessage(const QString &id, const QString &newText)
{
    for (auto &m : m_messages)
        if (m.id == id) { m.text = newText; break; }
    emit messagesChanged();
    pushToSource();
}

void TickerDock::saveToLibrary(const QString &id)
{
    for (auto &m : m_messages)
        if (m.id == id) { m.temporary = false; break; }
    emit messagesChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings operations
// ─────────────────────────────────────────────────────────────────────────────

void TickerDock::applySettings(const TickerSettings &s)
{
    bool liveChanged = (s.isLive != m_settings.isLive);
    m_settings = s;
    emit settingsChanged();
    pushToSource();
    if (liveChanged) emit messagesChanged();
}

void TickerDock::setLive(bool live)
{
    m_settings.isLive = live;
    emit settingsChanged();
    pushToSource();
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset operations
// ─────────────────────────────────────────────────────────────────────────────

void TickerDock::savePreset(const QString &name)
{
    TickerPreset p;
    p.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name     = name;
    p.settings = m_settings;
    m_presets.append(p);
    emit presetsChanged();
}

void TickerDock::loadPreset(const QString &id)
{
    for (const auto &p : m_presets) {
        if (p.id == id) {
            bool wasLive = m_settings.isLive;
            m_settings = p.settings;
            m_settings.isLive = wasLive;   // preserve live state like React does
            emit settingsChanged();
            m_formattingTab->loadFromSettings();
            pushToSource();
            return;
        }
    }
}

void TickerDock::deletePreset(const QString &id)
{
    m_presets.erase(
        std::remove_if(m_presets.begin(), m_presets.end(),
                       [&](const TickerPreset &p){ return p.id == id; }),
        m_presets.end());
    emit presetsChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// pushToSource — send individual messages (newline-delimited) to OBS source
// The source draws per-message ft2 sources with visual separator rects.
// ─────────────────────────────────────────────────────────────────────────────

void TickerDock::pushToSource()
{
    QStringList parts;
    for (const auto &m : m_messages)
        if (m.active) parts << m.text;

    // Auto-stop if no active messages
    if (parts.isEmpty())
        m_settings.isLive = false;

    // Always push — source needs clock/settings even when stopped.
    // We send the messages even if not live so the source can drain them.
    QString messagesStr = parts.join("\n");
    ticker_push_settings(m_settings, messagesStr);
}

// ─────────────────────────────────────────────────────────────────────────────
// State Persistence (QSettings)
// ─────────────────────────────────────────────────────────────────────────────

// Helpers to serialize/deserialize Settings
static QJsonObject settingsToJson(const TickerSettings &s)
{
    QJsonObject o;
    o["height"]          = s.height;
    o["speed"]           = s.speed;
    o["backgroundColor"] = s.backgroundColor;
    o["fontFamily"]      = s.fontFamily;
    o["fontStyleName"]   = s.fontStyleName;
    o["fontSize"]        = s.fontSize;
    o["color"]           = s.color;
    o["itemSeparator"]   = s.itemSeparator;
    o["fontScaleX"]      = s.fontScaleX;
    o["fontScaleY"]      = s.fontScaleY;
    o["showClock"]       = s.showClock;
    o["clockFormat"]     = s.clockFormat;
    o["clockFontSize"]   = s.clockFontSize;
    o["clockScaleX"]     = s.clockScaleX;
    o["clockScaleY"]     = s.clockScaleY;
    o["clockZoneWidth"]  = s.clockZoneWidth;
    o["clockSepColor"]   = s.clockSepColor;
    o["isLive"]          = s.isLive;
    return o;
}

static TickerSettings jsonToSettings(const QJsonObject &o, const TickerSettings &def)
{
    TickerSettings s = def; // fallback to defaults if keys missing
    if (o.contains("height"))          s.height          = o["height"].toInt();
    if (o.contains("speed"))           s.speed           = o["speed"].toInt();
    if (o.contains("backgroundColor")) s.backgroundColor = o["backgroundColor"].toString();
    if (o.contains("fontFamily"))      s.fontFamily      = o["fontFamily"].toString();
    if (o.contains("fontStyleName"))   s.fontStyleName   = o["fontStyleName"].toString();
    if (o.contains("fontSize"))        s.fontSize        = o["fontSize"].toInt();
    if (o.contains("color"))           s.color           = o["color"].toString();
    if (o.contains("itemSeparator"))   s.itemSeparator   = o["itemSeparator"].toString();
    if (o.contains("fontScaleX"))      s.fontScaleX      = o["fontScaleX"].toDouble();
    if (o.contains("fontScaleY"))      s.fontScaleY      = o["fontScaleY"].toDouble();
    if (o.contains("showClock"))       s.showClock       = o["showClock"].toBool();
    if (o.contains("clockFormat"))     s.clockFormat     = o["clockFormat"].toString();
    if (o.contains("clockFontSize"))   s.clockFontSize   = o["clockFontSize"].toInt();
    if (o.contains("clockScaleX"))     s.clockScaleX     = o["clockScaleX"].toDouble();
    if (o.contains("clockScaleY"))     s.clockScaleY     = o["clockScaleY"].toDouble();
    if (o.contains("clockZoneWidth"))  s.clockZoneWidth  = o["clockZoneWidth"].toInt();
    if (o.contains("clockSepColor"))   s.clockSepColor   = o["clockSepColor"].toString();
    if (o.contains("isLive"))          s.isLive          = o["isLive"].toBool();
    return s;
}

void TickerDock::saveState()
{
    QSettings s("OBS", "TickerPlugin");

    // 1. Settings
    s.setValue("settings", QJsonDocument(settingsToJson(m_settings)).toVariant());

    // 2. Messages
    QJsonArray msgArr;
    for (const auto &m : m_messages) {
        QJsonObject o;
        o["id"]        = m.id;
        o["text"]      = m.text;
        o["active"]    = m.active;
        o["temporary"] = m.temporary;
        msgArr.append(o);
    }
    s.setValue("messages", QJsonDocument(msgArr).toVariant());

    // 3. Presets
    QJsonArray preArr;
    for (const auto &p : m_presets) {
        QJsonObject o;
        o["id"]       = p.id;
        o["name"]     = p.name;
        o["settings"] = settingsToJson(p.settings);
        preArr.append(o);
    }
    s.setValue("presets", QJsonDocument(preArr).toVariant());
}

void TickerDock::loadState()
{
    QSettings s("OBS", "TickerPlugin");
    if (!s.contains("settings")) return; // No save data

    // 1. Settings
    QJsonDocument sDoc = QJsonDocument::fromVariant(s.value("settings"));
    if (!sDoc.isNull() && sDoc.isObject()) {
        m_settings = jsonToSettings(sDoc.object(), m_settings);
    }
    
    // FORCE "Off" state on startup, ignoring saved state
    m_settings.isLive = false;

    // 2. Messages
    QJsonDocument mDoc = QJsonDocument::fromVariant(s.value("messages"));
    if (!mDoc.isNull() && mDoc.isArray()) {
        m_messages.clear();
        QJsonArray arr = mDoc.array();
        for (const auto &val : arr) {
            QJsonObject o = val.toObject();
            TickerMessage msg;
            msg.id        = o["id"].toString();
            msg.text      = o["text"].toString();
            msg.active    = o["active"].toBool();
            msg.temporary = o["temporary"].toBool();
            m_messages.append(msg);
        }
    }

    // 3. Presets
    QJsonDocument pDoc = QJsonDocument::fromVariant(s.value("presets"));
    if (!pDoc.isNull() && pDoc.isArray()) {
        m_presets.clear();
        QJsonArray arr = pDoc.array();
        for (const auto &val : arr) {
            QJsonObject o = val.toObject();
            TickerPreset p;
            p.id       = o["id"].toString();
            p.name     = o["name"].toString();
            p.settings = jsonToSettings(o["settings"].toObject(), TickerSettings());
            m_presets.append(p);
        }
    }

    // Force UI refresh
    emit settingsChanged();
    emit messagesChanged();
    emit presetsChanged();
}

// ═══════════════════════════════════════════════════════════════════════════
// GoLiveTab
// ═══════════════════════════════════════════════════════════════════════════

GoLiveTab::GoLiveTab(TickerDock *dock, QWidget *parent)
    : QWidget(parent), m_dock(dock)
{
    setStyleSheet(S_ROOT);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    // ── Status toggle card ────────────────────────────────────────────────
    auto *statusCard = new QWidget;
    statusCard->setStyleSheet(S_CARD);
    auto *statusRow = new QHBoxLayout(statusCard);
    statusRow->setContentsMargins(10, 8, 10, 8);

    auto *statusTitle = new QLabel("Ticker Status");
    statusTitle->setStyleSheet("font-weight:600; font-size:12px; color:#e2e8f0;"
                               "text-transform:uppercase; letter-spacing:1px;");
    statusRow->addWidget(statusTitle);
    statusRow->addStretch();

    m_statusLabel = new QLabel("OFF");
    m_statusLabel->setStyleSheet("color:#64748b; font-size:11px; font-weight:600;");
    statusRow->addWidget(m_statusLabel);

    m_liveBtn = new QPushButton("Go Live");
    m_liveBtn->setStyleSheet(S_BTN_PRIMARY);
    m_liveBtn->setCursor(Qt::PointingHandCursor);
    m_liveBtn->setFixedWidth(80);
    connect(m_liveBtn, &QPushButton::clicked, this, &GoLiveTab::onToggleLive);
    statusRow->addWidget(m_liveBtn);

    outer->addWidget(statusCard);

    // ── Live queue card ───────────────────────────────────────────────────
    auto *queueCard = new QWidget;
    queueCard->setStyleSheet(S_CARD);
    auto *queueOuter = new QVBoxLayout(queueCard);
    queueOuter->setContentsMargins(0, 0, 0, 0);
    queueOuter->setSpacing(0);

    auto *queueHeader = new QWidget;
    queueHeader->setStyleSheet("background:#1e293b; border-radius:6px 6px 0 0;");
    auto *queueHeaderRow = new QHBoxLayout(queueHeader);
    queueHeaderRow->setContentsMargins(10, 8, 10, 8);
    auto *queueTitle = new QLabel("Live Queue");
    queueTitle->setStyleSheet("font-weight:600; font-size:12px; color:#cbd5e1;");
    queueHeaderRow->addWidget(queueTitle);
    queueOuter->addWidget(queueHeader);

    // Scrollable queue area
    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(
        "QScrollArea { border:none; background:transparent; }"
        "QScrollBar:vertical { background:#1e293b; width:6px; border-radius:3px; }"
        "QScrollBar::handle:vertical { background:#475569; border-radius:3px; }");
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_queueContainer = new QWidget;
    m_queueContainer->setStyleSheet("background:transparent;");
    m_queueLayout = new QVBoxLayout(m_queueContainer);
    m_queueLayout->setContentsMargins(8, 8, 8, 8);
    m_queueLayout->setSpacing(6);
    m_queueLayout->addStretch();
    scrollArea->setWidget(m_queueContainer);
    queueOuter->addWidget(scrollArea);
    outer->addWidget(queueCard, 1);

    // ── Quick-add card ────────────────────────────────────────────────────
    auto *addCard = new QWidget;
    addCard->setStyleSheet(S_CARD);
    auto *addRow = new QHBoxLayout(addCard);
    addRow->setContentsMargins(10, 8, 10, 8);
    addRow->setSpacing(6);

    m_quickInput = new QLineEdit;
    m_quickInput->setPlaceholderText("Quick Add...");
    m_quickInput->setStyleSheet(S_INPUT);
    connect(m_quickInput, &QLineEdit::returnPressed,
            this, &GoLiveTab::onQuickAdd);

    auto *sendBtn = new QPushButton("Send");
    sendBtn->setStyleSheet(S_BTN_PRIMARY);
    sendBtn->setCursor(Qt::PointingHandCursor);
    connect(sendBtn, &QPushButton::clicked, this, &GoLiveTab::onQuickAdd);

    addRow->addWidget(m_quickInput, 1);
    addRow->addWidget(sendBtn);
    outer->addWidget(addCard);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(100);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        obs_source_t *src = ticker_find_source();
        if (!src) return;

        obs_data_t *s = obs_source_get_settings(src);
        int state = (int)obs_data_get_int(s, "runtime_state");
        bool sourceLive = obs_data_get_bool(s, "is_live");
        obs_data_release(s);
        obs_source_release(src);

        // Sync dock if source changed externally (e.g. Lua hotkey)
        if (m_dock->settings().isLive != sourceLive) {
            TickerSettings set = m_dock->settings();
            set.isLive = sourceLive;
            m_dock->applySettings(set);
        }

        // state: 0=STOPPED, 1=ACTIVE, 2=STOPPING
        bool isLiveDesired = m_dock->settings().isLive;

        if (state == 2) { // STOPPING (Draining)
            m_liveBtn->setText("Stopping...");
            m_liveBtn->setEnabled(false);
            m_liveBtn->setStyleSheet(S_BTN_NEUTRAL);
            m_statusLabel->setText("DRAINING");
            m_statusLabel->setStyleSheet("color:#f59e0b; font-size:11px; font-weight:600;");
        } else {
            m_liveBtn->setEnabled(true);
            if (isLiveDesired) {
                m_liveBtn->setText("Stop");
                m_liveBtn->setStyleSheet(S_BTN_DANGER);
                m_statusLabel->setText("LIVE");
                m_statusLabel->setStyleSheet("color:#22c55e; font-size:11px; font-weight:700;");
            } else {
                m_liveBtn->setText("Go Live");
                m_liveBtn->setStyleSheet(S_BTN_PRIMARY);
                m_statusLabel->setText("OFF");
                m_statusLabel->setStyleSheet("color:#64748b; font-size:11px; font-weight:600;");
            }
        }
    });
    m_pollTimer->start();

    refresh();
}

void GoLiveTab::refresh()
{
    // Refresh queue only; button state is handled by poll timer now
    rebuildQueue();
}

void GoLiveTab::rebuildQueue()
{
    // Clear all items except the trailing stretch
    while (m_queueLayout->count() > 1)
        delete m_queueLayout->takeAt(0)->widget();

    const auto &msgs = m_dock->messages();
    bool any = false;

    for (const auto &msg : msgs) {
        if (!msg.active) continue;
        any = true;

        // Row card
        auto *row = new QWidget;
        row->setStyleSheet(
            "background:#1e293b; border:1px solid #334155;"
            "border-radius:5px;");
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(6);

        // Pulsing green dot (static in Qt — we use a colored label)
        auto *dot = new QLabel("●");
        dot->setStyleSheet("color:#22c55e; font-size:10px;");
        dot->setFixedWidth(14);
        rowLayout->addWidget(dot);

        // Message text
        auto *txt = new QLabel(msg.text);
        txt->setStyleSheet("color:#e2e8f0; font-size:12px;");
        txt->setWordWrap(true);
        txt->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        rowLayout->addWidget(txt, 1);

        // Save to Library button (only for temporary messages)
        if (msg.temporary) {
            auto *saveBtn = new QPushButton("Save");
            saveBtn->setStyleSheet(S_BTN_NEUTRAL);
            saveBtn->setFixedWidth(48);
            saveBtn->setCursor(Qt::PointingHandCursor);
            QString capturedId = msg.id;
            connect(saveBtn, &QPushButton::clicked, this,
                    [this, capturedId]() {
                        m_dock->saveToLibrary(capturedId);
                    });
            rowLayout->addWidget(saveBtn);
        }

        // Remove from queue button
        auto *removeBtn = new QPushButton("✕");
        removeBtn->setStyleSheet(S_BTN_DANGER);
        removeBtn->setFixedWidth(28);
        removeBtn->setCursor(Qt::PointingHandCursor);
        QString capturedId = msg.id;
        connect(removeBtn, &QPushButton::clicked, this,
                [this, capturedId]() {
                    m_dock->toggleMessage(capturedId);
                });
        rowLayout->addWidget(removeBtn);

        m_queueLayout->insertWidget(m_queueLayout->count() - 1, row);
    }

    if (!any) {
        auto *empty = new QLabel("Ticker is empty. Add messages below.");
        empty->setStyleSheet("color:#64748b; font-size:11px; font-style:italic;");
        empty->setAlignment(Qt::AlignCenter);
        m_queueLayout->insertWidget(0, empty);
    }
}

void GoLiveTab::onToggleLive()
{
    m_dock->setLive(!m_dock->settings().isLive);
}

void GoLiveTab::onQuickAdd()
{
    QString text = m_quickInput->text().trimmed();
    if (text.isEmpty()) return;
    m_dock->addMessage(text, /*temporary=*/true);
    m_quickInput->clear();
}

// ═══════════════════════════════════════════════════════════════════════════
// LibraryTab
// ═══════════════════════════════════════════════════════════════════════════

LibraryTab::LibraryTab(TickerDock *dock, QWidget *parent)
    : QWidget(parent), m_dock(dock)
{
    setStyleSheet(S_ROOT);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    // Header
    auto *header = new QLabel("Library");
    header->setStyleSheet("font-size:20px; font-weight:700; color:#e2e8f0;");
    outer->addWidget(header);

    auto *subtitle = new QLabel("Manage the messages that appear in your ticker.");
    subtitle->setStyleSheet("color:#94a3b8; font-size:11px;");
    outer->addWidget(subtitle);

    // Add form
    auto *addRow = new QHBoxLayout;
    m_newInput = new QLineEdit;
    m_newInput->setPlaceholderText("Enter new ticker message...");
    m_newInput->setStyleSheet(S_INPUT);
    connect(m_newInput, &QLineEdit::returnPressed, this, &LibraryTab::onAdd);

    auto *addBtn = new QPushButton("Add");
    addBtn->setStyleSheet(S_BTN_PRIMARY);
    addBtn->setCursor(Qt::PointingHandCursor);
    connect(addBtn, &QPushButton::clicked, this, &LibraryTab::onAdd);

    addRow->addWidget(m_newInput, 1);
    addRow->addWidget(addBtn);
    outer->addLayout(addRow);

    // Scrollable message list
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        "QScrollArea { border:none; background:transparent; }"
        "QScrollBar:vertical { background:#1e293b; width:6px; border-radius:3px; }"
        "QScrollBar::handle:vertical { background:#475569; border-radius:3px; }");

    m_listContainer = new QWidget;
    m_listContainer->setStyleSheet("background:transparent;");
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(6);
    m_listLayout->addStretch();

    scroll->setWidget(m_listContainer);
    outer->addWidget(scroll, 1);

    refresh();
}

void LibraryTab::refresh()
{
    rebuildList();
}

void LibraryTab::rebuildList()
{
    while (m_listLayout->count() > 1)
        delete m_listLayout->takeAt(0)->widget();

    const auto &msgs = m_dock->messages();
    bool any = false;

    for (const auto &msg : msgs) {
        if (msg.temporary) continue;
        any = true;

        bool locked = msg.active; // in queue → lock editing

        // ── Row card ──────────────────────────────────────────────────────
        auto *row = new QWidget;
        row->setStyleSheet(
            locked
                ? "background:#111827; border:1px solid #1e3a5f;"   // dimmed blue tint = in queue
                  "border-radius:6px;"
                : "background:#1e293b; border:1px solid #334155;"
                  "border-radius:6px;");
        auto *outerV = new QVBoxLayout(row);
        outerV->setContentsMargins(10, 8, 10, 8);
        outerV->setSpacing(4);

        // ── Message text ──────────────────────────────────────────────────
        auto *displayLabel = new QLabel(msg.text);
        displayLabel->setStyleSheet(
            locked ? "color:#475569; font-size:13px; background:transparent; font-style:italic;"
                   : "color:#e2e8f0; font-size:13px; background:transparent;");
        displayLabel->setWordWrap(true);
        outerV->addWidget(displayLabel);

        if (locked) {
            // In-queue: no action buttons, styling communicates the lock
        } else {
            // ── Inline edit widgets ───────────────────────────────────────
            auto *editInput = new QLineEdit(msg.text);
            editInput->setStyleSheet(S_INPUT);
            editInput->hide();
            outerV->addWidget(editInput);

            // ── Action buttons ────────────────────────────────────────────
            auto *btnRow   = new QHBoxLayout;
            btnRow->setSpacing(6);
            btnRow->addStretch();

            auto *addQueueBtn = new QPushButton("+ Queue");
            auto *editBtn     = new QPushButton("✎  Edit");
            auto *saveBtn     = new QPushButton("✓  Save");
            auto *cancelBtn   = new QPushButton("Cancel");
            auto *deleteBtn   = new QPushButton("✕  Delete");

            addQueueBtn->setStyleSheet(
                "QPushButton { background:#14532d; color:#4ade80; border:none;"
                "  border-radius:4px; padding:3px 10px; font-size:11px; }"
                "QPushButton:hover { background:#166534; }");
            editBtn->setStyleSheet(
                "QPushButton { background:#1e3a5f; color:#93c5fd; border:none;"
                "  border-radius:4px; padding:3px 10px; font-size:11px; }"
                "QPushButton:hover { background:#1d4ed8; }");
            saveBtn->setStyleSheet(S_BTN_GREEN);
            cancelBtn->setStyleSheet(S_BTN_NEUTRAL);
            deleteBtn->setStyleSheet(
                "QPushButton { background:#3b0d0d; color:#fca5a5; border:none;"
                "  border-radius:4px; padding:3px 10px; font-size:11px; }"
                "QPushButton:hover { background:#7f1d1d; }");

            for (auto *b : {addQueueBtn, editBtn, saveBtn, cancelBtn, deleteBtn}) {
                b->setCursor(Qt::PointingHandCursor);
                b->setFixedHeight(24);
            }

            saveBtn->hide();
            cancelBtn->hide();

            btnRow->addWidget(addQueueBtn);
            btnRow->addWidget(editBtn);
            btnRow->addWidget(saveBtn);
            btnRow->addWidget(cancelBtn);
            btnRow->addWidget(deleteBtn);
            outerV->addLayout(btnRow);

            QString capturedId = msg.id;

            connect(addQueueBtn, &QPushButton::clicked, this,
                    [this, capturedId]() { m_dock->toggleMessage(capturedId); });

            connect(editBtn, &QPushButton::clicked, this,
                    [displayLabel, editInput, editBtn, addQueueBtn,
                     saveBtn, cancelBtn, deleteBtn]() {
                        displayLabel->hide(); editInput->show();
                        editInput->setFocus(); editInput->selectAll();
                        editBtn->hide(); addQueueBtn->hide(); deleteBtn->hide();
                        saveBtn->show(); cancelBtn->show();
                    });

            auto finishEdit = [this, capturedId, displayLabel, editInput,
                               editBtn, addQueueBtn, saveBtn, cancelBtn, deleteBtn]
                              (bool save) {
                if (save) {
                    QString t = editInput->text().trimmed();
                    if (!t.isEmpty()) {
                        m_dock->updateMessage(capturedId, t);
                        displayLabel->setText(t);
                    }
                }
                editInput->hide(); displayLabel->show();
                saveBtn->hide(); cancelBtn->hide();
                editBtn->show(); addQueueBtn->show(); deleteBtn->show();
            };

            connect(editInput, &QLineEdit::returnPressed, this,
                    [finishEdit]() { finishEdit(true); });
            connect(saveBtn,   &QPushButton::clicked, this,
                    [finishEdit]() { finishEdit(true); });
            connect(cancelBtn, &QPushButton::clicked, this,
                    [finishEdit]() { finishEdit(false); });
            connect(deleteBtn, &QPushButton::clicked, this,
                    [this, capturedId]() { m_dock->removeMessage(capturedId); });
        }

        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }

    if (!any) {
        auto *empty = new QLabel("No messages yet. Add one above!");
        empty->setStyleSheet(
            "color:#64748b; font-size:11px; font-style:italic;"
            "background:#1e293b; border:1px dashed #334155; border-radius:6px;"
            "padding:24px;");
        empty->setAlignment(Qt::AlignCenter);
        m_listLayout->insertWidget(0, empty);
    }
}

void LibraryTab::onAdd()
{
    QString text = m_newInput->text().trimmed();
    if (text.isEmpty()) return;
    m_dock->addMessage(text, /*temporary=*/false);
    m_newInput->clear();
}

// ═══════════════════════════════════════════════════════════════════════════
// FormattingTab
// ═══════════════════════════════════════════════════════════════════════════

// Helper: paint a QPushButton to show a solid color swatch
void FormattingTab::paintColorButton(QPushButton *btn, const QColor &c)
{
    QString fg = (c.lightness() > 128) ? "#000000" : "#ffffff";
    btn->setStyleSheet(
        QString("QPushButton { background-color:%1; color:%2;"
                "  border-radius:4px; padding:4px 10px; border:1px solid #475569; }"
                "QPushButton:hover { border-color:#94a3b8; }")
            .arg(c.name(), fg));
}


FormattingTab::FormattingTab(TickerDock *dock, QWidget *parent)
    : QWidget(parent), m_dock(dock)
{
    setStyleSheet(S_ROOT);

    // Outer scrollable area so the tab doesn't clip on small docks
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(
        "QScrollArea { border:none; background:transparent; }"
        "QScrollBar:vertical { background:#1e293b; width:6px; border-radius:3px; }"
        "QScrollBar::handle:vertical { background:#475569; border-radius:3px; }");

    auto *inner = new QWidget;
    inner->setStyleSheet("background:transparent;");
    auto *innerLayout = new QVBoxLayout(inner);
    innerLayout->setContentsMargins(8, 8, 8, 8);
    innerLayout->setSpacing(4);

    // ── SECTION 1: Layout & Colors ────────────────────────────────────────
    auto *layoutSec = new CollapsibleSection("Layout & Colors", true);
    {
        auto *b = layoutSec->body();

        // Height row
        {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *lbl  = new QLabel("Height"); lbl->setStyleSheet(S_LABEL_MUTED); lbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            m_heightSlider = new QSlider(Qt::Horizontal);
            m_heightSlider->setRange(30, 200); m_heightSlider->setValue(m_dock->settings().height);
            m_heightSlider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            m_heightVal = new QLabel(QString::number(m_dock->settings().height) + "px");
            m_heightVal->setStyleSheet("color:#e2e8f0;font-size:11px;"); m_heightVal->setMinimumWidth(36);
            m_heightVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(lbl); rowL->addWidget(m_heightSlider, 1); rowL->addWidget(m_heightVal);
            b->addWidget(rowW);
        }

        // Speed row
        {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *lbl  = new QLabel("Speed"); lbl->setStyleSheet(S_LABEL_MUTED); lbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            m_speedSlider = new QSlider(Qt::Horizontal);
            m_speedSlider->setRange(10, 500); m_speedSlider->setValue(m_dock->settings().speed);
            m_speedSlider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            m_speedVal = new QLabel(QString::number(m_dock->settings().speed));
            m_speedVal->setStyleSheet("color:#e2e8f0;font-size:11px;"); m_speedVal->setMinimumWidth(36);
            m_speedVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(lbl); rowL->addWidget(m_speedSlider, 1); rowL->addWidget(m_speedVal);
            b->addWidget(rowW);
        }

        // Background color
        m_bgColorBtn = new QPushButton("Background");
        m_bgColor = QColor(m_dock->settings().backgroundColor);
        paintColorButton(m_bgColorBtn, m_bgColor);
        m_bgColorBtn->setCursor(Qt::PointingHandCursor);
        b->addLayout(ctrlRow("Background", m_bgColorBtn));

        connect(m_heightSlider, &QSlider::valueChanged, this, &FormattingTab::onHeightChanged);
        connect(m_speedSlider,  &QSlider::valueChanged, this, &FormattingTab::onSpeedChanged);
        connect(m_bgColorBtn,   &QPushButton::clicked,  this, &FormattingTab::onBgColorPick);
    }
    innerLayout->addWidget(layoutSec);

    // ── SECTION 2: Typography ─────────────────────────────────────────────
    auto *typeSec = new CollapsibleSection("Typography");
    {
        auto *b = typeSec->body();

        // Font family — all fonts installed on this machine
        m_fontFamilyCombo = new QComboBox;
        m_fontFamilyCombo->setStyleSheet(S_COMBO);
        m_fontFamilyCombo->setEditable(true);  // allow typing to search
        m_fontFamilyCombo->setInsertPolicy(QComboBox::NoInsert);
        {
            QFontDatabase fdb;
            QStringList families = fdb.families();
            m_fontFamilyCombo->addItems(families);
            int fi = families.indexOf(m_dock->settings().fontFamily);
            m_fontFamilyCombo->setCurrentIndex(fi >= 0 ? fi : 0);
        }
        b->addLayout(ctrlRow("Font", m_fontFamilyCombo));

        // Font style — populated dynamically based on selected family
        m_fontStyleCombo = new QComboBox;
        m_fontStyleCombo->setStyleSheet(S_COMBO);
        {
            QFontDatabase fdb;
            QString family = m_fontFamilyCombo->currentText();
            m_fontStyleCombo->addItems(fdb.styles(family));
            int si = m_fontStyleCombo->findText(m_dock->settings().fontStyleName);
            m_fontStyleCombo->setCurrentIndex(si >= 0 ? si : 0);
        }
        b->addLayout(ctrlRow("Style", m_fontStyleCombo));

        // Font size slider
        {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *lbl  = new QLabel("Size"); lbl->setStyleSheet(S_LABEL_MUTED); lbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            m_fontSizeSlider = new QSlider(Qt::Horizontal);
            m_fontSizeSlider->setRange(8, 200); m_fontSizeSlider->setValue(m_dock->settings().fontSize);
            m_fontSizeSlider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            m_fontSizeVal = new QLabel(QString::number(m_dock->settings().fontSize) + "px");
            m_fontSizeVal->setStyleSheet("color:#e2e8f0;font-size:11px;"); m_fontSizeVal->setMinimumWidth(36);
            m_fontSizeVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(lbl); rowL->addWidget(m_fontSizeSlider, 1); rowL->addWidget(m_fontSizeVal);
            b->addWidget(rowW);
        }

        // Text color
        m_textColorBtn = new QPushButton("Text Color");
        m_textColor = QColor(m_dock->settings().color);
        paintColorButton(m_textColorBtn, m_textColor);
        m_textColorBtn->setCursor(Qt::PointingHandCursor);
        b->addLayout(ctrlRow("Color", m_textColorBtn));

        // Item separator
        m_separatorEdit = new QLineEdit(m_dock->settings().itemSeparator);
        m_separatorEdit->setStyleSheet(S_INPUT);
        b->addLayout(ctrlRow("Separator", m_separatorEdit));

        // Separator width (spacing on each side)
        auto makeSimpleSliderRow = [&](const QString &label, int min, int max,
                                      int val, const QString &suffix,
                                      QSlider *&slider, QLabel *&valLabel) {
            auto *rw = new QWidget; rw->setStyleSheet("background:transparent;");
            auto *rl = new QHBoxLayout(rw); rl->setContentsMargins(0,0,0,0);
            auto *l = new QLabel(label); l->setStyleSheet(S_LABEL_MUTED);
            l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            slider = new QSlider(Qt::Horizontal);
            slider->setRange(min, max); slider->setValue(val);
            slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            slider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            valLabel = new QLabel(QString::number(val) + suffix);
            valLabel->setStyleSheet("color:#e2e8f0;font-size:11px;");
            valLabel->setMinimumWidth(36); valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rl->addWidget(l); rl->addWidget(slider, 1); rl->addWidget(valLabel);
            b->addWidget(rw);
        };

        // When family changes, refresh the style list
        connect(m_fontFamilyCombo, &QComboBox::currentTextChanged, this,
                [this](const QString &family) {
                    QFontDatabase fdb;
                    m_fontStyleCombo->blockSignals(true);
                    m_fontStyleCombo->clear();
                    m_fontStyleCombo->addItems(fdb.styles(family));
                    m_fontStyleCombo->setCurrentIndex(0);
                    m_fontStyleCombo->blockSignals(false);
                    onFontFamilyChanged(m_fontFamilyCombo->currentIndex());
                });
        connect(m_fontStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &FormattingTab::onFontStyleChanged);
        connect(m_fontSizeSlider,  &QSlider::valueChanged,
                this, &FormattingTab::onFontSizeChanged);
        connect(m_textColorBtn,    &QPushButton::clicked,
                this, &FormattingTab::onTextColorPick);
        connect(m_separatorEdit,   &QLineEdit::textChanged,
                this, &FormattingTab::onSeparatorChanged);
    }
    innerLayout->addWidget(typeSec);

    // ── SECTION 3: Scale ──────────────────────────────────────────────────
    // Text Scale X stretches/squishes text width; Scale Y sets text height.
    // Both are independent of the bar height.
    auto *scaleSec = new CollapsibleSection("Text Scale");
    {
        auto *b = scaleSec->body();

        auto *scaleNote = new QLabel("Scale X stretches text width. Scale Y sets text height.");
        scaleNote->setStyleSheet("color:#64748b; font-size:10px;");
        scaleNote->setWordWrap(true);
        b->addWidget(scaleNote);

        // Scale X  (50% – 300%, stored as 0.5 – 3.0)
        {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *lbl  = new QLabel("Width Scale"); lbl->setStyleSheet(S_LABEL_MUTED); lbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            m_scaleXSlider = new QSlider(Qt::Horizontal);
            m_scaleXSlider->setRange(50, 300);
            m_scaleXSlider->setValue((int)(m_dock->settings().fontScaleX * 100.0));
            m_scaleXSlider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            m_scaleXVal = new QLabel(QString::number((int)(m_dock->settings().fontScaleX * 100.0)) + "%");
            m_scaleXVal->setStyleSheet("color:#e2e8f0;font-size:11px;"); m_scaleXVal->setMinimumWidth(36);
            m_scaleXVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(lbl); rowL->addWidget(m_scaleXSlider, 1); rowL->addWidget(m_scaleXVal);
            b->addWidget(rowW);
        }

        // Scale Y
        {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *lbl  = new QLabel("Height Scale"); lbl->setStyleSheet(S_LABEL_MUTED); lbl->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            m_scaleYSlider = new QSlider(Qt::Horizontal);
            m_scaleYSlider->setRange(50, 300);
            m_scaleYSlider->setValue((int)(m_dock->settings().fontScaleY * 100.0));
            m_scaleYSlider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            m_scaleYVal = new QLabel(QString::number((int)(m_dock->settings().fontScaleY * 100.0)) + "%");
            m_scaleYVal->setStyleSheet("color:#e2e8f0;font-size:11px;"); m_scaleYVal->setMinimumWidth(36);
            m_scaleYVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(lbl); rowL->addWidget(m_scaleYSlider, 1); rowL->addWidget(m_scaleYVal);
            b->addWidget(rowW);
        }

        connect(m_scaleXSlider, &QSlider::valueChanged, this, &FormattingTab::onScaleXChanged);
        connect(m_scaleYSlider, &QSlider::valueChanged, this, &FormattingTab::onScaleYChanged);
    }
    innerLayout->addWidget(scaleSec);

    // ── SECTION 3: Clock ──────────────────────────────────────────────────
    auto *clockSec = new CollapsibleSection("Clock", true); // open by default (clock is on)
    {
        auto *b = clockSec->body();

        // Show clock toggle
        auto *clockToggleRow = new QHBoxLayout;
        auto *clockToggleLbl = new QLabel("Show Clock");
        clockToggleLbl->setStyleSheet(S_LABEL_MUTED);
        m_showClockCheck = new QCheckBox;
        m_showClockCheck->setChecked(m_dock->settings().showClock);
        m_showClockCheck->setStyleSheet("QCheckBox::indicator { width:16px; height:16px; }");
        clockToggleRow->addWidget(clockToggleLbl, 1);
        clockToggleRow->addWidget(m_showClockCheck);
        b->addLayout(clockToggleRow);

        m_clockDetails = new QWidget;
        m_clockDetails->setStyleSheet("background:transparent;");
        auto *cdLayout = new QVBoxLayout(m_clockDetails);
        cdLayout->setContentsMargins(0, 4, 0, 0);
        cdLayout->setSpacing(8);

        // Format
        m_clockFormatCombo = new QComboBox;
        m_clockFormatCombo->setStyleSheet(S_COMBO);
        m_clockFormatCombo->addItems({"24 Hour", "12 Hour"});
        m_clockFormatCombo->setCurrentIndex(
            m_dock->settings().clockFormat == "12h" ? 1 : 0);
        cdLayout->addLayout(ctrlRow("Format", m_clockFormatCombo));

        // Clock font size
        // Helper lambda to create clock slider rows
        auto makeSliderRow = [&](QVBoxLayout *layout, const QString &lbl,
                                 QSlider *&slider, QLabel *&val,
                                 int minV, int maxV, int curV,
                                 const QString &suffix) {
            auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
            auto *rowL = new QHBoxLayout(rowW); rowL->setContentsMargins(0,0,0,0);
            auto *l = new QLabel(lbl); l->setStyleSheet(S_LABEL_MUTED);
            l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            slider = new QSlider(Qt::Horizontal);
            slider->setRange(minV, maxV); slider->setValue(curV);
            slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            slider->setStyleSheet(
                "QSlider::groove:horizontal{background:#334155;height:4px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:#3b82f6;width:14px;height:14px;margin:-5px 0;border-radius:7px;}"
                "QSlider::sub-page:horizontal{background:#2563eb;border-radius:2px;}");
            val = new QLabel(QString::number(curV) + suffix);
            val->setStyleSheet("color:#e2e8f0;font-size:11px;");
            val->setMinimumWidth(36); val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowL->addWidget(l); rowL->addWidget(slider, 1); rowL->addWidget(val);
            layout->addWidget(rowW);
        };

        makeSliderRow(cdLayout, "Size", m_clockFontSizeSlider, m_clockFontSizeVal,
                      8, 150, m_dock->settings().clockFontSize, "px");
        makeSliderRow(cdLayout, "W Scale", m_clockScaleXSlider, m_clockScaleXVal,
                      50, 300, (int)(m_dock->settings().clockScaleX * 100.0), "%");
        makeSliderRow(cdLayout, "H Scale", m_clockScaleYSlider, m_clockScaleYVal,
                      50, 300, (int)(m_dock->settings().clockScaleY * 100.0), "%");

        // Clock separator color
        m_clockSepColorBtn = new QPushButton("Divider Color");
        m_clockSepColor = QColor(m_dock->settings().clockSepColor);
        paintColorButton(m_clockSepColorBtn, m_clockSepColor);
        m_clockSepColorBtn->setCursor(Qt::PointingHandCursor);
        cdLayout->addLayout(ctrlRow("Divider", m_clockSepColorBtn));

        m_clockDetails->setVisible(m_dock->settings().showClock);
        b->addWidget(m_clockDetails);

        connect(m_showClockCheck,      &QCheckBox::toggled,
                this, &FormattingTab::onShowClockToggled);
        connect(m_clockFormatCombo,    QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &FormattingTab::onClockFormatChanged);
        connect(m_clockFontSizeSlider, &QSlider::valueChanged,
                this, &FormattingTab::onClockFontSizeChanged);
        connect(m_clockScaleXSlider,   &QSlider::valueChanged,
                this, &FormattingTab::onClockScaleXChanged);
        connect(m_clockScaleYSlider,   &QSlider::valueChanged,
                this, &FormattingTab::onClockScaleYChanged);
        connect(m_clockSepColorBtn, &QPushButton::clicked,
                this, &FormattingTab::onClockSepColorPick);
    }
    innerLayout->addWidget(clockSec);

    // ── SECTION 4: Presets ────────────────────────────────────────────────
    auto *presetSec = new CollapsibleSection("Presets");
    {
        auto *b = presetSec->body();

        // Save row
        auto *saveRow = new QHBoxLayout;
        m_presetNameEdit = new QLineEdit;
        m_presetNameEdit->setPlaceholderText("New Preset Name...");
        m_presetNameEdit->setStyleSheet(S_INPUT);
        auto *saveBtn = new QPushButton("Save");
        saveBtn->setStyleSheet(S_BTN_GREEN);
        saveBtn->setCursor(Qt::PointingHandCursor);
        connect(saveBtn, &QPushButton::clicked, this, &FormattingTab::onSavePreset);
        connect(m_presetNameEdit, &QLineEdit::returnPressed,
                this, &FormattingTab::onSavePreset);
        saveRow->addWidget(m_presetNameEdit, 1);
        saveRow->addWidget(saveBtn);
        b->addLayout(saveRow);

        // Preset list container
        m_presetsContainer = new QWidget;
        m_presetsContainer->setStyleSheet("background:transparent;");
        m_presetsLayout = new QVBoxLayout(m_presetsContainer);
        m_presetsLayout->setContentsMargins(0, 4, 0, 0);
        m_presetsLayout->setSpacing(4);
        b->addWidget(m_presetsContainer);

        refreshPresets();
    }
    innerLayout->addWidget(presetSec);
    innerLayout->addStretch();

    scroll->setWidget(inner);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(scroll);
}

// ── Formatting slots ─────────────────────────────────────────────────────────

void FormattingTab::onHeightChanged(int v)
{
    m_heightVal->setText(QString::number(v) + "px");
    auto s = m_dock->settings(); s.height = v;
    m_dock->applySettings(s);
}

void FormattingTab::onSpeedChanged(int v)
{
    m_speedVal->setText(QString::number(v));
    auto s = m_dock->settings(); s.speed = v;
    m_dock->applySettings(s);
}

void FormattingTab::onBgColorPick()
{
    QColor c = QColorDialog::getColor(m_bgColor, this, "Background Color");
    if (!c.isValid()) return;
    m_bgColor = c;
    paintColorButton(m_bgColorBtn, c);
    auto s = m_dock->settings(); s.backgroundColor = c.name();
    m_dock->applySettings(s);
}

void FormattingTab::onFontFamilyChanged(int)
{
    QString family = m_fontFamilyCombo->currentText();
    if (family.isEmpty()) return;
    auto s = m_dock->settings(); s.fontFamily = family;
    // When family changes the style combo was already refreshed by the lambda;
    // pick up whatever it landed on.
    s.fontStyleName = m_fontStyleCombo->currentText();
    m_dock->applySettings(s);
}

void FormattingTab::onFontSizeChanged(int v)
{
    m_fontSizeVal->setText(QString::number(v) + "px");
    auto s = m_dock->settings(); s.fontSize = v;
    m_dock->applySettings(s);
}

void FormattingTab::onTextColorPick()
{
    QColor c = QColorDialog::getColor(m_textColor, this, "Text Color");
    if (!c.isValid()) return;
    m_textColor = c;
    paintColorButton(m_textColorBtn, c);
    auto s = m_dock->settings(); s.color = c.name();
    m_dock->applySettings(s);
}

void FormattingTab::onFontStyleChanged(int)
{
    QString style = m_fontStyleCombo->currentText();
    if (style.isEmpty()) return;
    auto s = m_dock->settings(); s.fontStyleName = style;
    m_dock->applySettings(s);
}

void FormattingTab::onScaleXChanged(int v)
{
    m_scaleXVal->setText(QString::number(v) + "%");
    auto s = m_dock->settings(); s.fontScaleX = v / 100.0;
    m_dock->applySettings(s);
}

void FormattingTab::onScaleYChanged(int v)
{
    m_scaleYVal->setText(QString::number(v) + "%");
    auto s = m_dock->settings(); s.fontScaleY = v / 100.0;
    m_dock->applySettings(s);
}

void FormattingTab::onSeparatorChanged(const QString &sep)
{
    auto s = m_dock->settings(); s.itemSeparator = sep;
    m_dock->applySettings(s);
}

void FormattingTab::onShowClockToggled(bool on)
{
    m_clockDetails->setVisible(on);
    auto s = m_dock->settings(); s.showClock = on;
    m_dock->applySettings(s);
}

void FormattingTab::onClockFormatChanged(int idx)
{
    auto s = m_dock->settings();
    s.clockFormat = (idx == 1) ? "12h" : "24h";
    m_dock->applySettings(s);
}

void FormattingTab::onClockFontSizeChanged(int v)
{
    m_clockFontSizeVal->setText(QString::number(v) + "px");
    auto s = m_dock->settings(); s.clockFontSize = v;
    m_dock->applySettings(s);
}

void FormattingTab::onClockScaleXChanged(int v)
{
    m_clockScaleXVal->setText(QString::number(v) + "%");
    auto s = m_dock->settings(); s.clockScaleX = v / 100.0;
    m_dock->applySettings(s);
}

void FormattingTab::onClockScaleYChanged(int v)
{
    m_clockScaleYVal->setText(QString::number(v) + "%");
    auto s = m_dock->settings(); s.clockScaleY = v / 100.0;
    m_dock->applySettings(s);
}

void FormattingTab::onClockSepColorPick()
{
    QColor c = QColorDialog::getColor(m_clockSepColor, this, "Clock Divider Color");
    if (!c.isValid()) return;
    m_clockSepColor = c;
    paintColorButton(m_clockSepColorBtn, c);
    auto s = m_dock->settings(); s.clockSepColor = c.name();
    m_dock->applySettings(s);
}


void FormattingTab::onSavePreset()
{
    QString name = m_presetNameEdit->text().trimmed();
    if (name.isEmpty()) return;
    m_dock->savePreset(name);
    m_presetNameEdit->clear();
}

void FormattingTab::refreshPresets()
{
    while (m_presetsLayout->count() > 0)
        delete m_presetsLayout->takeAt(0)->widget();

    const auto &presets = m_dock->presets();

    if (presets.isEmpty()) {
        auto *empty = new QLabel("No saved presets.");
        empty->setStyleSheet("color:#64748b; font-size:11px; font-style:italic;");
        empty->setAlignment(Qt::AlignCenter);
        m_presetsLayout->addWidget(empty);
        return;
    }

    for (const auto &preset : presets) {
        auto *row = new QWidget;
        row->setStyleSheet(
            "background:#1e293b; border:1px solid #334155; border-radius:4px;");
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(6);

        auto *nameLabel = new QLabel(preset.name);
        nameLabel->setStyleSheet("color:#e2e8f0; font-size:12px; font-weight:500;");
        rowLayout->addWidget(nameLabel, 1);

        auto *loadBtn = new QPushButton("Load");
        loadBtn->setStyleSheet(
            "QPushButton { background:#1e3a5f; color:#93c5fd; border-radius:4px;"
            "  padding:3px 10px; border:none; font-size:11px; }"
            "QPushButton:hover { background:#1d4ed8; }");
        loadBtn->setCursor(Qt::PointingHandCursor);
        QString capturedId = preset.id;
        connect(loadBtn, &QPushButton::clicked, this,
                [this, capturedId]() { m_dock->loadPreset(capturedId); });

        auto *delBtn = new QPushButton("✕");
        delBtn->setStyleSheet(S_BTN_DANGER);
        delBtn->setFixedSize(24, 24);
        delBtn->setCursor(Qt::PointingHandCursor);
        connect(delBtn, &QPushButton::clicked, this,
                [this, capturedId]() { m_dock->deletePreset(capturedId); });

        rowLayout->addWidget(loadBtn);
        rowLayout->addWidget(delBtn);
        m_presetsLayout->addWidget(row);
    }
}

void FormattingTab::loadFromSettings()
{
    const auto &s = m_dock->settings();

    // Layout
    m_heightSlider->setValue(s.height);
    m_speedSlider->setValue(s.speed);
    m_bgColor = QColor(s.backgroundColor);
    paintColorButton(m_bgColorBtn, m_bgColor);

    // Typography — font family
    {
        int fi = m_fontFamilyCombo->findText(s.fontFamily);
        m_fontFamilyCombo->blockSignals(true);
        m_fontFamilyCombo->setCurrentIndex(fi >= 0 ? fi : 0);
        m_fontFamilyCombo->blockSignals(false);
    }
    // Refresh style list for this family, then select saved style
    {
        QFontDatabase fdb;
        m_fontStyleCombo->blockSignals(true);
        m_fontStyleCombo->clear();
        m_fontStyleCombo->addItems(fdb.styles(m_fontFamilyCombo->currentText()));
        int si = m_fontStyleCombo->findText(s.fontStyleName);
        m_fontStyleCombo->setCurrentIndex(si >= 0 ? si : 0);
        m_fontStyleCombo->blockSignals(false);
    }

    m_fontSizeSlider->setValue(s.fontSize);
    m_textColor = QColor(s.color);
    paintColorButton(m_textColorBtn, m_textColor);

    m_separatorEdit->blockSignals(true);
    m_separatorEdit->setText(s.itemSeparator);
    m_separatorEdit->blockSignals(false);

    // Scale
    m_scaleXSlider->setValue((int)(s.fontScaleX * 100.0));
    m_scaleYSlider->setValue((int)(s.fontScaleY * 100.0));

    // Clock
    m_showClockCheck->setChecked(s.showClock);
    m_clockDetails->setVisible(s.showClock);
    m_clockFormatCombo->setCurrentIndex(s.clockFormat == "12h" ? 1 : 0);
    m_clockFontSizeSlider->setValue(s.clockFontSize);
    m_clockScaleXSlider->setValue((int)(s.clockScaleX * 100.0));
    m_clockScaleYSlider->setValue((int)(s.clockScaleY * 100.0));
    m_clockSepColor = QColor(s.clockSepColor);
    paintColorButton(m_clockSepColorBtn, m_clockSepColor);
}