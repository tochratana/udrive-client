// SPDX-License-Identifier: GPL-2.0-or-later
#include "appearancewidget.h"
#include "application.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QtMath>

namespace OCC {

class ThemePreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ThemePreviewWidget(ConfigFile::ThemeMode mode, QWidget *parent = nullptr)
        : QWidget(parent)
        , _mode(mode)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(114);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setSelected(bool selected)
    {
        if (_selected != selected) {
            _selected = selected;
            update();
        }
    }

    void setSystemIsDark(bool dark)
    {
        if (_systemIsDark != dark) {
            _systemIsDark = dark;
            if (_mode == ConfigFile::ThemeMode::System)
                update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        const qreal radius = 8.0;

        // Clip to rounded window
        QPainterPath clipPath;
        clipPath.addRoundedRect(bounds, radius, radius);
        p.setClipPath(clipPath);

        const bool isDark = (_mode == ConfigFile::ThemeMode::Dark) || (_mode == ConfigFile::ThemeMode::System && _systemIsDark);
        const QColor winBg = (_mode == ConfigFile::ThemeMode::Light) ? QColor(QStringLiteral("#EFF4F8"))
            : (_mode == ConfigFile::ThemeMode::Dark)                 ? QColor(QStringLiteral("#0F1722"))
                                                                     : QColor(QStringLiteral("#1B2431"));
        p.fillRect(bounds, winBg);

        // Title Bar
        const QRectF titleBar(bounds.left(), bounds.top(), bounds.width(), 22);
        const QColor barBg = (_mode == ConfigFile::ThemeMode::Light) ? QColor(QStringLiteral("#DFE7EE"))
            : (_mode == ConfigFile::ThemeMode::Dark)                 ? QColor(QStringLiteral("#1A2230"))
                                                                     : QColor(QStringLiteral("#141C27"));
        p.fillRect(titleBar, barBg);

        // Traffic lights (Red, Yellow, Green)
        const qreal dotY = bounds.top() + 11;
        const qreal dotRadius = 2.8;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(QStringLiteral("#FF5F56")));
        p.drawEllipse(QPointF(bounds.left() + 12, dotY), dotRadius, dotRadius);
        p.setBrush(QColor(QStringLiteral("#FFBD2E")));
        p.drawEllipse(QPointF(bounds.left() + 21, dotY), dotRadius, dotRadius);
        p.setBrush(QColor(QStringLiteral("#27C93F")));
        p.drawEllipse(QPointF(bounds.left() + 30, dotY), dotRadius, dotRadius);

        // Header Pill
        const QColor headerPillColor = (_mode == ConfigFile::ThemeMode::Light) ? QColor(QStringLiteral("#D2DFE8")) : QColor(QStringLiteral("#253344"));
        p.setBrush(headerPillColor);
        p.drawRoundedRect(QRectF(bounds.center().x() - 20, bounds.top() + 8, 40, 5), 2.5, 2.5);

        if (_mode == ConfigFile::ThemeMode::System) {
            // Split window layout matching mockup
            p.setPen(QPen(QColor(QStringLiteral("#253243")), 1.0));
            p.drawLine(QPointF(bounds.center().x(), bounds.top() + 22), QPointF(bounds.center().x(), bounds.bottom()));
            p.setPen(Qt::NoPen);

            const qreal cardW = (bounds.width() / 2.0) - 14;
            const qreal cardH = bounds.height() - 34;

            // Left pane
            const QRectF leftCard(bounds.left() + 8, bounds.top() + 27, cardW, cardH);
            p.setBrush(QColor(QStringLiteral("#16212E")));
            p.drawRoundedRect(leftCard, 5, 5);
            p.setBrush(QColor(QStringLiteral("#FFFFFF")));
            p.drawRoundedRect(QRectF(leftCard.left() + 6, leftCard.top() + 8, leftCard.width() - 14, 4), 2, 2);

            // Right pane
            const QRectF rightCard(bounds.center().x() + 6, bounds.top() + 27, cardW, cardH);
            p.setBrush(QColor(QStringLiteral("#16212E")));
            p.drawRoundedRect(rightCard, 5, 5);
            p.setBrush(QColor(QStringLiteral("#394C66")));
            p.drawRoundedRect(QRectF(rightCard.left() + 6, rightCard.top() + 8, rightCard.width() - 16, 4), 2, 2);
            p.setBrush(QColor(QStringLiteral("#29384D")));
            p.drawRoundedRect(QRectF(rightCard.left() + 6, rightCard.top() + 16, rightCard.width() - 24, 3), 1.5, 1.5);
        } else {
            // Sidebar
            const qreal sidebarW = 34;
            const QRectF sidebar(bounds.left() + 6, bounds.top() + 27, sidebarW, bounds.height() - 33);
            const QColor sidebarBg = isDark ? QColor(QStringLiteral("#15202D")) : QColor(QStringLiteral("#DDE7EE"));
            p.setBrush(sidebarBg);
            p.drawRoundedRect(sidebar, 5, 5);

            const QColor sidebarItem = isDark ? QColor(QStringLiteral("#2D3C4E")) : QColor(QStringLiteral("#FFFFFF"));
            p.setBrush(sidebarItem);
            p.drawRoundedRect(QRectF(sidebar.left() + 6, sidebar.top() + 8, sidebarW - 12, 3), 1.5, 1.5);
            p.drawRoundedRect(QRectF(sidebar.left() + 6, sidebar.top() + 15, sidebarW - 18, 3), 1.5, 1.5);

            // Content card
            const QRectF content(sidebar.right() + 6, bounds.top() + 27, bounds.right() - sidebar.right() - 12, bounds.height() - 33);
            const QColor contentBg = isDark ? QColor(QStringLiteral("#192534")) : QColor(QStringLiteral("#FFFFFF"));
            p.setBrush(contentBg);
            if (isDark) {
                p.setPen(QPen(QColor(QStringLiteral("#233245")), 1.0));
            } else {
                p.setPen(Qt::NoPen);
            }
            p.drawRoundedRect(content, 6, 6);
            p.setPen(Qt::NoPen);

            const QColor accentBar = isDark ? QColor(QStringLiteral("#385068")) : QColor(QStringLiteral("#283540"));
            p.setBrush(accentBar);
            p.drawRoundedRect(QRectF(content.left() + 8, content.top() + 8, 38, 6), 3, 3);

            const QColor line1 = isDark ? QColor(QStringLiteral("#2A3C4E")) : QColor(QStringLiteral("#D0DCE5"));
            p.setBrush(line1);
            p.drawRoundedRect(QRectF(content.left() + 8, content.top() + 18, content.width() - 20, 3.5), 1.75, 1.75);

            const QColor line2 = isDark ? QColor(QStringLiteral("#253444")) : QColor(QStringLiteral("#DDE6ED"));
            p.setBrush(line2);
            p.drawRoundedRect(QRectF(content.left() + 8, content.top() + 25, 60, 3.5), 1.75, 1.75);
        }

        // Circular Green Checkmark Badge on Top-Right
        p.setClipping(false);
        if (_selected) {
            const QPointF checkCenter(bounds.right() - 14, bounds.top() + 14);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(QStringLiteral("#40D979")));
            p.drawEllipse(checkCenter, 9.5, 9.5);

            QPainterPath checkPath;
            checkPath.moveTo(checkCenter.x() - 3.8, checkCenter.y() - 0.2);
            checkPath.lineTo(checkCenter.x() - 1.0, checkCenter.y() + 2.8);
            checkPath.lineTo(checkCenter.x() + 4.0, checkCenter.y() - 3.0);

            p.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(checkPath);
        }
    }

