#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QProcess>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QDebug>
#include <QGridLayout>
#include <QKeyEvent>
#include <QStyle>
#include <QToolButton>
#include <QtGlobal>
#include <cstdio>
#include <functional>

namespace {

struct ExternalAction {
    QString label;
    QString command;
    QStringList args;
    QString icon;
};

struct MenuAction {
    QString label;
    std::function<void()> handler;
    bool enabled = true;
    QString icon;
};

struct AppSettings {
    bool pollEnabled = false;
    int pollIntervalMs = 1500;
    bool wlPasteEnabled = false;
    QString wlPasteMode = "primary";
    int actionIconsPerRow = 10;
    QString logLevel = "info";
};

QString normalizeWhitespace(const QString &text) {
    QString result = text;
    result.replace(QRegularExpression("\\s+"), " ");
    return result.trimmed();
}

QString toTitleCase(const QString &text) {
    QStringList parts = normalizeWhitespace(text).split(' ', Qt::SkipEmptyParts);
    for (QString &part : parts) {
        if (!part.isEmpty()) {
            part[0] = part[0].toUpper();
            if (part.size() > 1) {
                part = part[0] + part.mid(1).toLower();
            }
        }
    }
    return parts.join(' ');
}

QList<ExternalAction> loadExternalActions() {
    QList<ExternalAction> actions;
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configPath = configDir + "/codexpopclip/actions.json";

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qInfo() << "No external actions config at" << configPath;
        return actions;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        qWarning() << "Invalid actions.json (not an object).";
        return actions;
    }

    const QJsonArray list = doc.object().value("actions").toArray();
    for (const QJsonValue &entry : list) {
        const QJsonObject obj = entry.toObject();
        const QString label = obj.value("label").toString();
        const QString command = obj.value("command").toString();
        if (label.isEmpty() || command.isEmpty()) {
            qWarning() << "Skipping action with missing label or command.";
            continue;
        }
        ExternalAction action;
        action.label = label;
        action.command = command;
        for (const QJsonValue &arg : obj.value("args").toArray()) {
            action.args.append(arg.toString());
        }
        action.icon = obj.value("icon").toString();
        actions.append(action);
    }

    qInfo() << "Loaded external actions:" << actions.size();

    return actions;
}

QStringList expandArgs(const QStringList &args, const QString &text) {
    QStringList expanded;
    expanded.reserve(args.size());
    for (const QString &arg : args) {
        QString out = arg;
        out.replace("{text}", text);
        expanded.append(out);
    }
    return expanded;
}

QString previewText(const QString &text) {
    QString preview = text;
    preview.replace("\n", "\\n");
    preview.replace("\r", "\\r");
    if (preview.size() > 80) {
        return preview.left(80) + "...";
    }
    return preview;
}

AppSettings loadSettings() {
    AppSettings settings;
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configPath = configDir + "/codexpopclip/settings.json";

    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qInfo() << "No settings config at" << configPath;
        return settings;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        qWarning() << "Invalid settings.json (not an object).";
        return settings;
    }

    const QJsonObject obj = doc.object();
    if (obj.contains("poll")) {
        settings.pollEnabled = obj.value("poll").toBool(settings.pollEnabled);
    }
    if (obj.contains("poll_ms")) {
        const int value = obj.value("poll_ms").toInt(settings.pollIntervalMs);
        if (value > 0) {
            settings.pollIntervalMs = value;
        }
    }
    if (obj.contains("wlpaste")) {
        settings.wlPasteEnabled = obj.value("wlpaste").toBool(settings.wlPasteEnabled);
    }
    if (obj.contains("wlpaste_mode")) {
        const QString mode = obj.value("wlpaste_mode").toString(settings.wlPasteMode);
        if (!mode.isEmpty()) {
            settings.wlPasteMode = mode;
        }
    }
    if (obj.contains("icons_per_row")) {
        const QJsonValue value = obj.value("icons_per_row");
        int parsed = settings.actionIconsPerRow;
        if (value.isString()) {
            bool ok = false;
            parsed = value.toString().toInt(&ok);
            if (!ok) {
                parsed = settings.actionIconsPerRow;
            }
        } else {
            parsed = value.toInt(settings.actionIconsPerRow);
        }
        if (parsed > 0) {
            settings.actionIconsPerRow = parsed;
        }
    }
    if (obj.contains("log_level")) {
        const QJsonValue value = obj.value("log_level");
        const QString level = value.toString(settings.logLevel).toLower();
        if (!level.isEmpty()) {
            settings.logLevel = level;
        }
    }

    qInfo() << "Loaded settings from" << configPath;
    return settings;
}

