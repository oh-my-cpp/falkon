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
#include "browserwindow.h"
#include "tabwidget.h"
#include "tabbar.h"
#include "webpage.h"
#include "tabbedwebview.h"
#include "lineedit.h"
#include "history.h"
#include "locationbar.h"
#include "websearchbar.h"
#include "pluginproxy.h"
#include "sidebar.h"
#include "cookiejar.h"
#include "cookiemanager.h"
#include "bookmarkstoolbar.h"
#include "clearprivatedata.h"
#include "autofill.h"
#include "mainapplication.h"
#include "checkboxdialog.h"
#include "clickablelabel.h"
#include "docktitlebarwidget.h"
#include "iconprovider.h"
#include "progressbar.h"
#include "closedwindowsmanager.h"
#include "statusbar.h"
#include "browsinglibrary.h"
#include "navigationbar.h"
#include "bookmarksimport/bookmarksimportdialog.h"
#include "qztools.h"
#include "reloadstopbutton.h"
#include "enhancedmenu.h"
#include "navigationcontainer.h"
#include "settings.h"
#include "qzsettings.h"
#include "speeddial.h"
#include "menubar.h"
#include "bookmarkstools.h"
#include "bookmarksmenu.h"
#include "historymenu.h"
#include "mainmenu.h"
#include "downloadsbutton.h"
#include "tabmodel.h"
#include "tabmrumodel.h"

#include <algorithm>

#include <QKeyEvent>
#include <QSplitter>
#include <QMenuBar>
#include <QTimer>
#include <QShortcut>
#include <QStackedWidget>
#include <QFileDialog>
#include <QDesktopServices>
#include <QWebEngineHistory>
#include <QWebEngineSettings>
#include <QMessageBox>
#include <QToolTip>
#include <QScrollArea>
#include <QCollator>
#include <QTemporaryFile>
#include <QActionGroup>
#include <QStringConverter>

#ifdef QZ_WS_X11
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#endif

static const int savedWindowVersion = 2;

BrowserWindow::SavedWindow::SavedWindow()
= default;

BrowserWindow::SavedWindow::SavedWindow(BrowserWindow *window)
{
    windowState = window->isFullScreen() ? QByteArray() : window->saveState();
    windowGeometry = window->saveGeometry();
    windowUiState = window->saveUiState();
#ifdef QZ_WS_X11
    virtualDesktop = window->getCurrentVirtualDesktop();
#endif

    const int tabsCount = window->tabCount();
    tabs.reserve(tabsCount);
    for (int i = 0; i < tabsCount; ++i) {
        TabbedWebView *webView = window->weView(i);
        if (!webView) {
            continue;
        }
        WebTab* webTab = webView->webTab();
        if (!webTab) {
            continue;
        }
        WebTab::SavedTab tab(webTab);
        if (!tab.isValid()) {
            continue;
        }
        if (webTab->isCurrentTab()) {
            currentTab = tabs.size();
        }
        tabs.append(tab);
    }
}

bool BrowserWindow::SavedWindow::isValid() const
{
    for (const WebTab::SavedTab &tab : std::as_const(tabs)) {
        if (!tab.isValid()) {
            return false;
        }
    }
    return currentTab > -1;
}

void BrowserWindow::SavedWindow::clear()
{
    windowState.clear();
    windowGeometry.clear();
    virtualDesktop = -1;
    currentTab = -1;
    tabs.clear();
}

QDataStream &operator<<(QDataStream &stream, const BrowserWindow::SavedWindow &window)
{
    stream << savedWindowVersion;
    stream << window.windowState;
    stream << window.windowGeometry;
    stream << window.virtualDesktop;
    stream << window.currentTab;
    stream << static_cast<int>(window.tabs.count());

    for (int i = 0; i < window.tabs.count(); ++i) {
        stream << window.tabs.at(i);
    }

    stream << window.windowUiState;

    return stream;
}

QDataStream &operator>>(QDataStream &stream, BrowserWindow::SavedWindow &window)
{
    int version;
    stream >> version;

    if (version < 1) {
        return stream;
    }

    stream >> window.windowState;
    stream >> window.windowGeometry;
    stream >> window.virtualDesktop;
    stream >> window.currentTab;

    int tabsCount = -1;
    stream >> tabsCount;
    window.tabs.reserve(tabsCount);

    for (int i = 0; i < tabsCount; ++i) {
        WebTab::SavedTab tab;
        stream >> tab;
        window.tabs.append(tab);
    }

    if (version >= 2) {
        stream >> window.windowUiState;
    }

    return stream;
}

BrowserWindow::BrowserWindow(Qz::BrowserWindowType type, const QList<QUrl> startUrls)
    : QMainWindow(nullptr)
    , m_startUrls(startUrls)
    , m_windowType(type)
    , m_startTab(nullptr)
    , m_startPage(nullptr)
    , m_sideBarManager(new SideBarManager(this))
    , m_hideNavigationTimer(nullptr)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_DontCreateNativeAncestors);

    setObjectName(QSL("mainwindow"));
    setWindowTitle(tr("Falkon"));
    setProperty("private", mApp->isPrivate());

    setupUi();
    setupMenu();

    m_hideNavigationTimer = new QTimer(this);
    m_hideNavigationTimer->setInterval(1000);
    m_hideNavigationTimer->setSingleShot(true);
    connect(m_hideNavigationTimer, &QTimer::timeout, this, &BrowserWindow::hideNavigationSlot);

    connect(mApp, &MainApplication::settingsReloaded, this, &BrowserWindow::loadSettings);

    QTimer::singleShot(0, this, &BrowserWindow::postLaunch);

    if (mApp->isPrivate()) {
        QzTools::setWmClass(QSL("Falkon Browser (Private Window)"), this);
    }
    else {
        QzTools::setWmClass(QSL("Falkon Browser"), this);
    }
}

BrowserWindow::~BrowserWindow()
{
    mApp->plugins()->emitMainWindowDeleted(this);

    for (const QPointer<QWidget> &pointer : std::as_const(m_deleteOnCloseWidgets)) {
        if (pointer) {
            pointer->deleteLater();
        }
    }
}