private:
    ConfigFile::ThemeMode _mode;
    bool _selected = false;
    bool _systemIsDark = false;
};

class ThemeBadgeIconWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ThemeBadgeIconWidget(ConfigFile::ThemeMode mode, QWidget *parent = nullptr)
        : QWidget(parent)
        , _mode(mode)
    {
        setFixedSize(32, 32);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF r = rect();
        const QPointF c = r.center();

        p.setPen(Qt::NoPen);
        if (_mode == ConfigFile::ThemeMode::Light) {
            p.setBrush(QColor(QStringLiteral("#FEF3D6")));
            p.drawEllipse(r);

            p.setPen(QPen(QColor(QStringLiteral("#D97706")), 1.8, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 4.0, 4.0);
            for (int i = 0; i < 8; ++i) {
                const qreal angle = i * M_PI / 4.0;
                p.drawLine(c + QPointF(qCos(angle) * 6.2, qSin(angle) * 6.2),
                           c + QPointF(qCos(angle) * 9.0, qSin(angle) * 9.0));
            }
        } else if (_mode == ConfigFile::ThemeMode::Dark) {
            p.setBrush(QColor(QStringLiteral("#ECE7F6")));
            p.drawEllipse(r);

            QPainterPath moon;
            moon.moveTo(c.x() + 2.5, c.y() - 6.5);
            moon.cubicTo(c.x() - 6.5, c.y() - 6.5, c.x() - 6.5, c.y() + 6.5, c.x() + 2.5, c.y() + 6.5);
            moon.cubicTo(c.x() - 2.5, c.y() + 4.0, c.x() - 2.5, c.y() - 4.0, c.x() + 2.5, c.y() - 6.5);
            moon.closeSubpath();

            p.setPen(Qt::NoPen);
            p.setBrush(QColor(QStringLiteral("#7C3AED")));
            p.drawPath(moon);
        } else {
            p.setBrush(QColor(QStringLiteral("#E0F2FE")));
            p.drawEllipse(r);

            p.setPen(QPen(QColor(QStringLiteral("#0284C7")), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            // Screen
            p.drawRoundedRect(QRectF(c.x() - 7.5, c.y() - 7.0, 15, 10.5), 1.5, 1.5);
            // Stand
            p.drawLine(QPointF(c.x(), c.y() + 3.5), QPointF(c.x(), c.y() + 6.5));
            p.drawLine(QPointF(c.x() - 4, c.y() + 6.5), QPointF(c.x() + 4, c.y() + 6.5));
        }
    }

private:
    ConfigFile::ThemeMode _mode;
};

ThemeCardWidget::ThemeCardWidget(ConfigFile::ThemeMode mode, const QString &title, const QString &subtitle, QWidget *parent)
    : QFrame(parent)
    , _mode(mode)
    , _titleText(title)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumWidth(180);
    setMaximumWidth(248);
    setFixedHeight(204);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    _preview = new ThemePreviewWidget(_mode, this);
    mainLayout->addWidget(_preview);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(2, 0, 2, 2);
    bottomRow->setSpacing(10);

    auto *badgeIcon = new ThemeBadgeIconWidget(_mode, this);
    bottomRow->addWidget(badgeIcon, 0, Qt::AlignVCenter);

    auto *textLayout = new QVBoxLayout;
    textLayout->setSpacing(2);
    textLayout->setContentsMargins(0, 0, 0, 0);

    _titleLabel = new QLabel(this);
    QFont titleFont = _titleLabel->font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    _titleLabel->setFont(titleFont);
    textLayout->addWidget(_titleLabel);

    _subtitleLabel = new QLabel(subtitle, this);
    _subtitleLabel->setWordWrap(true);
    QFont subFont = _subtitleLabel->font();
    subFont.setPointSize(11);
    _subtitleLabel->setFont(subFont);
    textLayout->addWidget(_subtitleLabel);

    bottomRow->addLayout(textLayout, 1);
    mainLayout->addLayout(bottomRow);

    updateLabels(false);
    updateCardTheme();
}

void ThemeCardWidget::setSelected(bool selected)
{
    if (_selected != selected) {
        _selected = selected;
        _preview->setSelected(_selected);
        updateCardTheme();
        update();
    }
}

void ThemeCardWidget::updateSystemState(bool isDark)
{
    _systemIsDark = isDark;
    _preview->setSystemIsDark(isDark);
    updateLabels(isDark);
}

void ThemeCardWidget::updateLabels(bool isDark)
{
    if (_mode == ConfigFile::ThemeMode::System) {
        _titleLabel->setText(tr("System <span style='font-size:11px; font-weight:normal; color:#9BA3AF;'>(%1)</span>")
            .arg(isDark ? tr("Dark") : tr("Light")));
        _titleLabel->setTextFormat(Qt::RichText);
    } else {
        _titleLabel->setText(_titleText);
    }
}

void ThemeCardWidget::retranslate(const QString &title, const QString &subtitle)
{
    _titleText = title;
    _subtitleLabel->setText(subtitle);
    updateLabels(_systemIsDark);
}

void ThemeCardWidget::updateCardTheme()
{
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString titleColor = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#202923");
    const QString subColor = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#5D6B62");

    _titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(titleColor));
    _subtitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(subColor));
    update();
}

void ThemeCardWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        setFocus();
        Q_EMIT clicked();
    }
    QFrame::mousePressEvent(e);
}

void ThemeCardWidget::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        Q_EMIT clicked();
        e->accept();
        return;
    }
    QFrame::keyPressEvent(e);
}

void ThemeCardWidget::enterEvent(QEnterEvent *e)
{
    _hovered = true;
    update();
    QFrame::enterEvent(e);
}

void ThemeCardWidget::leaveEvent(QEvent *e)
{
    _hovered = false;
    update();
    QFrame::leaveEvent(e);
}

void ThemeCardWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QRectF cardRect = QRectF(rect()).adjusted(1, 1, -1, -1);

    const QColor cardBg = dark ? QColor(QStringLiteral("#2E3033")) : QColor(QStringLiteral("#FFFFFF"));
    painter.setBrush(cardBg);

    if (_selected) {
        painter.setPen(QPen(QColor(QStringLiteral("#40D979")), 2.0));
    } else if (_hovered) {
        painter.setPen(QPen(dark ? QColor(QStringLiteral("#5A6169")) : QColor(QStringLiteral("#CBD1D6")), 1.0));
    } else {
        painter.setPen(QPen(dark ? QColor(QStringLiteral("#414549")) : QColor(QStringLiteral("#E1E5E8")), 1.0));
    }

    painter.drawRoundedRect(cardRect, 12, 12);

    if (hasFocus() && !_selected) {
        painter.setPen(QPen(QColor(QStringLiteral("#40D979")), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cardRect.adjusted(2, 2, -2, -2), 10, 10);
    }
}

