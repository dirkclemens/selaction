#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QDebug>
#include <QVBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QKeyEvent>
#include <functional>

namespace {

struct ExternalAction {
    QString label;
    QString command;
    QStringList args;
};

struct MenuAction {
    QString label;
    std::function<void()> handler;
    bool enabled = true;
};

struct AppSettings {
    bool pollEnabled = false;
    int pollIntervalMs = 1500;
    bool wlPasteEnabled = false;
    QString wlPasteMode = "primary";
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

class PopupController : public QObject {
    Q_OBJECT

public:
    explicit PopupController(QObject *parent = nullptr)
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

        AppSettings settings = loadSettings();
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_POLL")) {
            settings.pollEnabled = true;
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_POLL_MS")) {
            const int value = qEnvironmentVariableIntValue("CODEXPOPCLIP_POLL_MS");
            if (value > 0) {
                settings.pollIntervalMs = value;
            }
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_WLPASTE")) {
            settings.wlPasteEnabled = true;
        }
        if (qEnvironmentVariableIsSet("CODEXPOPCLIP_WLPASTE_MODE")) {
            const QString mode = qEnvironmentVariable("CODEXPOPCLIP_WLPASTE_MODE", settings.wlPasteMode);
            if (!mode.isEmpty()) {
                settings.wlPasteMode = mode;
            }
        }

        pollEnabled_ = settings.pollEnabled;
        pollIntervalMs_ = settings.pollIntervalMs;
        wlPasteEnabled_ = settings.wlPasteEnabled;
        wlPasteMode_ = settings.wlPasteMode;
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

            list_ = new QListWidget(this);
            list_->setSelectionMode(QAbstractItemView::SingleSelection);
            list_->setUniformItemSizes(true);
            list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

            auto *layout = new QVBoxLayout(this);
            layout->setContentsMargins(6, 6, 6, 6);
            layout->addWidget(list_);

            connect(list_, &QListWidget::itemActivated, this, &ActionPopup::onItemTriggered);
            connect(list_, &QListWidget::itemClicked, this, &ActionPopup::onItemTriggered);
        }

        void setOnClosed(std::function<void()> handler) {
            onClosed_ = std::move(handler);
        }

        void setContent(const QString &selectedText, const QList<MenuAction> &actions) {
            actions_ = actions;
            list_->clear();

            auto *header = new QListWidgetItem(selectedText, list_);
            header->setFlags(Qt::NoItemFlags);

            auto *separator = new QListWidgetItem("----------------", list_);
            separator->setFlags(Qt::NoItemFlags);

            int actionIndex = 0;
            for (const MenuAction &action : actions_) {
                auto *item = new QListWidgetItem(action.label, list_);
                if (!action.enabled) {
                    item->setFlags(Qt::NoItemFlags);
                } else {
                    item->setData(Qt::UserRole, actionIndex);
                }
                actionIndex++;
            }

            if (!actions_.isEmpty()) {
                list_->setCurrentRow(2);
            }
            const int rowCount = list_->count();
            const int rowHeight = list_->sizeHintForRow(0) > 0 ? list_->sizeHintForRow(0) : 20;
            const int listHeight = (rowHeight * rowCount) + (2 * list_->frameWidth());
            list_->setFixedHeight(listHeight);
            adjustSize();
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
            if (underMouse() || list_->underMouse()) {
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
        void onItemTriggered(QListWidgetItem *item) {
            if (!item) {
                return;
            }
            const QVariant actionIndexVariant = item->data(Qt::UserRole);
            if (!actionIndexVariant.isValid()) {
                return;
            }
            const int actionIndex = actionIndexVariant.toInt();
            if (actionIndex < 0 || actionIndex >= actions_.size()) {
                return;
            }
            qInfo() << "Menu choice:" << actions_[actionIndex].label;
            if (actions_[actionIndex].handler) {
                actions_[actionIndex].handler();
            }
            hide();
        }

    private:
        QListWidget *list_ = nullptr;
        QList<MenuAction> actions_;
        std::function<void()> onClosed_;
        QElapsedTimer showTimer_;
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
        actions.append({"UPPERCASE", [this, text]() { setClipboardText(text.toUpper()); }});
        actions.append({"lowercase", [this, text]() { setClipboardText(text.toLower()); }});
        actions.append({"Title Case", [this, text]() { setClipboardText(toTitleCase(text)); }});
        actions.append({"Normalize Whitespace", [this, text]() { setClipboardText(normalizeWhitespace(text)); }});
        actions.append({"Copy to Clipboard", [this, text]() { setClipboardText(text); }});
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
            //actions.append({"Custom actions", nullptr, false});
            actions.append({"----------------", nullptr, false});
            for (const ExternalAction &ext : externals) {
                actions.append({ext.label, [ext, text]() {
                    const bool ok = QProcess::startDetached(ext.command, expandArgs(ext.args, text));
                    qInfo() << "External action" << ext.command << "started:" << ok;
                }});
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

    qInfo() << "codexpopclip started. Platform:" << QGuiApplication::platformName();

    PopupController controller;
    return app.exec();
}

#include "main.moc"