QString readWlPaste(const QStringList &args, int timeoutMs, bool *ok) {
    QProcess proc;
    proc.start("wl-paste", args);
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        if (ok) {
            *ok = false;
        }
        return QString();
    }

    if (ok) {
        *ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
    }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

int logLevelFromString(const QString &level) {
    const QString normalized = level.trimmed().toLower();
    if (normalized == "debug") {
        return 0;
    }
    if (normalized == "warning" || normalized == "warn") {
        return 2;
    }
    if (normalized == "critical" || normalized == "error") {
        return 3;
    }
    if (normalized == "fatal") {
        return 4;
    }
    return 1;
}

int gMinLogLevel = 1;
QtMessageHandler gPrevLogHandler = nullptr;

int logSeverity(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return 0;
        case QtInfoMsg:
            return 1;
        case QtWarningMsg:
            return 2;
        case QtCriticalMsg:
            return 3;
        case QtFatalMsg:
            return 4;
    }
    return 1;
}

void logHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (logSeverity(type) < gMinLogLevel) {
        return;
    }
    if (gPrevLogHandler) {
        gPrevLogHandler(type, context, message);
        return;
    }
    const QByteArray bytes = message.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
}

class PopupController : public QObject {
    Q_OBJECT

public:
    explicit PopupController(const AppSettings &settings, QObject *parent = nullptr)
        : QObject(parent) {
        clipboard_ = QGuiApplication::clipboard();
        qInfo() << "Clipboard available:" << (clipboard_ != nullptr);
        qInfo() << "Supports selection:" << clipboard_->supportsSelection();
        qInfo() << "Supports find buffer:" << clipboard_->supportsFindBuffer();
        connect(clipboard_, &QClipboard::dataChanged, this, &PopupController::onClipboardChanged);
        connect(clipboard_, &QClipboard::selectionChanged, this, &PopupController::onSelectionChanged);

        debounce_.setSingleShot(true);
        debounce_.setInterval(120);
        connect(&debounce_, &QTimer::timeout, this, &PopupController::showMenuIfNeeded);

        AppSettings effective = settings;
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_POLL")) {
            effective.pollEnabled = true;
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_POLL_MS")) {
            const int value = qEnvironmentVariableIntValue("CODEXPOPCLIP_POLL_MS");
            if (value > 0) {
                effective.pollIntervalMs = value;
            }
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_WLPASTE")) {
            effective.wlPasteEnabled = true;
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_WLPASTE_MODE")) {
            const QString mode = qEnvironmentVariable("CODEXPOPCLIP_WLPASTE_MODE", effective.wlPasteMode);
            if (!mode.isEmpty()) {
                effective.wlPasteMode = mode;
            }
        }

        pollEnabled_ = effective.pollEnabled;
        pollIntervalMs_ = effective.pollIntervalMs;
        wlPasteEnabled_ = effective.wlPasteEnabled;
        wlPasteMode_ = effective.wlPasteMode;
        popup_.setActionIconsPerRow(effective.actionIconsPerRow);
        traceEnabled_ = qEnvironmentVariableIsSet("CODEXPOPCLIP_TRACE");
        if (pollEnabled_) {
            pollTimer_.setInterval(pollIntervalMs_);
            connect(&pollTimer_, &QTimer::timeout, this, &PopupController::pollClipboard);
            qInfo() << "Polling enabled" << "interval_ms=" << pollIntervalMs_;
        }