void BrowserWindow::setStartTab(WebTab* tab)
{
    m_startTab = tab;
}

void BrowserWindow::setStartPage(WebPage *page)
{
    m_startPage = page;
}

void BrowserWindow::postLaunch()
{
    loadSettings();

    bool addTab = true;
    QUrl startUrl;

    switch (mApp->afterLaunch()) {
    case MainApplication::OpenBlankPage:
        startUrl = QUrl();
        break;

    case MainApplication::OpenSpeedDial:
        startUrl = QUrl(QSL("falkon:speeddial"));
        break;

    case MainApplication::OpenHomePage:
    case MainApplication::RestoreSession:
    case MainApplication::SelectSession:
        startUrl = m_homepage;
        break;

    default:
        break;
    }

    if (!mApp->isTestModeEnabled()) {
        show();
    }

    switch (m_windowType) {
    case Qz::BW_FirstAppWindow:
        if (mApp->isStartingAfterCrash()) {
            addTab = false;
            startUrl.clear();
            m_tabWidget->addView(QUrl(QSL("falkon:restore")), Qz::NT_CleanSelectedTabAtTheEnd);
        }
        else if (mApp->afterLaunch() == MainApplication::SelectSession || mApp->afterLaunch() == MainApplication::RestoreSession) {
            addTab = m_tabWidget->count() <= 0;
        }
        break;

    case Qz::BW_NewWindow:
    case Qz::BW_MacFirstWindow:
        addTab = true;
        break;

    case Qz::BW_OtherRestoredWindow:
        addTab = false;
        break;
    }

    if (!m_startUrls.isEmpty()) {
        startUrl = m_startUrls.takeFirst();
        addTab = true;
    }

    if (m_startTab) {
        addTab = false;
        m_tabWidget->addView(m_startTab, Qz::NT_SelectedTab);
    }

    if (m_startPage) {
        addTab = false;
        m_tabWidget->addView(QUrl());
        weView()->setPage(m_startPage);
    }

    if (addTab) {
        m_tabWidget->addView(startUrl, Qz::NT_CleanSelectedTabAtTheEnd);

        for(const QUrl &url : std::as_const(m_startUrls)) {
            m_tabWidget->addView(url, Qz::NT_NotSelectedTabAtTheEnd);
        }

        if (startUrl.isEmpty() || startUrl.toString() == QLatin1String("falkon:speeddial")) {
            locationBar()->setFocus();
        }
    }

    // Something went really wrong .. add one tab
    if (m_tabWidget->count() <= 0) {
        m_tabWidget->addView(m_homepage, Qz::NT_SelectedTabAtTheEnd);
    }

    mApp->plugins()->emitMainWindowCreated(this);
    Q_EMIT startingCompleted();

    raise();
    activateWindow();
    updateStartupFocus();
}

void BrowserWindow::setupUi()
{
    Settings settings;
    settings.beginGroup(QSL("Browser-View-Settings"));
    const QByteArray windowGeometry = settings.value(QSL("WindowGeometry")).toByteArray();

    const QStringList keys = {
        QSL("LocationBarWidth"),
        QSL("WebSearchBarWidth"),
        QSL("SideBarWidth"),
        QSL("WebViewWidth"),
        QSL("SideBar")
    };
    QHash<QString, QVariant> uiState;
    for (const QString &key : keys) {
        if (settings.contains(key)) {
            uiState[key] = settings.value(key);
        }
    }
    settings.endGroup();

    auto* widget = new QWidget(this);
    widget->setCursor(Qt::ArrowCursor);
    setCentralWidget(widget);

    m_mainLayout = new QVBoxLayout(widget);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);
    m_mainSplitter = new QSplitter(this);
    m_mainSplitter->setObjectName(QSL("sidebar-splitter"));
    m_tabWidget = new TabWidget(this);
    m_superMenu = new QMenu(this);
    m_navigationToolbar = new NavigationBar(this);
    m_bookmarksToolbar = new BookmarksToolbar(this);

    m_tabModel = new TabModel(this, this);
    m_tabMruModel = new TabMruModel(this, this);
    m_tabMruModel->setSourceModel(m_tabModel);

    m_navigationContainer = new NavigationContainer(this);
    m_navigationContainer->addWidget(m_navigationToolbar);
    m_navigationContainer->addWidget(m_bookmarksToolbar);
    m_navigationContainer->setTabBar(m_tabWidget->tabBar());

    m_mainSplitter->addWidget(m_tabWidget);
    m_mainSplitter->setCollapsible(0, false);

    m_mainLayout->addWidget(m_navigationContainer);
    m_mainLayout->addWidget(m_mainSplitter);

    m_statusBar = new StatusBar(this);
    m_statusBar->setObjectName(QSL("mainwindow-statusbar"));
    m_statusBar->setCursor(Qt::ArrowCursor);
    setStatusBar(m_statusBar);
    m_progressBar = new ProgressBar(m_statusBar);
    m_ipLabel = new QLabel(this);
    m_ipLabel->setObjectName(QSL("statusbar-ip-label"));
    m_ipLabel->setToolTip(tr("IP Address of current page"));

    m_statusBar->addPermanentWidget(m_progressBar);
    m_statusBar->addPermanentWidget(m_ipLabel);

    auto *downloadsButton = new DownloadsButton(this);
    m_statusBar->addButton(downloadsButton);
    m_navigationToolbar->addToolButton(downloadsButton);

    auto desktop = QGuiApplication::primaryScreen();
    int windowWidth = desktop->availableGeometry().width() / 1.3;
    int windowHeight = desktop->availableGeometry().height() / 1.3;

    // Let the WM decides where to put new browser window
    if (m_windowType != Qz::BW_FirstAppWindow && m_windowType != Qz::BW_MacFirstWindow && mApp->getWindow()) {
#ifdef Q_WS_WIN
        // Windows WM places every new window in the middle of screen .. for some reason
        QPoint p = mApp->getWindow()->geometry().topLeft();
        p.setX(p.x() + 30);
        p.setY(p.y() + 30);

        if (!desktop->availableGeometry(mApp->getWindow()).contains(p)) {
            p.setX(desktop->availableGeometry(mApp->getWindow()).x() + 30);
            p.setY(desktop->availableGeometry(mApp->getWindow()).y() + 30);
        }

        setGeometry(QRect(p, mApp->getWindow()->size()));
#else
        resize(mApp->getWindow()->size());
#endif
    } else if (!restoreGeometry(windowGeometry)) {
#ifdef Q_WS_WIN
        setGeometry(QRect(desktop->availableGeometry(mApp->getWindow()).x() + 30,
                          desktop->availableGeometry(mApp->getWindow()).y() + 30, windowWidth, windowHeight));
#else
        resize(windowWidth, windowHeight);
#endif
    }

    restoreUiState(uiState);

    // Set some sane minimum width
    setMinimumWidth(300);
}

