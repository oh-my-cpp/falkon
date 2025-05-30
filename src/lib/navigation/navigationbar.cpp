/* ============================================================
* Falkon - Qt web browser
* Copyright (C) 2010-2018 David Rosca <nowrep@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ============================================================ */
#include "navigationbar.h"
#include "toolbutton.h"
#include "browserwindow.h"
#include "mainapplication.h"
#include "iconprovider.h"
#include "websearchbar.h"
#include "reloadstopbutton.h"
#include "enhancedmenu.h"
#include "tabwidget.h"
#include "tabbedwebview.h"
#include "webpage.h"
#include "qzsettings.h"
#include "qztools.h"
#include "abstractbuttoninterface.h"
#include "navigationbartoolbutton.h"
#include "navigationbarconfigdialog.h"
#include "statusbar.h"

#include <QTimer>
#include <QSplitter>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QWebEngineHistory>
#include <QMouseEvent>
#include <QStyleOption>

static QString titleForUrl(QString title, const QUrl &url)
{
    if (title.isEmpty()) {
        title = url.toString(QUrl::RemoveFragment);
    }
    if (title.isEmpty()) {
        return NavigationBar::tr("Empty Page");
    }
    return QzTools::truncatedText(title, 40);
}

static QIcon iconForPage(const QUrl &url, const QIcon &sIcon)
{
    QIcon icon;
    icon.addPixmap(IconProvider::iconForUrl(url).pixmap(16));
    icon.addPixmap(sIcon.pixmap(16), QIcon::Active);
    return icon;
}