        popup_.setOnClosed([this]() {
            popupVisible_ = false;
            nextAllowedPopupMs_ = popupTimer_.elapsed() + 800;
            if (pollEnabled_) {
                QTimer::singleShot(300, this, [this]() {
                    if (pollEnabled_ && !popupVisible_) {
                        pollTimer_.start();
                    }
                });
            }
        });
        popupTimer_.start();
        if (wlPasteEnabled_) {
            qInfo() << "wl-paste fallback enabled";
            qInfo() << "wl-paste mode:" << wlPasteMode_;
        }

        // Delay first poll to avoid immediate popup on app start.
        if (pollEnabled_) {
            QTimer::singleShot(pollIntervalMs_, this, [this]() {
                if (pollEnabled_ && !popupVisible_) {
                    pollTimer_.start();
                }
            });
        }
    }

private slots:
    void onClipboardChanged() {
        if (suppressNext_) {
            qInfo() << "Clipboard change suppressed.";
            suppressNext_ = false;
            return;
        }
        qInfo() << "Clipboard changed.";
        pendingMode_ = QClipboard::Clipboard;
        debounce_.start();
    }

    void onSelectionChanged() {
        if (suppressNext_) {
            qInfo() << "Selection change suppressed.";
            suppressNext_ = false;
            return;
        }
        qInfo() << "Selection changed.";
        pendingMode_ = QClipboard::Selection;
        debounce_.start();
    }

    void showMenuIfNeeded() {
        const QString text = clipboard_->text(pendingMode_).trimmed();
        qInfo() << "Evaluating text from mode" << pendingMode_
                << "len=" << text.size() << "preview=" << previewText(text);
        showMenuIfNeededWithText(text);
    }

    void showMenuIfNeededWithText(const QString &text) {
        if (text.isEmpty()) {
            qInfo() << "No text to act on.";
            return;
        }
        lastText_ = text;
        showMenu(text);
    }