void BrowserWindow::setupMenu()
{
#ifdef Q_OS_MACOS
    static MainMenu* macMainMenu = 0;

    if (!macMainMenu) {
        macMainMenu = new MainMenu(this, 0);
        macMainMenu->initMenuBar(new QMenuBar(0));
        connect(mApp, SIGNAL(activeWindowChanged(BrowserWindow*)), macMainMenu, SLOT(setWindow(BrowserWindow*)));
    }
    else {
        macMainMenu->setWindow(this);
    }

    m_mainMenu = macMainMenu;
#else
    setMenuBar(new MenuBar(this));

    m_mainMenu = new MainMenu(this, this);
    m_mainMenu->initMenuBar(menuBar());
#endif
    m_mainMenu->initSuperMenu(m_superMenu);

    // Setup other shortcuts
    auto* reloadBypassCacheAction = new QShortcut(QKeySequence(QSL("Ctrl+F5")), this);
    auto* reloadBypassCacheAction2 = new QShortcut(QKeySequence(QSL("Ctrl+Shift+R")), this);
    connect(reloadBypassCacheAction, &QShortcut::activated, this, &BrowserWindow::reloadBypassCache);
    connect(reloadBypassCacheAction2, &QShortcut::activated, this, &BrowserWindow::reloadBypassCache);

    auto* closeTabAction = new QShortcut(QKeySequence(QSL("Ctrl+W")), this);
    auto* closeTabAction2 = new QShortcut(QKeySequence(QSL("Ctrl+F4")), this);
    connect(closeTabAction, &QShortcut::activated, this, &BrowserWindow::closeTab);
    connect(closeTabAction2, &QShortcut::activated, this, &BrowserWindow::closeTab);

    auto* reloadAction = new QShortcut(QKeySequence(QSL("Ctrl+R")), this);
    connect(reloadAction, &QShortcut::activated, this, &BrowserWindow::reload);

    auto* openLocationAction = new QShortcut(QKeySequence(QSL("Alt+D")), this);
    connect(openLocationAction, &QShortcut::activated, this, &BrowserWindow::openLocation);

    auto* inspectorAction = new QShortcut(QKeySequence(QSL("F12")), this);
    connect(inspectorAction, &QShortcut::activated, this, &BrowserWindow::toggleWebInspector);

    auto* restoreClosedWindow = new QShortcut(QKeySequence(QSL("Ctrl+Shift+N")), this);
    connect(restoreClosedWindow, &QShortcut::activated, mApp->closedWindowsManager(), &ClosedWindowsManager::restoreClosedWindow);
}

void BrowserWindow::updateStartupFocus()
{
    QTimer::singleShot(500, this, [this]() {
        // Scroll to current tab
        tabWidget()->tabBar()->ensureVisible();
        // Update focus
        if (!m_startPage && LocationBar::convertUrlToText(weView()->page()->requestedUrl()).isEmpty())
            locationBar()->setFocus();
        else
            weView()->setFocus();
    });
}

QAction* BrowserWindow::createEncodingAction(const QString &codecName,
                                             const QString &activeCodecName, QMenu* menu)
{
    auto* action = new QAction(codecName, menu);
    action->setData(codecName);
    action->setCheckable(true);
    connect(action, &QAction::triggered, this, &BrowserWindow::changeEncoding);
    if (activeCodecName.compare(codecName, Qt::CaseInsensitive) == 0) {
        action->setChecked(true);
    }
    return action;
}

void BrowserWindow::createEncodingSubMenu(const QString &name, QStringList &codecNames, QMenu* menu)
{
    if (codecNames.isEmpty()) {
        return;
    }

    QCollator collator;
    collator.setNumericMode(true);
    std::sort(codecNames.begin(), codecNames.end(), [collator](const QString &a, const QString &b) {
        return collator.compare(a, b) < 0;
    });

    auto* subMenu = new QMenu(name, menu);
    const QString activeCodecName = mApp->webSettings()->defaultTextEncoding();

    auto *group = new QActionGroup(subMenu);

    for (const QString &codecName : std::as_const(codecNames)) {
        QAction *act = createEncodingAction(codecName, activeCodecName, subMenu);
        group->addAction(act);
        subMenu->addAction(act);
    }

    menu->addMenu(subMenu);
}

QHash<QString, QVariant> BrowserWindow::saveUiState()
{
    saveSideBarSettings();

    QHash<QString, QVariant> state;
    state[QSL("LocationBarWidth")] = m_navigationToolbar->splitter()->sizes().at(0);
    state[QSL("WebSearchBarWidth")] = m_navigationToolbar->splitter()->sizes().at(1);
    state[QSL("SideBarWidth")] = m_sideBarWidth;
    state[QSL("WebViewWidth")] = m_webViewWidth;
    state[QSL("SideBar")] = m_sideBarManager->activeSideBar();
    return state;
}

void BrowserWindow::restoreUiState(const QHash<QString, QVariant> &state)
{
    const int locationBarWidth = state.value(QSL("LocationBarWidth"), 480).toInt();
    const int websearchBarWidth = state.value(QSL("WebSearchBarWidth"), 140).toInt();
    m_navigationToolbar->setSplitterSizes(locationBarWidth, websearchBarWidth);

    m_sideBarWidth = state.value(QSL("SideBarWidth"), 250).toInt();
    m_webViewWidth = state.value(QSL("WebViewWidth"), 2000).toInt();
    if (m_sideBar) {
        m_mainSplitter->setSizes({m_sideBarWidth, m_webViewWidth});
    }

    const QString activeSideBar = state.value(QSL("SideBar")).toString();
    if (activeSideBar.isEmpty() && m_sideBar) {
        m_sideBar->close();
    } else {
        m_sideBarManager->showSideBar(activeSideBar, false);
    }
}