NavigationBar::NavigationBar(BrowserWindow* window)
    : QWidget(window)
    , m_window(window)
{
    setObjectName(QSL("navigationbar"));

    m_layout = new QHBoxLayout(this);
    auto contentsMargin = style()->pixelMetric(QStyle::PM_ToolBarItemMargin, nullptr, this)
                        + style()->pixelMetric(QStyle::PM_ToolBarFrameWidth, nullptr, this);
    m_layout->setContentsMargins(contentsMargin, contentsMargin, contentsMargin, contentsMargin);
    m_layout->setSpacing(style()->pixelMetric(QStyle::PM_ToolBarItemSpacing, nullptr, this));
    setLayout(m_layout);

    m_buttonBack = new ToolButton(this);
    m_buttonBack->setObjectName("navigation-button-back");
    m_buttonBack->setToolTip(tr("Back"));
    m_buttonBack->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_buttonBack->setToolbarButtonLook(true);
    m_buttonBack->setShowMenuOnRightClick(true);
    m_buttonBack->setAutoRaise(true);
    m_buttonBack->setEnabled(false);
    m_buttonBack->setFocusPolicy(Qt::NoFocus);

    m_buttonForward = new ToolButton(this);
    m_buttonForward->setObjectName("navigation-button-next");
    m_buttonForward->setToolTip(tr("Forward"));
    m_buttonForward->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_buttonForward->setToolbarButtonLook(true);
    m_buttonForward->setShowMenuOnRightClick(true);
    m_buttonForward->setAutoRaise(true);
    m_buttonForward->setEnabled(false);
    m_buttonForward->setFocusPolicy(Qt::NoFocus);

    auto* backNextLayout = new QHBoxLayout();
    backNextLayout->setContentsMargins(0, 0, 0, 0);
    backNextLayout->setSpacing(0);
    backNextLayout->addWidget(m_buttonBack);
    backNextLayout->addWidget(m_buttonForward);
    auto *backNextWidget = new QWidget(this);
    backNextWidget->setLayout(backNextLayout);

    m_reloadStop = new ReloadStopButton(this);

    auto *buttonHome = new ToolButton(this);
    buttonHome->setObjectName("navigation-button-home");
    buttonHome->setToolTip(tr("Home"));
    buttonHome->setToolButtonStyle(Qt::ToolButtonIconOnly);
    buttonHome->setToolbarButtonLook(true);
    buttonHome->setAutoRaise(true);
    buttonHome->setFocusPolicy(Qt::NoFocus);

    auto *buttonAddTab = new ToolButton(this);
    buttonAddTab->setObjectName("navigation-button-addtab");
    buttonAddTab->setToolTip(tr("New Tab"));
    buttonAddTab->setToolButtonStyle(Qt::ToolButtonIconOnly);
    buttonAddTab->setToolbarButtonLook(true);
    buttonAddTab->setAutoRaise(true);
    buttonAddTab->setFocusPolicy(Qt::NoFocus);

    m_menuBack = new Menu(this);
    m_menuBack->setCloseOnMiddleClick(true);
    m_buttonBack->setMenu(m_menuBack);
    connect(m_buttonBack, &ToolButton::aboutToShowMenu, this, &NavigationBar::aboutToShowHistoryBackMenu);

    m_menuForward = new Menu(this);
    m_menuForward->setCloseOnMiddleClick(true);
    m_buttonForward->setMenu(m_menuForward);
    connect(m_buttonForward, &ToolButton::aboutToShowMenu, this, &NavigationBar::aboutToShowHistoryNextMenu);

    auto *buttonTools = new ToolButton(this);
    buttonTools->setObjectName("navigation-button-tools");
    buttonTools->setPopupMode(QToolButton::InstantPopup);
    buttonTools->setToolbarButtonLook(true);
    buttonTools->setToolTip(tr("Tools"));
    buttonTools->setAutoRaise(true);
    buttonTools->setFocusPolicy(Qt::NoFocus);
    buttonTools->setShowMenuInside(true);

    m_menuTools = new Menu(this);
    buttonTools->setMenu(m_menuTools);
    connect(buttonTools, &ToolButton::aboutToShowMenu, this, &NavigationBar::aboutToShowToolsMenu);

    m_supMenu = new ToolButton(this);
    m_supMenu->setObjectName("navigation-button-supermenu");
    m_supMenu->setPopupMode(QToolButton::InstantPopup);
    m_supMenu->setToolbarButtonLook(true);
    m_supMenu->setToolTip(tr("Main Menu"));
    m_supMenu->setAutoRaise(true);
    m_supMenu->setFocusPolicy(Qt::NoFocus);
    m_supMenu->setMenu(m_window->superMenu());
    m_supMenu->setShowMenuInside(true);

    m_searchLine = new WebSearchBar(m_window);

    m_navigationSplitter = new QSplitter(this);
    m_navigationSplitter->addWidget(m_window->tabWidget()->locationBars());
    m_navigationSplitter->addWidget(m_searchLine);

    m_navigationSplitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_navigationSplitter->setCollapsible(0, false);

    m_exitFullscreen = new ToolButton(this);
    m_exitFullscreen->setObjectName("navigation-button-exitfullscreen");
    m_exitFullscreen->setToolTip(tr("Exit Fullscreen"));
    m_exitFullscreen->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_exitFullscreen->setToolbarButtonLook(true);
    m_exitFullscreen->setFocusPolicy(Qt::NoFocus);
    m_exitFullscreen->setAutoRaise(true);
    m_exitFullscreen->setVisible(false);

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &NavigationBar::contextMenuRequested);

    connect(m_buttonBack, &QAbstractButton::clicked, this, &NavigationBar::goBack);
    connect(m_buttonBack, &ToolButton::middleMouseClicked, this, &NavigationBar::goBackInNewTab);
    connect(m_buttonBack, &ToolButton::controlClicked, this, &NavigationBar::goBackInNewTab);
    connect(m_buttonForward, &QAbstractButton::clicked, this, &NavigationBar::goForward);
    connect(m_buttonForward, &ToolButton::middleMouseClicked, this, &NavigationBar::goForwardInNewTab);
    connect(m_buttonForward, &ToolButton::controlClicked, this, &NavigationBar::goForwardInNewTab);

    connect(m_reloadStop, &ReloadStopButton::stopClicked, this, &NavigationBar::stop);
    connect(m_reloadStop, &ReloadStopButton::reloadClicked, this, &NavigationBar::reload);
    connect(buttonHome, &QAbstractButton::clicked, m_window, &BrowserWindow::goHome);
    connect(buttonHome, &ToolButton::middleMouseClicked, m_window, &BrowserWindow::goHomeInNewTab);
    connect(buttonHome, &ToolButton::controlClicked, m_window, &BrowserWindow::goHomeInNewTab);
    connect(buttonAddTab, &QAbstractButton::clicked, m_window, &BrowserWindow::addTab);
    connect(buttonAddTab, &ToolButton::middleMouseClicked, m_window->tabWidget(), &TabWidget::addTabFromClipboard);
    connect(m_exitFullscreen, &QAbstractButton::clicked, m_window, &BrowserWindow::toggleFullScreen);

    addWidget(backNextWidget, QSL("button-backforward"), tr("Back and Forward buttons"));
    addWidget(m_reloadStop, QSL("button-reloadstop"), tr("Reload button"));
    addWidget(buttonHome, QSL("button-home"), tr("Home button"));
    addWidget(buttonAddTab, QSL("button-addtab"), tr("Add tab button"));
    addWidget(m_navigationSplitter, QSL("locationbar"), tr("Address and Search bar"));
    addWidget(buttonTools, QSL("button-tools"), tr("Tools button"));
    addWidget(m_exitFullscreen, QSL("button-exitfullscreen"), tr("Exit Fullscreen button"));

    loadSettings();
}