AppearanceWidget::AppearanceWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 14);
    vbox->setSpacing(4);

    _header = new QLabel(tr("Appearance"), this);
    QFont headerFont = _header->font();
    headerFont.setPointSize(16);
    headerFont.setBold(true);
    _header->setFont(headerFont);
    vbox->addWidget(_header);

    _subtitle = new QLabel(tr("Customize the color theme and display mode of your workspace."), this);
    _subtitle->setWordWrap(true);
    QFont subFont = _subtitle->font();
    subFont.setPointSize(12);
    _subtitle->setFont(subFont);
    vbox->addWidget(_subtitle);
    vbox->addSpacing(8);

    auto *cardsLayout = new QHBoxLayout;
    cardsLayout->setSpacing(14);
    cardsLayout->setContentsMargins(0, 0, 0, 0);

    _cards[ConfigFile::ThemeMode::Light] = new ThemeCardWidget(ConfigFile::ThemeMode::Light, tr("Light"), tr("Bright & clear daytime look"), this);
    _cards[ConfigFile::ThemeMode::Dark] = new ThemeCardWidget(ConfigFile::ThemeMode::Dark, tr("Dark"), tr("60-30-10 charcoal workspace"), this);
    _cards[ConfigFile::ThemeMode::System] = new ThemeCardWidget(ConfigFile::ThemeMode::System, tr("System"), tr("Follows macOS & OS settings"), this);

    cardsLayout->addWidget(_cards[ConfigFile::ThemeMode::Light]);
    cardsLayout->addWidget(_cards[ConfigFile::ThemeMode::Dark]);
    cardsLayout->addWidget(_cards[ConfigFile::ThemeMode::System]);
    cardsLayout->addStretch(1);

    for (auto it = _cards.begin(); it != _cards.end(); ++it) {
        const auto mode = it.key();
        auto *card = it.value();
        connect(card, &ThemeCardWidget::clicked, this, [this, mode] {
            setThemeMode(mode, true);
        });
    }

    vbox->addLayout(cardsLayout);

    setThemeMode(ConfigFile().themeMode(), false);
    updateSystemAppearance();
    updateThemeStyles();
}

void AppearanceWidget::setThemeMode(ConfigFile::ThemeMode mode, bool emitSignal)
{
    _currentMode = mode;
    for (auto it = _cards.begin(); it != _cards.end(); ++it) {
        it.value()->setSelected(it.key() == mode);
    }
    if (emitSignal) {
        Q_EMIT themeChanged(_currentMode);
    }
}

void AppearanceWidget::updateSystemAppearance()
{
    const bool sysDark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    for (auto *card : _cards) {
        card->updateSystemState(sysDark);
    }
}

void AppearanceWidget::updateThemeStyles()
{
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    if (_header) {
        _header->setStyleSheet(QStringLiteral("color: %1;").arg(dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#202923")));
    }
    if (_subtitle) {
        _subtitle->setStyleSheet(QStringLiteral("color: %1;").arg(dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#5D6B62")));
    }
    for (auto *card : _cards) {
        card->updateCardTheme();
    }
}

void AppearanceWidget::retranslate()
{
    if (_header) _header->setText(tr("Appearance"));
    if (_subtitle) _subtitle->setText(tr("Customize the color theme and display mode of your workspace."));
    if (auto *card = _cards.value(ConfigFile::ThemeMode::Light))
        card->retranslate(tr("Light"), tr("Bright & clear daytime look"));
    if (auto *card = _cards.value(ConfigFile::ThemeMode::Dark))
        card->retranslate(tr("Dark"), tr("60-30-10 charcoal workspace"));
    if (auto *card = _cards.value(ConfigFile::ThemeMode::System))
        card->retranslate(tr("System"), tr("Follows macOS & OS settings"));
}

void AppearanceWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        updateSystemAppearance();
        updateThemeStyles();
    }
    QWidget::changeEvent(event);
}

} // namespace OCC

#include "appearancewidget.moc"