void BrowserWindow::loadSettings()
{
    Settings settings;

    //Url settings
    settings.beginGroup(QSL("Web-URL-Settings"));
    m_homepage = settings.value(QSL("homepage"), QSL("falkon:start")).toUrl();
    settings.endGroup();

    //Browser Window settings
    settings.beginGroup(QSL("Browser-View-Settings"));
    bool showStatusBar = settings.value(QSL("showStatusBar"), false).toBool();
    bool showBookmarksToolbar = settings.value(QSL("showBookmarksToolbar"), false).toBool();
    bool showNavigationToolbar = settings.value(QSL("showNavigationToolbar"), true).toBool();
    bool showMenuBar = settings.value(QSL("showMenubar"), false).toBool();

    // Make sure both menubar and navigationbar are not hidden
    // Fixes #781
    if (!showNavigationToolbar) {
        showMenuBar = true;
        settings.setValue(QSL("showMenubar"), true);
    }

    settings.endGroup();

    settings.beginGroup(QSL("Shortcuts"));
    m_useTabNumberShortcuts = settings.value(QSL("useTabNumberShortcuts"), true).toBool();
    m_useSpeedDialNumberShortcuts = settings.value(QSL("useSpeedDialNumberShortcuts"), true).toBool();
    m_useSingleKeyShortcuts = settings.value(QSL("useSingleKeyShortcuts"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QSL("Web-Browser-Settings"));
    QAction *quitAction = m_mainMenu->action(QSL("Standard/Quit"));
    if (settings.value(QSL("closeAppWithCtrlQ"), true).toBool()) {
        quitAction->setShortcut(QzTools::actionShortcut(QKeySequence::Quit, QKeySequence(QSL("Ctrl+Q"))));
    } else {
        quitAction->setShortcut(QKeySequence());
    }
    settings.endGroup();

    m_statusBarVisible = showStatusBar;
    statusBar()->setVisible(!isFullScreen() && showStatusBar);
    m_bookmarksToolbar->setVisible(showBookmarksToolbar);
    m_navigationToolbar->setVisible(showNavigationToolbar);

#ifndef Q_OS_MACOS
    m_menuBarVisible = showMenuBar;
    menuBar()->setVisible(!isFullScreen() && showMenuBar);
#endif

    m_navigationToolbar->setSuperMenuVisible(isFullScreen() || !showMenuBar);
}

void BrowserWindow::goForward()
{
    weView()->forward();
}

void BrowserWindow::reload()
{
    weView()->reload();
}

void BrowserWindow::reloadBypassCache()
{
    weView()->reloadBypassCache();
}

void BrowserWindow::goBack()
{
    weView()->back();
}

int BrowserWindow::tabCount() const
{
    return m_tabWidget->count();
}

TabbedWebView* BrowserWindow::weView() const
{
    return weView(m_tabWidget->currentIndex());
}

TabbedWebView* BrowserWindow::weView(int index) const
{
    auto* webTab = qobject_cast<WebTab*>(m_tabWidget->widget(index));
    if (!webTab) {
        return nullptr;
    }

    return webTab->webView();
}

LocationBar* BrowserWindow::locationBar() const
{
    return qobject_cast<LocationBar*>(m_tabWidget->locationBars()->currentWidget());
}

TabWidget* BrowserWindow::tabWidget() const
{
    return m_tabWidget;
}

BookmarksToolbar* BrowserWindow::bookmarksToolbar() const
{
    return m_bookmarksToolbar;
}

StatusBar* BrowserWindow::statusBar() const
{
    return m_statusBar;
}

NavigationBar* BrowserWindow::navigationBar() const
{
    return m_navigationToolbar;
}

SideBarManager* BrowserWindow::sideBarManager() const
{
    return m_sideBarManager;
}

QLabel* BrowserWindow::ipLabel() const
{
    return m_ipLabel;
}

QMenu* BrowserWindow::superMenu() const
{
    return m_superMenu;
}

QUrl BrowserWindow::homepageUrl() const
{
    return m_homepage;
}

Qz::BrowserWindowType BrowserWindow::windowType() const
{
    return m_windowType;
}

QAction* BrowserWindow::action(const QString &name) const
{
    return m_mainMenu->action(name);
}

TabModel *BrowserWindow::tabModel() const
{
    return m_tabModel;
}

TabMruModel *BrowserWindow::tabMruModel() const
{
    return m_tabMruModel;
}

bool BrowserWindow::useTabNumberShortcuts() const
{
    return m_useTabNumberShortcuts;
}

void BrowserWindow::setWindowTitle(const QString &t)
{
    QString title = t;

    if (mApp->isPrivate()) {
        title.append(tr(" (Private Browsing)"));
    }

    QMainWindow::setWindowTitle(title);
}

void BrowserWindow::changeEncoding()
{
    if (auto* action = qobject_cast<QAction*>(sender())) {
        const QString encoding = action->data().toString();
        mApp->webSettings()->setDefaultTextEncoding(encoding);

        Settings settings;
        settings.setValue(QSL("Web-Browser-Settings/DefaultEncoding"), encoding);

        weView()->reload();
    }
}

void BrowserWindow::printPage()
{
    weView()->printPage();
}

void BrowserWindow::bookmarkPage()
{
    TabbedWebView* view = weView();
    BookmarksTools::addBookmarkDialog(this, view->url(), view->title());
}

void BrowserWindow::bookmarkAllTabs()
{
    BookmarksTools::bookmarkAllTabsDialog(this, m_tabWidget);
}

void BrowserWindow::addBookmark(const QUrl &url, const QString &title)
{
    BookmarksTools::addBookmarkDialog(this, url, title);
}

void BrowserWindow::goHome()
{
    loadAddress(m_homepage);
}

void BrowserWindow::goHomeInNewTab()
{
    m_tabWidget->addView(m_homepage, Qz::NT_SelectedTab);
}

void BrowserWindow::loadActionUrl(QObject* obj)
{
    if (!obj) {
        obj = sender();
    }

    if (auto* action = qobject_cast<QAction*>(obj)) {
        loadAddress(action->data().toUrl());
    }
}

void BrowserWindow::loadActionUrlInNewTab(QObject* obj)
{
    if (!obj) {
        obj = sender();
    }

    if (auto* action = qobject_cast<QAction*>(obj)) {
        m_tabWidget->addView(action->data().toUrl(), Qz::NT_SelectedTabAtTheEnd);
    }
}

void BrowserWindow::loadAddress(const QUrl &url)
{
    if (weView()->webTab()->isPinned()) {
        int index = m_tabWidget->addView(url, qzSettings->newTabPosition);
        weView(index)->setFocus();
    } else {
        weView()->setFocus();
        weView()->load(LoadRequest(url));
    }
}

void BrowserWindow::showHistoryManager()
{
    mApp->browsingLibrary()->showHistory(this);
}

void BrowserWindow::showSource(WebView *view)
{
    if (!view)
        view = weView();
    view->showSource();
}

SideBar* BrowserWindow::addSideBar()
{
    if (m_sideBar) {
        return m_sideBar.data();
    }

    m_sideBar = new SideBar(m_sideBarManager, this);

    m_mainSplitter->insertWidget(0, m_sideBar.data());
    m_mainSplitter->setCollapsible(0, false);
    m_mainSplitter->setSizes({m_sideBarWidth, m_webViewWidth});

    return m_sideBar.data();
}

void BrowserWindow::saveSideBarSettings()
{
    if (m_sideBar) {
        // That +1 is important here, without it, the sidebar width would
        // decrease by 1 pixel every close
        m_sideBarWidth = m_mainSplitter->sizes().at(0) + 1;
        m_webViewWidth = width() - m_sideBarWidth;
    }

    Settings().setValue(QSL("Browser-View-Settings/SideBar"), m_sideBarManager->activeSideBar());
}

void BrowserWindow::toggleShowMenubar()
{
#ifdef Q_OS_MACOS
    // We use one shared global menubar on Mac that can't be hidden
    return;
#endif

    setUpdatesEnabled(false);

    menuBar()->setVisible(!menuBar()->isVisible());
    m_navigationToolbar->setSuperMenuVisible(!menuBar()->isVisible());

    setUpdatesEnabled(true);

    Settings().setValue(QSL("Browser-View-Settings/showMenubar"), menuBar()->isVisible());

    // Make sure we show Navigation Toolbar when Menu Bar is hidden
    if (!m_navigationToolbar->isVisible() && !menuBar()->isVisible()) {
        toggleShowNavigationToolbar();
    }
}

void BrowserWindow::toggleShowStatusBar()
{
    setUpdatesEnabled(false);

    m_statusBar->setVisible(!m_statusBar->isVisible());

    setUpdatesEnabled(true);

    Settings().setValue(QSL("Browser-View-Settings/showStatusBar"), m_statusBar->isVisible());

}

void BrowserWindow::toggleShowBookmarksToolbar()
{
    setUpdatesEnabled(false);

    m_bookmarksToolbar->setVisible(!m_bookmarksToolbar->isVisible());

    setUpdatesEnabled(true);

    Settings().setValue(QSL("Browser-View-Settings/showBookmarksToolbar"), m_bookmarksToolbar->isVisible());
    Settings().setValue(QSL("Browser-View-Settings/instantBookmarksToolbar"), false);
}

void BrowserWindow::toggleShowNavigationToolbar()
{
    setUpdatesEnabled(false);

    m_navigationToolbar->setVisible(!m_navigationToolbar->isVisible());

    setUpdatesEnabled(true);

    Settings().setValue(QSL("Browser-View-Settings/showNavigationToolbar"), m_navigationToolbar->isVisible());

#ifndef Q_OS_MACOS
    // Make sure we show Menu Bar when Navigation Toolbar is hidden
    if (!m_navigationToolbar->isVisible() && !menuBar()->isVisible()) {
        toggleShowMenubar();
    }
#endif
}

void BrowserWindow::toggleTabsOnTop(bool enable)
{
    qzSettings->tabsOnTop = enable;
    m_navigationContainer->toggleTabsOnTop(enable);
}

void BrowserWindow::toggleFullScreen()
{
    if (m_htmlFullScreenView) {
        weView()->triggerPageAction(QWebEnginePage::ExitFullScreen);
        return;
    }

    if (isFullScreen()) {
        setWindowState(windowState() & ~Qt::WindowFullScreen);
    } else {
        setWindowState(windowState() | Qt::WindowFullScreen);
    }
}

void BrowserWindow::requestHtmlFullScreen(TabbedWebView *view, bool enable)
{
    if (enable) {
        setWindowState(windowState() | Qt::WindowFullScreen);
    } else {
        setWindowState(windowState() & ~Qt::WindowFullScreen);
    }

    if (m_sideBar)
        m_sideBar.data()->setHidden(enable);

    m_htmlFullScreenView = enable ? view : nullptr;
}

void BrowserWindow::showWebInspector()
{
    if (weView() && weView()->webTab()) {
        weView()->webTab()->showWebInspector();
    }
}

void BrowserWindow::toggleWebInspector()
{
    if (weView() && weView()->webTab()) {
        weView()->webTab()->toggleWebInspector();
    }
}

void BrowserWindow::currentTabChanged()
{
    TabbedWebView* view = weView();
    m_navigationToolbar->setCurrentView(view);

    if (!view) {
        return;
    }

    const QString title = view->webTab()->title(/*allowEmpty*/true);
    if (title.isEmpty()) {
        setWindowTitle(tr("Falkon"));
    } else {
        setWindowTitle(tr("%1 - Falkon").arg(title));
    }
    m_ipLabel->setText(view->getIp());
    view->setFocus();

    updateLoadingActions();

    // Setting correct tab order (LocationBar -> WebSearchBar -> WebView)
    setTabOrder(locationBar(), m_navigationToolbar->webSearchBar());
    setTabOrder(m_navigationToolbar->webSearchBar(), view);
}

void BrowserWindow::updateLoadingActions()
{
    TabbedWebView* view = weView();
    if (!view) {
        return;
    }

    bool isLoading = view->isLoading();

    m_ipLabel->setVisible(!isLoading);
    m_progressBar->setVisible(isLoading);

    action(QSL("View/Stop"))->setEnabled(isLoading);
    action(QSL("View/Reload"))->setEnabled(!isLoading);

    if (isLoading) {
        m_progressBar->setValue(view->loadingProgress());
        m_navigationToolbar->showStopButton();
    }
    else {
        m_navigationToolbar->showReloadButton();
    }
}

void BrowserWindow::addDeleteOnCloseWidget(QWidget* widget)
{
    if (!m_deleteOnCloseWidgets.contains(widget)) {
        m_deleteOnCloseWidgets.append(widget);
    }
}

void BrowserWindow::restoreWindow(const SavedWindow &window)
{
    restoreState(window.windowState);
    restoreGeometry(window.windowGeometry);
    restoreUiState(window.windowUiState);
#ifdef QZ_WS_X11
    moveToVirtualDesktop(window.virtualDesktop);
#endif
    if (!mApp->isTestModeEnabled()) {
        show(); // Window has to be visible before adding QWebEngineView's
    }
    m_tabWidget->restoreState(window.tabs, window.currentTab);
    updateStartupFocus();
}

void BrowserWindow::createToolbarsMenu(QMenu* menu)
{
    removeActions(menu->actions());
    menu->clear();

    QAction* action;

#ifndef Q_OS_MACOS
    action = menu->addAction(tr("&Menu Bar"), this, &BrowserWindow::toggleShowMenubar);
    action->setCheckable(true);
    action->setChecked(menuBar()->isVisible());
#endif

    action = menu->addAction(tr("&Navigation Toolbar"), this, &BrowserWindow::toggleShowNavigationToolbar);
    action->setCheckable(true);
    action->setChecked(m_navigationToolbar->isVisible());

    action = menu->addAction(tr("&Bookmarks Toolbar"), this, &BrowserWindow::toggleShowBookmarksToolbar);
    action->setCheckable(true);
    action->setChecked(Settings().value(QSL("Browser-View-Settings/showBookmarksToolbar")).toBool());

    menu->addSeparator();

    action = menu->addAction(tr("&Tabs on Top"), this, SLOT(toggleTabsOnTop(bool)));
    action->setCheckable(true);
    action->setChecked(qzSettings->tabsOnTop);

    addActions(menu->actions());
}

void BrowserWindow::createSidebarsMenu(QMenu* menu)
{
    m_sideBarManager->createMenu(menu);
}

void BrowserWindow::createEncodingMenu(QMenu* menu)
{
    const QString activeCodecName = mApp->webSettings()->defaultTextEncoding();

    QStringList isoCodecs;
    QStringList utfCodecs;
    QStringList windowsCodecs;
    QStringList isciiCodecs;
    QStringList ibmCodecs;
    QStringList otherCodecs;
    QStringList allCodecs;

    const auto codecs = QStringConverter::availableCodecs();
    for (const auto &codecName : codecs) {
        if (!allCodecs.contains(codecName))
            allCodecs.append(codecName);
        else
            continue;

        if (codecName.startsWith(QLatin1String("ISO"), Qt::CaseInsensitive))
            isoCodecs.append(codecName);
        else if (codecName.startsWith(QLatin1String("UTF"), Qt::CaseInsensitive))
            utfCodecs.append(codecName);
        else if (codecName.startsWith(QLatin1String("windows"), Qt::CaseInsensitive))
            windowsCodecs.append(codecName);
        else if (codecName.startsWith(QLatin1String("Iscii"), Qt::CaseInsensitive))
            isciiCodecs.append(codecName);
        else if (codecName.startsWith(QLatin1String("IBM"), Qt::CaseInsensitive))
            ibmCodecs.append(codecName);
        else
            otherCodecs.append(codecName);
    }

    if (!menu->isEmpty())
        menu->addSeparator();

    createEncodingSubMenu(QSL("ISO"), isoCodecs, menu);
    createEncodingSubMenu(QSL("UTF"), utfCodecs, menu);
    createEncodingSubMenu(QSL("Windows"), windowsCodecs, menu);
    createEncodingSubMenu(QSL("Iscii"), isciiCodecs, menu);
    createEncodingSubMenu(QSL("IBM"), ibmCodecs, menu);
    createEncodingSubMenu(tr("Other"), otherCodecs, menu);
}

void BrowserWindow::removeActions(const QList<QAction *> &actions)
{
    for (QAction *action : actions) {
        removeAction(action);
    }
}

void BrowserWindow::addTab()
{
    m_tabWidget->addView(QUrl(), Qz::NT_SelectedNewEmptyTab, true);
    m_tabWidget->setCurrentTabFresh(true);

    if (isFullScreen())
        showNavigationWithFullScreen();
}

void BrowserWindow::webSearch()
{
    m_navigationToolbar->webSearchBar()->setFocus();
    m_navigationToolbar->webSearchBar()->selectAll();
}

void BrowserWindow::searchOnPage()
{
    if (weView() && weView()->webTab()) {
        const QString searchText = weView()->page()->selectedText();
        if (!searchText.contains(QL1C('\n'))) {
            weView()->webTab()->showSearchToolBar(searchText);
        } else {
            weView()->webTab()->showSearchToolBar();
        }
    }
}

void BrowserWindow::openFile()
{
    const QString fileTypes = QSL("%1(*.html *.htm *.shtml *.shtm *.xhtml);;"
                                      "%2(*.png *.jpg *.jpeg *.bmp *.gif *.svg *.tiff);;"
                                      "%3(*.txt);;"
                                      "%4(*.*)").arg(tr("HTML files"), tr("Image files"), tr("Text files"), tr("All files"));

    const QString filePath = QzTools::getOpenFileName(QSL("MainWindow-openFile"), this, tr("Open file..."), QDir::homePath(), fileTypes);

    if (!filePath.isEmpty()) {
        loadAddress(QUrl::fromLocalFile(filePath));
    }
}

void BrowserWindow::openLocation()
{
    if (isFullScreen()) {
        showNavigationWithFullScreen();
    }

    locationBar()->setFocus();
    locationBar()->selectAll();
}

bool BrowserWindow::fullScreenNavigationVisible() const
{
    return m_navigationContainer->isVisible();
}

void BrowserWindow::showNavigationWithFullScreen()
{
    if (m_htmlFullScreenView)
        return;

    if (m_hideNavigationTimer->isActive()) {
        m_hideNavigationTimer->stop();
    }

    m_navigationContainer->show();
}

void BrowserWindow::hideNavigationWithFullScreen()
{
    if (m_tabWidget->isCurrentTabFresh())
        return;

    if (!m_hideNavigationTimer->isActive()) {
        m_hideNavigationTimer->start();
    }
}

void BrowserWindow::hideNavigationSlot()
{
    TabbedWebView* view = weView();
    bool mouseInView = view && view->underMouse();

    if (isFullScreen() && mouseInView) {
        m_navigationContainer->hide();
    }
}

bool BrowserWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        auto *e = static_cast<QWindowStateChangeEvent*>(event);
        if (!(e->oldState() & Qt::WindowFullScreen) && windowState() & Qt::WindowFullScreen) {
            // Enter fullscreen
            m_statusBarVisible = m_statusBar->isVisible();
#ifndef Q_OS_MACOS
            m_menuBarVisible = menuBar()->isVisible();
            menuBar()->hide();
#endif
            m_statusBar->hide();

            m_navigationContainer->hide();
            m_navigationToolbar->enterFullScreen();

            // Show main menu button since menubar is hidden
            m_navigationToolbar->setSuperMenuVisible(true);
        }
        else if (e->oldState() & Qt::WindowFullScreen && !(windowState() & Qt::WindowFullScreen)) {
            // Leave fullscreen
            m_statusBar->setVisible(m_statusBarVisible);
#ifndef Q_OS_MACOS
            menuBar()->setVisible(m_menuBarVisible);
#endif

            m_navigationContainer->show();
            m_navigationToolbar->setSuperMenuVisible(!m_menuBarVisible);
            m_navigationToolbar->leaveFullScreen();
            m_htmlFullScreenView = nullptr;
        }

        if (m_hideNavigationTimer) {
            m_hideNavigationTimer->stop();
        }
    }

    return QMainWindow::event(event);
}