NavigationBar::~NavigationBar()
{
    setCurrentView(nullptr);
}

void NavigationBar::setSplitterSizes(int locationBar, int websearchBar)
{
    QList<int> sizes;

    if (locationBar == 0) {
        int splitterWidth = m_navigationSplitter->width();
        sizes << (int)((double)splitterWidth * .80) << (int)((double)splitterWidth * .20);
    }
    else {
        sizes << locationBar << websearchBar;
    }

    m_navigationSplitter->setSizes(sizes);
}

void NavigationBar::setCurrentView(TabbedWebView *view)
{
    for (const WidgetData &data : std::as_const(m_widgets)) {
        if (data.button) {
            data.button->setWebView(view);
        }
    }

    if (!view) {
        return;
    }

    auto connectPageActions = [this](QWebEnginePage *page) {
        auto updateButton = [](ToolButton *button, QAction *action) {
            button->setEnabled(action->isEnabled());
        };
        auto updateBackButton = std::bind(updateButton, m_buttonBack, page->action(QWebEnginePage::Back));
        auto updateForwardButton = std::bind(updateButton, m_buttonForward, page->action(QWebEnginePage::Forward));

        updateBackButton();
        updateForwardButton();

        disconnect(m_backConnection);
        disconnect(m_forwardConnection);
        m_backConnection = connect(page->action(QWebEnginePage::Back), &QAction::changed, this, updateBackButton);
        m_forwardConnection = connect(page->action(QWebEnginePage::Forward), &QAction::changed, this, updateForwardButton);
    };

    connectPageActions(view->page());
    connect(view, &TabbedWebView::pageChanged, this, connectPageActions);
}

void NavigationBar::showReloadButton()
{
    m_reloadStop->showReloadButton();
}

void NavigationBar::showStopButton()
{
    m_reloadStop->showStopButton();
}

void NavigationBar::enterFullScreen()
{
    if (m_layout->indexOf(m_exitFullscreen) != -1) {
        m_exitFullscreen->show();
    }
}

void NavigationBar::leaveFullScreen()
{
    if (m_layout->indexOf(m_exitFullscreen) != -1) {
        m_exitFullscreen->hide();
    }
}

void NavigationBar::setSuperMenuVisible(bool visible)
{
    m_supMenu->setVisible(visible);
}

int NavigationBar::layoutMargin() const
{
    // NOTE: This assumes margins are always the same.
    return m_layout->contentsMargins().left();
}

void NavigationBar::setLayoutMargin(int margin)
{
    m_layout->setContentsMargins(margin, margin, margin, margin);
}

int NavigationBar::layoutSpacing() const
{
    return m_layout->spacing();
}

