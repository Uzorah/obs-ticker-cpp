#pragma once

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <obs-frontend-api.h>
#include <obs.h>

struct TickerMessage {
    QString id;
    QString text;
    bool    active    = true;
    bool    temporary = false;
};

struct TickerSettings {
    // Layout & Colors
    int     height          = 70;
    int     speed           = 50;
    QString backgroundColor = "#000000";

    // Typography
    QString fontFamily    = "Arial";
    QString fontStyleName = "Regular";
    int     fontSize      = 24;
    QString color         = "#ffffff";
    QString itemSeparator = "•";  // Just the bullet, spaces added automatically

    // Text scale
    double fontScaleX = 1.0;
    double fontScaleY = 1.0;

    // Clock
    bool    showClock       = true;
    QString clockFormat     = "24h";
    int     clockFontSize   = 24;
    double  clockScaleX     = 1.0;
    double  clockScaleY     = 1.0;
    int     clockZoneWidth  = 170;
    QString clockSepColor   = "#ffffff";

    // Live state
    bool isLive = false;
};

struct TickerPreset {
    QString        id;
    QString        name;
    TickerSettings settings;
};

obs_source_t *ticker_find_source();
void          ticker_push_settings(const TickerSettings &s,
                                   const QString        &messagesStr);

class CollapsibleSection : public QWidget
{
    Q_OBJECT
public:
    explicit CollapsibleSection(const QString &title,
                                bool           startOpen = false,
                                QWidget       *parent    = nullptr);
    QVBoxLayout *body() { return m_bodyLayout; }

private slots:
    void toggle();

private:
    QPushButton *m_header;
    QWidget     *m_body;
    QVBoxLayout *m_bodyLayout;
    bool         m_open;
};

class TickerDock;

class GoLiveTab : public QWidget
{
    Q_OBJECT
public:
    explicit GoLiveTab(TickerDock *dock, QWidget *parent = nullptr);
    void refresh();

private slots:
    void onToggleLive();
    void onQuickAdd();

private:
    void rebuildQueue();

    TickerDock  *m_dock;
    QTimer      *m_pollTimer;
    QPushButton *m_liveBtn;
    QLabel      *m_statusLabel;
    QWidget     *m_queueContainer;
    QVBoxLayout *m_queueLayout;
    QLineEdit   *m_quickInput;
};

class LibraryTab : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryTab(TickerDock *dock, QWidget *parent = nullptr);
    void refresh();

private slots:
    void onAdd();

private:
    void rebuildList();

    TickerDock  *m_dock;
    QWidget     *m_listContainer;
    QVBoxLayout *m_listLayout;
    QLineEdit   *m_newInput;
};

class FormattingTab : public QWidget
{
    Q_OBJECT
public:
    explicit FormattingTab(TickerDock *dock, QWidget *parent = nullptr);
    void loadFromSettings();
    void refreshPresets();

private slots:
    void onHeightChanged(int v);
    void onSpeedChanged(int v);
    void onBgColorPick();

    void onFontFamilyChanged(int idx);
    void onFontStyleChanged(int idx);
    void onFontSizeChanged(int v);
    void onTextColorPick();
    void onSeparatorChanged(const QString &s);
    void onScaleXChanged(int v);
    void onScaleYChanged(int v);

    void onShowClockToggled(bool on);
    void onClockFormatChanged(int idx);
    void onClockFontSizeChanged(int v);
    void onClockScaleXChanged(int v);
    void onClockScaleYChanged(int v);
    void onClockSepColorPick();

    void onSavePreset();

private:
    static void paintColorButton(QPushButton *btn, const QColor &c);

    TickerDock *m_dock;

    // Layout & Colors
    QSlider     *m_heightSlider;
    QLabel      *m_heightVal;
    QSlider     *m_speedSlider;
    QLabel      *m_speedVal;
    QPushButton *m_bgColorBtn;
    QColor       m_bgColor{Qt::black};

    // Typography
    QComboBox   *m_fontFamilyCombo;
    QComboBox   *m_fontStyleCombo;
    QSlider     *m_fontSizeSlider;
    QLabel      *m_fontSizeVal;
    QPushButton *m_textColorBtn;
    QColor       m_textColor{Qt::white};
    QLineEdit   *m_separatorEdit;

    // Scale
    QSlider     *m_scaleXSlider;
    QLabel      *m_scaleXVal;
    QSlider     *m_scaleYSlider;
    QLabel      *m_scaleYVal;

    // Clock
    QCheckBox   *m_showClockCheck;
    QWidget     *m_clockDetails;
    QComboBox   *m_clockFormatCombo;
    QSlider     *m_clockFontSizeSlider;
    QLabel      *m_clockFontSizeVal;
    QSlider     *m_clockScaleXSlider;
    QLabel      *m_clockScaleXVal;
    QSlider     *m_clockScaleYSlider;
    QLabel      *m_clockScaleYVal;
    QPushButton *m_clockSepColorBtn;
    QColor       m_clockSepColor{Qt::white};

    // Presets
    QLineEdit   *m_presetNameEdit;
    QWidget     *m_presetsContainer;
    QVBoxLayout *m_presetsLayout;
};

class TickerDock : public QWidget
{
    Q_OBJECT
public:
    explicit TickerDock(QWidget *parent = nullptr);
    ~TickerDock();

    static TickerDock *instance();

    const QVector<TickerMessage> &messages() const { return m_messages; }
    const TickerSettings         &settings() const { return m_settings; }
    const QVector<TickerPreset>  &presets()  const { return m_presets;  }

    void addMessage   (const QString &text, bool temporary = false);
    void removeMessage(const QString &id);
    void toggleMessage(const QString &id);
    void updateMessage(const QString &id, const QString &newText);
    void saveToLibrary(const QString &id);

    void applySettings(const TickerSettings &s);
    void setLive      (bool live);

    void savePreset  (const QString &name);
    void loadPreset  (const QString &id);
    void deletePreset(const QString &id);

    void pushToSource();

signals:
    void messagesChanged();
    void settingsChanged();
    void presetsChanged();

private:
    static TickerDock *s_instance;

    QTabWidget    *m_tabs;
    GoLiveTab     *m_goLiveTab;
    LibraryTab    *m_libraryTab;
    FormattingTab *m_formattingTab;

    QVector<TickerMessage> m_messages;
    TickerSettings          m_settings;
    QVector<TickerPreset>   m_presets;
};