/* ============================================================
* Falkon - Qt web browser
* Copyright (C) 2010-2014  David Rosca <nowrep@gmail.com>
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
#include "historysidebar.h"
#include "ui_historysidebar.h"
#include "browserwindow.h"
#include "tabwidget.h"
#include "tabbedwebview.h"
#include "mainapplication.h"
#include "qzsettings.h"
#include "iconprovider.h"
#include "historymodel.h"
#include "statusbar.h"

#include <QItemSelectionModel>

HistorySideBar::HistorySideBar(BrowserWindow* window, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::HistorySideBar)
    , m_window(window)
{
    ui->setupUi(this);
    ui->historyTree->setViewType(HistoryTreeView::HistorySidebarViewType);

    if (mApp->isPrivate()) {
        ui->search->setInputMethodHints(Qt::InputMethodHint::ImhSensitiveData);
    }

    connect(ui->historyTree, &HistoryTreeView::urlActivated, this, &HistorySideBar::urlActivated);
    connect(ui->historyTree, &HistoryTreeView::urlCtrlActivated, this, &HistorySideBar::urlCtrlActivated);
    connect(ui->historyTree, &HistoryTreeView::urlShiftActivated, this, &HistorySideBar::urlShiftActivated);
    connect(ui->historyTree, &HistoryTreeView::contextMenuRequested, this, &HistorySideBar::createContextMenu);

    connect(ui->historyTree, &QTreeView::entered, this, &HistorySideBar::showSidebarHint);
    connect(ui->historyTree->selectionModel(), &QItemSelectionModel::currentChanged, this, &HistorySideBar::onCurrentChanged);

    connect(ui->search, &QLineEdit::textEdited, ui->historyTree, &HistoryTreeView::search);
}

void HistorySideBar::urlActivated(const QUrl &url)
{
    openUrl(url);
}

void HistorySideBar::urlCtrlActivated(const QUrl &url)
{
    openUrlInNewTab(url);
}

void HistorySideBar::urlShiftActivated(const QUrl &url)
{
    openUrlInNewWindow(url);
}

void HistorySideBar::openUrl(const QUrl &url)
{
    const QUrl u = !url.isEmpty() ? url : ui->historyTree->selectedUrl();
    m_window->weView()->load(u);
}

void HistorySideBar::openUrlInNewTab(const QUrl &url)
{
    const QUrl u = !url.isEmpty() ? url : ui->historyTree->selectedUrl();
    m_window->tabWidget()->addView(u, qzSettings->newTabPosition);
}

void HistorySideBar::openUrlInNewWindow(const QUrl &url)
{
    const QUrl u = !url.isEmpty() ? url : ui->historyTree->selectedUrl();
    mApp->createWindow(Qz::BW_NewWindow, u);
}

void HistorySideBar::openUrlInNewPrivateWindow(const QUrl &url)
{
    const QUrl u = !url.isEmpty() ? url : ui->historyTree->selectedUrl();
    mApp->startPrivateBrowsing(u);
}

void HistorySideBar::createContextMenu(const QPoint &pos)
{
    QMenu menu;
    QAction* actNewTab = menu.addAction(IconProvider::newTabIcon(), tr("Open in new tab"));
    QAction* actNewWindow = menu.addAction(IconProvider::newWindowIcon(), tr("Open in new window"));
    QAction* actNewPrivateWindow = menu.addAction(IconProvider::privateBrowsingIcon(), tr("Open in new private window"));

    menu.addSeparator();
    QAction* actDelete = menu.addAction(QIcon::fromTheme(QSL("edit-delete")), tr("Delete"));

    connect(actNewTab, SIGNAL(triggered()), this, SLOT(openUrlInNewTab()));
    connect(actNewWindow, SIGNAL(triggered()), this, SLOT(openUrlInNewWindow()));
    connect(actNewPrivateWindow, SIGNAL(triggered()), this, SLOT(openUrlInNewPrivateWindow()));
    connect(actDelete, &QAction::triggered, ui->historyTree, &HistoryTreeView::removeSelectedItems);

    if (ui->historyTree->selectedUrl().isEmpty()) {
        actNewTab->setDisabled(true);
        actNewWindow->setDisabled(true);
        actNewPrivateWindow->setDisabled(true);
    }

    menu.exec(pos);
}

void HistorySideBar::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ui->search->setFocus();
}

void HistorySideBar::onCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    Q_UNUSED(previous)
    showSidebarHint(current);
}

void HistorySideBar::showSidebarHint(const QModelIndex& index)
{
    const QUrl url = index.data(HistoryModel::UrlRole).toUrl();
    mApp->getWindow()->statusBar()->showMessage(url.toString());
}

HistorySideBar::~HistorySideBar()
{
    delete ui;
}