void NavigationBar::setLayoutSpacing(int spacing)
{
    m_layout->setSpacing(spacing);
}

void NavigationBar::addWidget(QWidget *widget, const QString &id, const QString &name)
{
    if (!widget || id.isEmpty() || name.isEmpty()) {
        return;
    }

    WidgetData data;
    data.id = id;
    data.name = name;
    data.widget = widget;
    m_widgets[id] = data;

    reloadLayout();
}

void NavigationBar::removeWidget(const QString &id)
{
    if (!m_widgets.contains(id)) {
        return;
    }

    m_widgets.remove(id);
    reloadLayout();
}

void NavigationBar::addToolButton(AbstractButtonInterface *button)
{
    if (!button || !button->isValid()) {
        return;
    }

    auto *toolButton = new NavigationBarToolButton(button, this);
    toolButton->setProperty("button-id", button->id());
    connect(toolButton, &NavigationBarToolButton::visibilityChangeRequested, this, [=]() {
        if (m_layout->indexOf(toolButton) != -1) {
            toolButton->updateVisibility();
        }
    });

    WidgetData data;
    data.id = button->id();
    data.name = button->name();
    data.widget = toolButton;
    data.button = button;
    m_widgets[data.id] = data;

    data.button->setWebView(m_window->weView());

    reloadLayout();
}

void NavigationBar::removeToolButton(AbstractButtonInterface *button)
{
    if (!button || !m_widgets.contains(button->id())) {
        return;
    }

    delete m_widgets.take(button->id()).widget;
}

void NavigationBar::aboutToShowHistoryBackMenu()
{
    if (!m_menuBack || !m_window->weView()) {
        return;
    }
    m_menuBack->clear();
    QWebEngineHistory* history = m_window->weView()->history();

    int curindex = history->currentItemIndex();
    int count = 0;

    for (int i = curindex - 1; i >= 0; i--) {
        QWebEngineHistoryItem item = history->itemAt(i);
        if (item.isValid()) {
            QString title = titleForUrl(item.title(), item.url());

            const QIcon icon = iconForPage(item.url(), IconProvider::standardIcon(QStyle::SP_ArrowBack));
            auto* act = new Action(icon, title);
            act->setData(i);
            connect(act, &QAction::triggered, this, &NavigationBar::loadHistoryIndex);
            connect(act, SIGNAL(ctrlTriggered()), this, SLOT(loadHistoryIndexInNewTab()));
            connect(act, &QAction::hovered, [=]() {
                mApp->getWindow()->statusBar()->showMessage(item.url().toString());
            });
            m_menuBack->addAction(act);
        }

        count++;
        if (count == 20) {
            break;
        }
    }

    m_menuBack->addSeparator();
    auto *act = m_menuBack->addAction(QIcon::fromTheme(QSL("edit-clear")), tr("Clear history"), this, &NavigationBar::clearHistory);
    connect(act, &QAction::hovered, mApp->getWindow()->statusBar(), &StatusBar::clearMessage);
    connect(m_menuBack, &QMenu::aboutToHide, mApp->getWindow()->statusBar(), &StatusBar::clearMessage);
}

void NavigationBar::aboutToShowHistoryNextMenu()
{
    if (!m_menuForward || !m_window->weView()) {
        return;
    }
    m_menuForward->clear();

    QWebEngineHistory* history = m_window->weView()->history();
    int curindex = history->currentItemIndex();
    int count = 0;

    for (int i = curindex + 1; i < history->count(); i++) {
        QWebEngineHistoryItem item = history->itemAt(i);
        if (item.isValid()) {
            QString title = titleForUrl(item.title(), item.url());

            const QIcon icon = iconForPage(item.url(), IconProvider::standardIcon(QStyle::SP_ArrowForward));
            auto* act = new Action(icon, title);
            act->setData(i);
            connect(act, &QAction::triggered, this, &NavigationBar::loadHistoryIndex);
            connect(act, SIGNAL(ctrlTriggered()), this, SLOT(loadHistoryIndexInNewTab()));
            connect(act, &QAction::hovered, [=]() {
                mApp->getWindow()->statusBar()->showMessage(item.url().toString());
            });
            m_menuForward->addAction(act);
        }

        count++;
        if (count == 20) {
            break;
        }
    }

    m_menuForward->addSeparator();
    auto *act = m_menuForward->addAction(QIcon::fromTheme(QSL("edit-clear")), tr("Clear history"), this, &NavigationBar::clearHistory);
    connect(act, &QAction::hovered, mApp->getWindow()->statusBar(), &StatusBar::clearMessage);
    connect(m_menuForward, &QMenu::aboutToHide, mApp->getWindow()->statusBar(), &StatusBar::clearMessage);
}