void BrowserWindow::resizeEvent(QResizeEvent* event)
{
    m_bookmarksToolbar->setMaximumWidth(width());

    QMainWindow::resizeEvent(event);
}

void BrowserWindow::keyPressEvent(QKeyEvent* event)
{
    if (mApp->plugins()->processKeyPress(Qz::ON_BrowserWindow, this, event)) {
        return;
    }

    int number = -1;
    TabbedWebView* view = weView();

    switch (event->key()) {
    case Qt::Key_Back:
        if (view) {
            view->back();
            event->accept();
        }
        break;

    case Qt::Key_Forward:
        if (view) {
            view->forward();
            event->accept();
        }
        break;

    case Qt::Key_Stop:
        if (view) {
            view->stop();
            event->accept();
        }
        break;

    case Qt::Key_Reload:
    case Qt::Key_Refresh:
        if (view) {
            view->reload();
            event->accept();
        }
        break;

    case Qt::Key_HomePage:
        goHome();
        event->accept();
        break;

    case Qt::Key_Favorites:
        mApp->browsingLibrary()->showBookmarks(this);
        event->accept();
        break;

    case Qt::Key_Search:
        searchOnPage();
        event->accept();
        break;

    case Qt::Key_F6:
    case Qt::Key_OpenUrl:
        openLocation();
        event->accept();
        break;

    case Qt::Key_History:
        showHistoryManager();
        event->accept();
        break;

    case Qt::Key_AddFavorite:
        bookmarkPage();
        event->accept();
        break;

    case Qt::Key_Tools:
        action(QSL("Standard/Preferences"))->trigger();
        event->accept();
        break;

    case Qt::Key_Tab:
        if (event->modifiers() == Qt::ControlModifier) {
            static_cast<QObject*>(m_tabWidget)->event(event);
        }
        break;

    case Qt::Key_Backtab:
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            static_cast<QObject*>(m_tabWidget)->event(event);
        }
        break;

    case Qt::Key_PageDown:
        if (event->modifiers() == Qt::ControlModifier) {
            m_tabWidget->nextTab();
            event->accept();
        }
        break;

    case Qt::Key_PageUp:
        if (event->modifiers() == Qt::ControlModifier) {
            m_tabWidget->previousTab();
            event->accept();
        }
        break;

    case Qt::Key_Equal:
        if (view && event->modifiers() == Qt::ControlModifier) {
            view->zoomIn();
            event->accept();
        }
        break;

    case Qt::Key_I:
        if (event->modifiers() == Qt::ControlModifier) {
            action(QSL("Tools/SiteInfo"))->trigger();
            event->accept();
        }
        break;

    case Qt::Key_U:
        if (event->modifiers() == Qt::ControlModifier) {
            action(QSL("View/PageSource"))->trigger();
            event->accept();
        }
        break;

    case Qt::Key_F:
        if (event->modifiers() == Qt::ControlModifier) {
            action(QSL("Edit/Find"))->trigger();
            event->accept();
        }
        break;

    case Qt::Key_Slash:
        if (m_useSingleKeyShortcuts) {
            action(QSL("Edit/Find"))->trigger();
            event->accept();
        }
        break;

    case Qt::Key_1:
        number = 1;
        break;
    case Qt::Key_2:
        number = 2;
        break;
    case Qt::Key_3:
        number = 3;
        break;
    case Qt::Key_4:
        number = 4;
        break;
    case Qt::Key_5:
        number = 5;
        break;
    case Qt::Key_6:
        number = 6;
        break;
    case Qt::Key_7:
        number = 7;
        break;
    case Qt::Key_8:
        number = 8;
        break;
    case Qt::Key_9:
        number = 9;
        break;

    default:
        break;
    }

    if (number != -1) {
        if (event->modifiers() & Qt::AltModifier && m_useTabNumberShortcuts) {
            if (number == 9) {
                number = m_tabWidget->count();
            }
            m_tabWidget->setCurrentIndex(number - 1);
            event->accept();
            return;
        }
        if (event->modifiers() & Qt::ControlModifier && m_useSpeedDialNumberShortcuts) {
            const QUrl url = mApp->plugins()->speedDial()->urlForShortcut(number - 1);
            if (url.isValid()) {
                loadAddress(url);
                event->accept();
                return;
            }
        }
        if (event->modifiers() == Qt::NoModifier && m_useSingleKeyShortcuts) {
            if (number == 1)
                m_tabWidget->previousTab();
            if (number == 2)
                m_tabWidget->nextTab();
        }
    }

    QMainWindow::keyPressEvent(event);
}

void BrowserWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (mApp->plugins()->processKeyRelease(Qz::ON_BrowserWindow, this, event)) {
        return;
    }

    switch (event->key()) {
    case Qt::Key_F:
        if (event->modifiers() == Qt::ControlModifier) {
            action(QSL("Edit/Find"))->trigger();
            event->accept();
        }
        break;

    default:
        break;
    }

    QMainWindow::keyReleaseEvent(event);
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
    if (mApp->isClosing()) {
        saveSettings();
        return;
    }

    Settings settings;
    bool askOnClose = settings.value(QSL("Browser-Tabs-Settings/AskOnClosing"), true).toBool();

    if ((mApp->afterLaunch() == MainApplication::SelectSession || mApp->afterLaunch() == MainApplication::RestoreSession) && mApp->windowCount() == 1) {
        askOnClose = false;
    }

    if (askOnClose && m_tabWidget->normalTabsCount() > 1) {
        CheckBoxDialog dialog(QMessageBox::Yes | QMessageBox::No, this);
        dialog.setDefaultButton(QMessageBox::No);
        //~ singular There is still %n open tab and your session won't be stored.\nAre you sure you want to close this window?
        //~ plural There are still %n open tabs and your session won't be stored.\nAre you sure you want to close this window?
        dialog.setText(tr("There are still %n open tabs and your session won't be stored.\nAre you sure you want to close this window?", "", m_tabWidget->count()));
        dialog.setCheckBoxText(tr("Don't ask again"));
        dialog.setWindowTitle(tr("There are still open tabs"));
        dialog.setIcon(QMessageBox::Warning);

        if (dialog.exec() != QMessageBox::Yes) {
            event->ignore();
            return;
        }

        if (dialog.isChecked()) {
            settings.setValue(QSL("Browser-Tabs-Settings/AskOnClosing"), false);
        }
    }

    Q_EMIT aboutToClose();

    saveSettings();
    mApp->closedWindowsManager()->saveWindow(this);

    #ifndef Q_OS_MACOS
        if (mApp->windowCount() == 1)
            mApp->quitApplication();
    #endif

    event->accept();
}