private:
    class ActionPopup : public QWidget {
    public:
        explicit ActionPopup(QWidget *parent = nullptr)
            : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint) {
            setObjectName("ActionPopup");
            setAttribute(Qt::WA_ShowWithoutActivating, false);
            setFocusPolicy(Qt::StrongFocus);

            grid_ = new QGridLayout(this);
            grid_->setContentsMargins(6, 6, 6, 6);
            grid_->setSpacing(4);
            setLayout(grid_);
        }

        void setOnClosed(std::function<void()> handler) {
            onClosed_ = std::move(handler);
        }

        void setActionIconsPerRow(int count) {
            actionIconsPerRow_ = qMax(1, count);
        }

        void setContent(const QString &selectedText, const QList<MenuAction> &actions) {
            Q_UNUSED(selectedText);
            actions_ = actions;
            visibleActions_.clear();
            for (const MenuAction &action : actions_) {
                if (action.enabled) {
                    visibleActions_.append(action);
                }
            }
            currentPage_ = 0;
            rebuildGrid();
        }

        void showAtCursor() {
            const QPoint pos = QCursor::pos();
            const QScreen *screen = QGuiApplication::screenAt(pos);
            const QRect geom = screen ? screen->availableGeometry() : QGuiApplication::primaryScreen()->availableGeometry();
            QSize size = sizeHint();
            const int x = qBound(geom.left(), pos.x(), geom.right() - size.width());
            const int y = qBound(geom.top(), pos.y(), geom.bottom() - size.height());
            move(QPoint(x, y));
            showTimer_.start();
            show();
            raise();
            activateWindow();
            setFocus();
        }

    protected:
        void focusOutEvent(QFocusEvent *event) override {
            QWidget::focusOutEvent(event);
            if (showTimer_.isValid() && showTimer_.elapsed() < 200) {
                return;
            }
            if (underMouse()) {
                return;
            }
            hide();
        }

        void hideEvent(QHideEvent *event) override {
            QWidget::hideEvent(event);
            if (onClosed_) {
                onClosed_();
            }
        }

        void keyPressEvent(QKeyEvent *event) override {
            if (event->key() == Qt::Key_Escape) {
                hide();
                return;
            }
            QWidget::keyPressEvent(event);
        }

    private:
        void rebuildGrid() {
            while (QLayoutItem *item = grid_->takeAt(0)) {
                if (item->widget()) {
                    item->widget()->deleteLater();
                }
                delete item;
            }

            const int totalActions = visibleActions_.size();
            const bool needsPaging = totalActions > actionIconsPerRow_;
            const int slotsPerPage = qMax(1, actionIconsPerRow_);
            const int totalPages = needsPaging ? ((totalActions + slotsPerPage - 1) / slotsPerPage) : 1;
            currentPage_ = qBound(0, currentPage_, totalPages - 1);

            const int totalColumns = actionIconsPerRow_ + (needsPaging ? 2 : 0);

            const int startIndex = currentPage_ * slotsPerPage;
            const int endIndex = qMin(totalActions, startIndex + slotsPerPage);
            int actionOffset = 0;

            for (int col = 0; col < totalColumns; ++col) {
                QWidget *widget = nullptr;
                if (startIndex + actionOffset < endIndex) {
                    const int actionIndex = startIndex + actionOffset;
                    widget = createActionButton(visibleActions_[actionIndex], actionIndex);
                    actionOffset++;
                } else if (needsPaging && col == totalColumns - 2) {
                    widget = createNavButton(QStyle::SP_ArrowBack, "Previous actions", currentPage_ > 0, -1);
                } else if (needsPaging && col == totalColumns - 1) {
                    widget = createNavButton(QStyle::SP_ArrowForward, "Next actions", currentPage_ + 1 < totalPages, 1);
                } else {
                    widget = createSpacer();
                }
                grid_->addWidget(widget, 0, col);
            }

            adjustSize();
        }

        QWidget *createSpacer() {
            auto *spacer = new QWidget(this);
            spacer->setFixedSize(buttonSize_, buttonSize_);
            return spacer;
        }

        QToolButton *createActionButton(const MenuAction &action, int index) {
            auto *button = new QToolButton(this);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setAutoRaise(true);
            button->setIcon(iconForAction(action));
            button->setToolTip(action.label);
            button->setFixedSize(buttonSize_, buttonSize_);
            button->setIconSize(QSize(iconSize_, iconSize_));
            connect(button, &QToolButton::clicked, this, [this, index]() {
                if (index < 0 || index >= visibleActions_.size()) {
                    return;
                }
                const MenuAction action = visibleActions_[index];
                qInfo() << "Menu choice:" << action.label;
                if (action.handler) {
                    action.handler();
                }
                hide();
            });
            return button;
        }

        QToolButton *createNavButton(QStyle::StandardPixmap icon, const QString &tooltip, bool enabled, int delta) {
            auto *button = new QToolButton(this);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setAutoRaise(true);
            button->setIcon(style()->standardIcon(icon));
            button->setToolTip(tooltip);
            button->setEnabled(enabled);
            button->setFixedSize(buttonSize_, buttonSize_);
            button->setIconSize(QSize(iconSize_, iconSize_));
            connect(button, &QToolButton::clicked, this, [this, delta]() {
                currentPage_ += delta;
                rebuildGrid();
            });
            return button;
        }

        QIcon iconForAction(const MenuAction &action) const {
            QIcon icon = iconFromSpec(action.icon);
            if (!icon.isNull()) {
                return icon;
            }
            const QString key = action.label.toLower();
            if (key.contains("uppercase")) {
                return iconFromSpec("sp:SP_ArrowUp");
            }
            if (key.contains("lowercase")) {
                return iconFromSpec("sp:SP_ArrowDown");
            }
            if (key.contains("title")) {
                return iconFromSpec("sp:SP_FileDialogDetailedView");
            }
            if (key.contains("normalize")) {
                return iconFromSpec("sp:SP_BrowserReload");
            }
            if (key.contains("paste")) {
                return iconFromSpec("sp:SP_DialogResetButton");
            }
            if (key.contains("copy")) {
                return iconFromSpec("sp:SP_DialogOpenButton");
            }
            return iconFromSpec("sp:SP_FileIcon");
        }

        QIcon iconFromSpec(const QString &spec) const {
            if (spec.isEmpty()) {
                return QIcon();
            }
            if (spec.startsWith("sp:")) {
                const QString name = spec.mid(3);
                const QStyle::StandardPixmap pixmap = standardPixmapFromName(name);
                if (pixmap != QStyle::SP_CustomBase) {
                    return style()->standardIcon(pixmap);
                }
                return QIcon();
            }
            const QString filePath = resolveIconPath(spec);
            if (!filePath.isEmpty()) {
                return QIcon(filePath);
            }
            return QIcon::fromTheme(spec);
        }

        QString resolveIconPath(const QString &spec) const {
            if (spec.startsWith("file:")) {
                const QString localPath = QUrl(spec).toLocalFile();
                if (QFileInfo::exists(localPath)) {
                    return localPath;
                }
                return QString();
            }
            QString path = spec;
            if (spec.startsWith("~")) {
                path = QDir::homePath() + spec.mid(1);
            }
            if (QDir::isAbsolutePath(path) && QFileInfo::exists(path)) {
                return path;
            }
            return QString();
        }

        QStyle::StandardPixmap standardPixmapFromName(const QString &name) const {
            if (name == "SP_ArrowUp") {
                return QStyle::SP_ArrowUp;
            }
            if (name == "SP_ArrowDown") {
                return QStyle::SP_ArrowDown;
            }
            if (name == "SP_ArrowBack") {
                return QStyle::SP_ArrowBack;
            }
            if (name == "SP_ArrowForward") {
                return QStyle::SP_ArrowForward;
            }
            if (name == "SP_FileDialogDetailedView") {
                return QStyle::SP_FileDialogDetailedView;
            }
            if (name == "SP_BrowserReload") {
                return QStyle::SP_BrowserReload;
            }
            if (name == "SP_DialogResetButton") {
                return QStyle::SP_DialogResetButton;
            }
            if (name == "SP_DialogOpenButton") {
                return QStyle::SP_DialogOpenButton;
            }
            if (name == "SP_FileIcon") {
                return QStyle::SP_FileIcon;
            }
            return QStyle::SP_CustomBase;
        }

    private:
        QGridLayout *grid_ = nullptr;
        QList<MenuAction> actions_;
        QList<MenuAction> visibleActions_;
        std::function<void()> onClosed_;
        QElapsedTimer showTimer_;
        int currentPage_ = 0;
        int actionIconsPerRow_ = 10;
        int buttonSize_ = 30;
        int iconSize_ = 20;
    };

    void showMenu(const QString &text) {
        if (popupVisible_) {
            qInfo() << "Popup already visible; skipping.";
            return;
        }
        popupVisible_ = true;
        if (pollEnabled_) {
            pollTimer_.stop();
        }
        QList<MenuAction> actions;
        actions.append({"UPPERCASE", [this, text]() { setClipboardText(text.toUpper()); }, true, "sp:SP_ArrowUp"});
        actions.append({"lowercase", [this, text]() { setClipboardText(text.toLower()); }, true, "sp:SP_ArrowDown"});
        actions.append({"Title Case", [this, text]() { setClipboardText(toTitleCase(text)); }, true, "sp:SP_FileDialogDetailedView"});
        actions.append({"Normalize Whitespace", [this, text]() { setClipboardText(normalizeWhitespace(text)); }, true, "sp:SP_BrowserReload"});
        actions.append({"Paste and Match Style", [this, text]() { setClipboardPlainText(text); }, true, "sp:SP_DialogResetButton"});
        actions.append({"Copy to Clipboard", [this, text]() { setClipboardText(text); }, true, "sp:SP_DialogOpenButton"});
        // actions.append({"Search Google", [text]() {
        //     const QString query = QString::fromUtf8(QUrl::toPercentEncoding(text));
        //     QDesktopServices::openUrl(QUrl("https://www.google.com/search?q=" + query));
        // }});
        // actions.append({"Search Amazon", [text]() {
        //     const QString query = QString::fromUtf8(QUrl::toPercentEncoding(text));
        //     QDesktopServices::openUrl(QUrl("https://www.amazon.com/s?k=" + query));
        // }});

        QList<ExternalAction> externals = loadExternalActions();
        if (!externals.isEmpty()) {
            for (const ExternalAction &ext : externals) {
                actions.append({ext.label, [ext, text]() {
                    const bool ok = QProcess::startDetached(ext.command, expandArgs(ext.args, text));
                    qInfo() << "External action" << ext.command << "started:" << ok;
                }, true, ext.icon});
            }
        }

        qInfo() << "Showing menu with" << actions.size() << "items.";
        popup_.setContent(text, actions);
        popup_.showAtCursor();
    }

    void setClipboardText(const QString &text) {
        suppressNext_ = true;
        lastText_ = text;
        qInfo() << "Setting clipboard text len=" << text.size();
        clipboard_->setText(text);
    }

    void setClipboardPlainText(const QString &text) {
        suppressNext_ = true;
        lastText_ = text;
        qInfo() << "Setting clipboard plain text len=" << text.size();
        auto *mime = new QMimeData();
        mime->setText(text);
        clipboard_->setMimeData(mime, QClipboard::Clipboard);
    }

    void logClipboardState(const char *prefix, QClipboard::Mode mode) {
        const QString text = clipboard_->text(mode).trimmed();
        qInfo() << prefix << "mode" << mode << "len=" << text.size()
                << "preview=" << previewText(text);
    }

    void pollClipboard() {
        QString clip = clipboard_->text(QClipboard::Clipboard).trimmed();
        QString sel = clipboard_->text(QClipboard::Selection).trimmed();

        if (wlPasteEnabled_) {
            bool okClipboard = false;
            bool okSelection = false;
            QString wlClip;
            QString wlSel;
            if (wlPasteMode_ != "primary") {
                wlClip = readWlPaste({}, 200, &okClipboard);
                if (okClipboard && !wlClip.isEmpty()) {
                    clip = wlClip;
                }
            }
            if (wlPasteMode_ != "clipboard") {
                wlSel = readWlPaste({"--primary"}, 200, &okSelection);
                if (okSelection && !wlSel.isEmpty()) {
                    sel = wlSel;
                }
            }
            if (traceEnabled_) {
                qInfo() << "Trace: wl-paste ok=" << okClipboard << "len=" << wlClip.size();
                qInfo() << "Trace: wl-paste --primary ok=" << okSelection << "len=" << wlSel.size();
            }
        }

        if (traceEnabled_) {
            qInfo() << "Trace: clipboard len=" << clip.size() << "preview=" << previewText(clip);
            qInfo() << "Trace: selection len=" << sel.size() << "preview=" << previewText(sel);
        }

        if (clip != lastClipboardText_) {
            lastClipboardText_ = clip;
            if (!clip.isEmpty()) {
                qInfo() << "Poll: clipboard changed len=" << clip.size();
                pendingMode_ = QClipboard::Clipboard;
                showMenuIfNeededWithText(clip);
            }
        }

        if (sel != lastSelectionText_) {
            lastSelectionText_ = sel;
            if (!sel.isEmpty()) {
                qInfo() << "Poll: selection changed len=" << sel.size();
                pendingMode_ = QClipboard::Selection;
                showMenuIfNeededWithText(sel);
            }
        }
    }

    QClipboard *clipboard_ = nullptr;
    QClipboard::Mode pendingMode_ = QClipboard::Clipboard;
    QTimer debounce_;
    QTimer pollTimer_;
    ActionPopup popup_;
    QString lastText_;
    QString lastClipboardText_;
    QString lastSelectionText_;
    bool suppressNext_ = false;
    bool pollEnabled_ = false;
    bool traceEnabled_ = false;
    bool wlPasteEnabled_ = false;
    bool popupVisible_ = false;
    QElapsedTimer popupTimer_;
    qint64 nextAllowedPopupMs_ = 0;
    int pollIntervalMs_ = 1500;
    QString wlPasteMode_ = "primary";
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    const AppSettings settings = loadSettings();
    gMinLogLevel = logLevelFromString(settings.logLevel);
    gPrevLogHandler = qInstallMessageHandler(logHandler);

    qInfo() << "codexpopclip started. Platform:" << QGuiApplication::platformName();

    PopupController controller(settings);
    return app.exec();
}

#include "main.moc"