void NavigationBar::aboutToShowToolsMenu()
{
    m_menuTools->clear();

    m_window->createToolbarsMenu(m_menuTools->addMenu(tr("Toolbars")));
    m_window->createSidebarsMenu(m_menuTools->addMenu(tr("Sidebar")));
    m_menuTools->addSeparator();

    for (const WidgetData &data : std::as_const(m_widgets)) {
        AbstractButtonInterface *button = data.button;
        if (button && (!button->isVisible() || !m_layoutIds.contains(data.id))) {
            QString title = button->title();
            if (!button->badgeText().isEmpty()) {
                title.append(QSL(" (%1)").arg(button->badgeText()));
            }
            m_menuTools->addAction(button->icon(), title, this, &NavigationBar::toolActionActivated)->setData(data.id);
        }
    }

    m_menuTools->addSeparator();
    m_menuTools->addAction(IconProvider::settingsIcon(), tr("Configure Toolbar"), this, &NavigationBar::openConfigurationDialog);
}

void NavigationBar::clearHistory()
{
    QWebEngineHistory* history = m_window->weView()->page()->history();
    history->clear();
}

void NavigationBar::contextMenuRequested(const QPoint &pos)
{
    QMenu menu;
    m_window->createToolbarsMenu(&menu);
    menu.addSeparator();
    menu.addAction(IconProvider::settingsIcon(), tr("Configure Toolbar"), this, &NavigationBar::openConfigurationDialog);
    menu.exec(mapToGlobal(pos));
}

void NavigationBar::openConfigurationDialog()
{
    auto *dialog = new NavigationBarConfigDialog(this);
    dialog->show();
}

void NavigationBar::toolActionActivated()
{
    auto *act = qobject_cast<QAction*>(sender());
    if (!act) {
        return;
    }
    const QString id = act->data().toString();
    if (!m_widgets.contains(id)) {
        return;
    }
    WidgetData data = m_widgets.value(id);
    if (!data.button) {
        return;
    }
    auto *buttonTools = qobject_cast<ToolButton*>(m_widgets.value(QSL("button-tools")).widget);
    if (!buttonTools) {
        return;
    }

    auto *c = new AbstractButtonInterface::ClickController;
    c->visualParent = buttonTools;
    c->popupPosition = [=](const QSize &size) {
        QPoint pos = buttonTools->mapToGlobal(buttonTools->rect().bottomRight());
        if (QApplication::isRightToLeft()) {
            pos.setX(pos.x() - buttonTools->rect().width());
        } else {
            pos.setX(pos.x() - size.width());
        }
        c->popupOpened = true;
        return pos;
    };
    c->popupClosed = [=]() {
        buttonTools->setDown(false);
        delete c;
    };
    Q_EMIT data.button->clicked(c);
    if (c->popupOpened) {
        buttonTools->setDown(true);
    } else {
        c->popupClosed();
    }
}

void NavigationBar::loadSettings()
{
    const QStringList defaultIds = {
        QSL("button-backforward"),
        QSL("button-reloadstop"),
        QSL("button-home"),
        QSL("locationbar"),
        QSL("button-downloads"),
        QSL("adblock-icon"),
        QSL("button-tools")
    };

    Settings settings;
    settings.beginGroup(QSL("NavigationBar"));
    m_layoutIds = settings.value(QSL("Layout"), defaultIds).toStringList();
    m_searchLine->setVisible(settings.value(QSL("ShowSearchBar"), true).toBool());
    settings.endGroup();

    m_layoutIds.removeDuplicates();
    m_layoutIds.removeAll(QString());
    if (!m_layoutIds.contains(QSL("locationbar"))) {
        m_layoutIds.append(QSL("locationbar"));
    }

    reloadLayout();
}