void BrowserWindow::closeWindow()
{
#ifdef Q_OS_MACOS
    close();
    return;
#endif

    if (mApp->windowCount() > 1) {
        close();
    }
}

void BrowserWindow::saveSettings()
{
    if (mApp->isPrivate()) {
        return;
    }

    Settings settings;
    settings.beginGroup(QSL("Browser-View-Settings"));
    settings.setValue(QSL("WindowGeometry"), saveGeometry());

    const auto state = saveUiState();
    for (auto it = state.constBegin(); it != state.constEnd(); ++it) {
        settings.setValue(it.key(), it.value());
    }

    settings.endGroup();
}

void BrowserWindow::closeTab()
{
    // Don't close pinned tabs with keyboard shortcuts (Ctrl+W, Ctrl+F4)
    if (weView() && !weView()->webTab()->isPinned()) {
        m_tabWidget->requestCloseTab();
    }
}

#ifdef QZ_WS_X11
static xcb_connection_t *getXcbConnection()
{
    const QNativeInterface::QX11Application *x11App = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (x11App == nullptr)
        return nullptr;
    return x11App->connection();
}

int BrowserWindow::getCurrentVirtualDesktop() const
{
    if (QGuiApplication::platformName() != QL1S("xcb"))
        return 0;

    xcb_connection_t *connection = getXcbConnection();
    if (connection == nullptr)
        return 0;

    xcb_intern_atom_cookie_t intern_atom;
    xcb_intern_atom_reply_t *atom_reply = nullptr;
    xcb_atom_t atom;
    xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t *reply = nullptr;
    uint32_t value;

    intern_atom = xcb_intern_atom(connection, false, qstrlen("_NET_WM_DESKTOP"), "_NET_WM_DESKTOP");
    atom_reply = xcb_intern_atom_reply(connection, intern_atom, nullptr);

    if (!atom_reply)
        goto error;

    atom = atom_reply->atom;

    cookie = xcb_get_property(connection, false, winId(), atom, XCB_ATOM_CARDINAL, 0, 1);
    reply = xcb_get_property_reply(connection, cookie, nullptr);

    if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->value_len != 1 || reply->format != sizeof(uint32_t) * 8)
        goto error;

    value = *reinterpret_cast<uint32_t*>(xcb_get_property_value(reply));

    free(reply);
    free(atom_reply);
    return value;

error:
    free(reply);
    free(atom_reply);
    return 0;
}

void BrowserWindow::moveToVirtualDesktop(int desktopId)
{
    if (QGuiApplication::platformName() != QL1S("xcb"))
        return;

    // Don't move when window is already visible or it is first app window
    if (desktopId < 0 || isVisible() || m_windowType == Qz::BW_FirstAppWindow)
        return;

    xcb_connection_t *connection = getXcbConnection();
    if (connection == nullptr)
        return;

    xcb_intern_atom_cookie_t intern_atom;
    xcb_intern_atom_reply_t *atom_reply = nullptr;
    xcb_atom_t atom;

    intern_atom = xcb_intern_atom(connection, false, qstrlen("_NET_WM_DESKTOP"), "_NET_WM_DESKTOP");
    atom_reply = xcb_intern_atom_reply(connection, intern_atom, nullptr);

    if (!atom_reply)
        goto error;

    atom = atom_reply->atom;

    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, winId(), atom,
                        XCB_ATOM_CARDINAL, 32, 1, (const void*) &desktopId);

error:
    free(atom_reply);
}
#endif