void NavigationBar::reloadLayout()
{
    if (m_widgets.isEmpty()) {
        return;
    }

    setUpdatesEnabled(false);

    // Clear layout
    while (m_layout->count() != 0) {
        QLayoutItem *item = m_layout->takeAt(0);
        if (!item) {
            continue;
        }
        QWidget *widget = item->widget();
        if (!widget) {
            continue;
        }
        widget->setParent(nullptr);
    }

    // Hide all widgets
    for (const WidgetData &data : m_widgets) {
        data.widget->hide();
    }

    // Add widgets to layout
    for (const QString &id : std::as_const(m_layoutIds)) {
        const WidgetData data = m_widgets.value(id);
        if (data.widget) {
            m_layout->addWidget(data.widget);
            auto *button = qobject_cast<NavigationBarToolButton*>(data.widget);
            if (button) {
                button->updateVisibility();
            } else {
                data.widget->show();
            }
        }
    }

    m_layout->addWidget(m_supMenu);

    // Make sure search bar is visible
    if (m_searchLine->isVisible() && m_navigationSplitter->sizes().at(1) == 0) {
        const int locationBarSize = m_navigationSplitter->sizes().at(0);
        setSplitterSizes(locationBarSize - 50, 50);
    }

    if (m_window->isFullScreen()) {
        enterFullScreen();
    } else {
        leaveFullScreen();
    }

    setUpdatesEnabled(true);
}

void NavigationBar::loadHistoryIndex()
{
    QWebEngineHistory* history = m_window->weView()->page()->history();

    if (auto* action = qobject_cast<QAction*>(sender())) {
        loadHistoryItem(history->itemAt(action->data().toInt()));
    }
}

void NavigationBar::loadHistoryIndexInNewTab(int index)
{
    if (auto* action = qobject_cast<QAction*>(sender())) {
        index = action->data().toInt();
    }

    if (index == -1) {
        return;
    }

    QWebEngineHistory* history = m_window->weView()->page()->history();
    loadHistoryItemInNewTab(history->itemAt(index));
}

void NavigationBar::stop()
{
    m_window->action(QSL("View/Stop"))->trigger();
}

void NavigationBar::reload()
{
    m_window->action(QSL("View/Reload"))->trigger();
}

void NavigationBar::goBack()
{
    auto view = m_window->weView();
    view->setFocus();
    view->back();
}

void NavigationBar::goBackInNewTab()
{
    QWebEngineHistory* history = m_window->weView()->page()->history();

    if (!history->canGoBack()) {
        return;
    }

    loadHistoryItemInNewTab(history->backItem());
}

void NavigationBar::goForward()
{
    auto view = m_window->weView();
    view->setFocus();
    view->forward();
}

void NavigationBar::goForwardInNewTab()
{
    QWebEngineHistory* history = m_window->weView()->page()->history();

    if (!history->canGoForward()) {
        return;
    }

    loadHistoryItemInNewTab(history->forwardItem());
}

void NavigationBar::loadHistoryItem(const QWebEngineHistoryItem &item)
{
    m_window->weView()->page()->history()->goToItem(item);
}

void NavigationBar::loadHistoryItemInNewTab(const QWebEngineHistoryItem &item)
{
    TabWidget* tabWidget = m_window->tabWidget();
    int tabIndex = tabWidget->duplicateTab(tabWidget->currentIndex());

    QWebEngineHistory* history = m_window->weView(tabIndex)->page()->history();
    history->goToItem(item);

    if (qzSettings->newTabPosition == Qz::NT_SelectedTab) {
        tabWidget->setCurrentIndex(tabIndex);
    }

}
