#include "cmainwindow.h"
#include "plugininterface/cpluginwindow.h"
#include "progressdialogs/ccopymovedialog.h"
#include "progressdialogs/cdeleteprogressdialog.h"
#include "progressdialogs/cfileoperationconfirmationprompt.h"
#include "settings.h"
#include "settings/csettings.h"
#include "shell/cshell.h"
#include "settingsui/csettingsdialog.h"
#include "settings/csettingspageinterface.h"
#include "settings/csettingspageoperations.h"
#include "settings/csettingspageedit.h"
#include "settings/csettingspageother.h"
#include "pluginengine/cpluginengine.h"
#include "panel/filelistwidget/cfilelistview.h"
#include "panel/columns.h"
#include "panel/cpanelwidget.h"
#include "filesystemhelperfunctions.h"
#include "filessearchdialog/cfilessearchwindow.h"
#include "updaterUI/cupdaterdialog.h"
#include "aboutdialog/caboutdialog.h"
#include "widgets/cpersistentwindow.h"
#include "version.h"

DISABLE_COMPILER_WARNINGS
#include "ui_cmainwindow.h"

#include <QCloseEvent>
#include <QDesktopWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSortFilterProxyModel>
#include <QWidgetList>
RESTORE_COMPILER_WARNINGS

#ifdef _WIN32
#include <Windows.h>
#endif

#include <memory>

// Main window settings keys
#define KEY_RPANEL_STATE      "Ui/RPanel/State"
#define KEY_LPANEL_STATE      "Ui/LPanel/State"
#define KEY_RPANEL_GEOMETRY   "Ui/RPanel/Geometry"
#define KEY_LPANEL_GEOMETRY   "Ui/LPanel/Geometry"
#define KEY_SPLITTER_SIZES    "Ui/Splitter"
#define KEY_LAST_ACTIVE_PANEL "Ui/LastActivePanel"

CMainWindow * CMainWindow::_instance = nullptr;

CMainWindow::CMainWindow(QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::CMainWindow)
{
	assert_r(!_instance);
	_instance = this;
	ui->setupUi(this);

	installEventFilter(new CPersistenceEnabler("UI/MainWindow", this));

	QSplitterHandle * handle = ui->splitter->handle(1);
	handle->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(handle, &QSplitterHandle::customContextMenuRequested, this, &CMainWindow::splitterContextMenuRequested);

	connect(ui->_commandLine, &CHistoryComboBox::itemActivated, this, &CMainWindow::executeCommand);

	_commandLineCompleter.setCaseSensitivity(Qt::CaseInsensitive);
	_commandLineCompleter.setCompletionMode(QCompleter::InlineCompletion);
	_commandLineCompleter.setCompletionColumn(NameColumn);
	ui->_commandLine->setCompleter(&_commandLineCompleter);
	ui->_commandLine->setClearEditorOnItemActivation(true);
	ui->_commandLine->installEventFilter(this);
}

void CMainWindow::initButtons()
{
	connect(ui->btnView, &QPushButton::clicked, this, &CMainWindow::viewFile);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F3"), this, SLOT(viewFile()), nullptr , Qt::WidgetWithChildrenShortcut));

	connect(ui->btnEdit, &QPushButton::clicked, this, &CMainWindow::editFile);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F4"), this, SLOT(editFile()), nullptr, Qt::WidgetWithChildrenShortcut));

	connect(ui->btnCopy, &QPushButton::clicked, this, &CMainWindow::copySelectedFiles);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F5"), this, SLOT(copySelectedFiles()), nullptr, Qt::WidgetWithChildrenShortcut));

	connect(ui->btnMove, &QPushButton::clicked, this, &CMainWindow::moveSelectedFiles);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F6"), this, SLOT(moveSelectedFiles()), nullptr, Qt::WidgetWithChildrenShortcut));

	connect(ui->btnNewFolder, &QPushButton::clicked, this, &CMainWindow::createFolder);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F7"), this, SLOT(createFolder()), nullptr, Qt::WidgetWithChildrenShortcut));
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("Shift+F7"), this, SLOT(createFile()), nullptr, Qt::WidgetWithChildrenShortcut));

	connect(ui->btnDelete, &QPushButton::clicked, this, &CMainWindow::deleteFiles);
	connect(ui->btnDelete, &QPushButton::customContextMenuRequested, this, &CMainWindow::showRecycleBInContextMenu);
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("F8"), this, SLOT(deleteFiles()), nullptr, Qt::WidgetWithChildrenShortcut));
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("Delete"), this, SLOT(deleteFiles()), nullptr, Qt::WidgetWithChildrenShortcut));
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("Shift+F8"), this, SLOT(deleteFilesIrrevocably()), nullptr, Qt::WidgetWithChildrenShortcut));
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("Shift+Delete"), this, SLOT(deleteFilesIrrevocably()), nullptr, Qt::WidgetWithChildrenShortcut));

	// Command line
	ui->_commandLine->setSelectPreviousItemShortcut(QKeySequence("Ctrl+E"));
	_shortcuts.push_back(std::make_shared<QShortcut>(QKeySequence("Ctrl+E"), this, SLOT(selectPreviousCommandInTheCommandLine()), nullptr, Qt::WidgetWithChildrenShortcut));
}

void CMainWindow::initActions()
{
	connect(ui->actionRefresh, &QAction::triggered, this, &CMainWindow::refresh);
	connect(ui->actionFind, &QAction::triggered, this, &CMainWindow::findFiles);
	connect(ui->actionCopy_current_item_s_path_to_clipboard, &QAction::triggered, this, [this]() {
		_controller->copyCurrentItemToClipboard();
	});
	connect(ui->actionExit, &QAction::triggered, qApp, &QApplication::quit);

	connect(ui->actionOpen_Console_Here, &QAction::triggered, [this]() {
		_controller->openTerminal(_currentFileList->currentDir());
	});

	connect(ui->actionOpen_Admin_console_here, &QAction::triggered, [this]() {
		_controller->openTerminal(_currentFileList->currentDir(), true);
	});

	ui->action_Show_hidden_files->setChecked(CSettings().value(KEY_INTERFACE_SHOW_HIDDEN_FILES, true).toBool());
	connect(ui->action_Show_hidden_files, &QAction::triggered, this, &CMainWindow::showHiddenFiles);
	connect(ui->actionShowAllFiles, &QAction::triggered, this, &CMainWindow::showAllFilesFromCurrentFolderAndBelow);
	connect(ui->action_Settings, &QAction::triggered, this, &CMainWindow::openSettingsDialog);
	connect(ui->actionCalculate_occupied_space, &QAction::triggered, this, &CMainWindow::calculateOccupiedSpace);
	connect(ui->actionQuick_view, &QAction::triggered, this, &CMainWindow::toggleQuickView);

	connect(ui->action_Invert_selection, &QAction::triggered, this, &CMainWindow::invertSelection);

	connect(ui->actionFull_screen_mode, &QAction::toggled, this, &CMainWindow::toggleFullScreenMode);
	connect(ui->actionTablet_mode, &QAction::toggled, this, &CMainWindow::toggleTabletMode);

	connect(ui->action_Check_for_updates, &QAction::triggered, this, &CMainWindow::checkForUpdates);
	connect(ui->actionAbout, &QAction::triggered, this, &CMainWindow::about);
}

// For manual focus management
void CMainWindow::tabKeyPressed()
{
	_otherFileList->setFocusToFileList();
}

bool CMainWindow::copyFiles(const std::vector<CFileSystemObject> & files, const QString & destDir)
{
	if (files.empty() || destDir.isEmpty())
		return false;

	// Fix for #91
	raise();
	activateWindow();

	const QString destPath = files.size() == 1 && files.front().isFile() ? cleanPath(destDir % '/' % files.front().fullName()) : destDir;
	CFileOperationConfirmationPrompt prompt(tr("Copy files"), tr("Copy %1 %2 to").arg(files.size()).arg(files.size() > 1 ? "files" : "file"), toNativeSeparators(destPath), this);
	if (CSettings().value(KEY_OPERATIONS_ASK_FOR_COPY_MOVE_CONFIRMATION, true).toBool())
	{
		if (prompt.exec() != QDialog::Accepted)
			return false;
	}

	CCopyMoveDialog * dialog = new CCopyMoveDialog(operationCopy, files, toPosixSeparators(prompt.text()), this);
	connect(this, &CMainWindow::closed, dialog, &CCopyMoveDialog::deleteLater);
	dialog->show();

	return true;
}

bool CMainWindow::moveFiles(const std::vector<CFileSystemObject> & files, const QString & destDir)
{
	if (files.empty() || destDir.isEmpty())
		return false;

	// Fix for #91
	raise();
	activateWindow();

	CFileOperationConfirmationPrompt prompt(tr("Move files"), tr("Move %1 %2 to").arg(files.size()).arg(files.size() > 1 ? "files" : "file"), toNativeSeparators(destDir), this);
	if (CSettings().value(KEY_OPERATIONS_ASK_FOR_COPY_MOVE_CONFIRMATION, true).toBool())
	{
		if (prompt.exec() != QDialog::Accepted)
			return false;
	}

	CCopyMoveDialog * dialog = new CCopyMoveDialog(operationMove, files, toPosixSeparators(prompt.text()), this);
	connect(this, &CMainWindow::closed, dialog, &CCopyMoveDialog::deleteLater);
	dialog->show();

	return true;
}

CMainWindow::~CMainWindow()
{
	_instance = nullptr;
	delete ui;
}

CMainWindow *CMainWindow::get()
{
	return _instance;
}

bool CMainWindow::created() const
{
	return _controller != nullptr;
}

// One-time initialization
void CMainWindow::onCreate()
{
	assert(!created());

	initCore();

	// Check for updates
	if (CSettings().value(KEY_OTHER_CHECK_FOR_UPDATES_AUTOMATICALLY, true).toBool() && CSettings().value(KEY_LAST_UPDATE_CHECK_TIMESTAMP, QDateTime::fromTime_t(1)).toDateTime().msecsTo(QDateTime::currentDateTime()) >= 1000 * 3600 * 24)
	{
		CSettings().setValue(KEY_LAST_UPDATE_CHECK_TIMESTAMP, QDateTime::currentDateTime());
		auto dlg = new CUpdaterDialog(this, "https://github.com/VioletGiraffe/file-commander", VERSION_STRING, true);
		connect(dlg, &QDialog::rejected, dlg, &QDialog::deleteLater);
		connect(dlg, &QDialog::accepted, dlg, &QDialog::deleteLater);
	}
}

void CMainWindow::updateInterface()
{
	CSettings s;
	ui->splitter->restoreState(s.value(KEY_SPLITTER_SIZES).toByteArray());
	ui->leftPanel->restorePanelGeometry(s.value(KEY_LPANEL_GEOMETRY).toByteArray());
	ui->leftPanel->restorePanelState(s.value(KEY_LPANEL_STATE).toByteArray());
	ui->rightPanel->restorePanelGeometry(s.value(KEY_RPANEL_GEOMETRY).toByteArray());
	ui->rightPanel->restorePanelState(s.value(KEY_RPANEL_STATE).toByteArray());

	ui->_commandLine->addItems(s.value(KEY_LAST_COMMANDS_EXECUTED).toStringList());
	ui->_commandLine->lineEdit()->clear();

	show();

	if ((windowState() & Qt::WindowFullScreen) != 0)
		ui->actionFull_screen_mode->setChecked(true);

	const Panel lastActivePanel = (Panel)CSettings().value(KEY_LAST_ACTIVE_PANEL, LeftPanel).toInt();
	if (lastActivePanel == LeftPanel)
		ui->leftPanel->setFocusToFileList();
	else
		ui->rightPanel->setFocusToFileList();
}

void CMainWindow::closeEvent(QCloseEvent *e)
{
	if (e->type() == QCloseEvent::Close)
	{
		CSettings s;
		s.setValue(KEY_SPLITTER_SIZES, ui->splitter->saveState());
		s.setValue(KEY_LPANEL_GEOMETRY, ui->leftPanel->savePanelGeometry());
		s.setValue(KEY_RPANEL_GEOMETRY, ui->rightPanel->savePanelGeometry());
		s.setValue(KEY_LPANEL_STATE, ui->leftPanel->savePanelState());
		s.setValue(KEY_RPANEL_STATE, ui->rightPanel->savePanelState());

		emit closed(); // Is used to close all child windows
		emit fileQuickVewFinished(); // Cleaning up quick view widgets, if any

		CPluginEngine::get().destroyAllPluginWindows();
	}

	QMainWindow::closeEvent(e);
}

bool CMainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == ui->_commandLine && event->type() == QEvent::KeyPress)
	{
		QKeyEvent * keyEvent = static_cast<QKeyEvent*>(event);
		if (keyEvent->key() == Qt::Key_Escape)
			clearCommandLineAndRestoreFocus();
	}

	return QMainWindow::eventFilter(watched, event);
}

void CMainWindow::itemActivated(qulonglong hash, CPanelWidget *panel)
{
	const FileOperationResultCode result = _controller->itemHashExists(panel->panelPosition(), hash) ? _controller->itemActivated(hash, panel->panelPosition()) : rcObjectDoesntExist;
	switch (result)
	{
	case rcObjectDoesntExist:
		QMessageBox(QMessageBox::Warning, tr("Error"), tr("The file doesn't exist.")).exec();
		break;
	case rcFail:
		QMessageBox(QMessageBox::Critical, tr("Error"), tr("Failed to launch %1").arg(_controller->itemByHash(panel->panelPosition(), hash).fullAbsolutePath())).exec();
		break;
	case rcDirNotAccessible:
		QMessageBox(QMessageBox::Critical, tr("No access"), tr("This item is not accessible.")).exec();
		break;
	default:
		break;
	}
}

void CMainWindow::currentPanelChanged(QStackedWidget *panel)
{
	_currentPanelWidget = panel;
	_currentFileList = dynamic_cast<CPanelWidget*>(panel->widget(0));
	if (panel)
	{
		_otherPanelWidget = panel == ui->leftWidget ? ui->rightWidget : ui->leftWidget;
		_otherFileList = dynamic_cast<CPanelWidget*>(_otherPanelWidget->widget(0));
		assert_r(_otherPanelWidget && _otherFileList);
	}
	else
	{
		_otherPanelWidget = nullptr;
		_otherFileList = nullptr;
	}

	if (_currentFileList)
	{
		_controller->activePanelChanged(_currentFileList->panelPosition());
		CSettings().setValue(KEY_LAST_ACTIVE_PANEL, _currentFileList->panelPosition());
		ui->fullPath->setText(_controller->panel(_currentFileList->panelPosition()).currentDirPathNative());
		CPluginEngine::get().currentPanelChanged(_currentFileList->panelPosition());
		_commandLineCompleter.setModel(_currentFileList->sortModel());
	}
	else
		_commandLineCompleter.setModel(nullptr);
}

void CMainWindow::uiThreadTimerTick()
{
	if (_controller)
		_controller->uiThreadTimerTick();
}

// Window title management (#143)
void CMainWindow::updateWindowTitleWithCurrentFolderNames()
{
	QString leftPanelDirName = _controller->panel(LeftPanel).currentDirName();
	if (leftPanelDirName.length() > 1 && leftPanelDirName.endsWith('/'))
		leftPanelDirName.chop(1);

	QString rightPanelDirName = _controller->panel(RightPanel).currentDirName();
	if (rightPanelDirName.length() > 1 && rightPanelDirName.endsWith('/'))
		rightPanelDirName.chop(1);

	setWindowTitle(leftPanelDirName % " / " % rightPanelDirName);
}

bool CMainWindow::widgetBelongsToHierarchy(QWidget * const widget, QObject * const hierarchy)
{
	if (widget == hierarchy)
		return true;

	const auto& children = hierarchy->children();
	if (children.contains(widget))
		return true;

	for (const auto& child: children)
		if (widgetBelongsToHierarchy(widget, child))
			return true;

	return false;
}

void CMainWindow::splitterContextMenuRequested(QPoint pos)
{
	const QPoint globalPos = dynamic_cast<QWidget*>(sender())->mapToGlobal(pos);
	QMenu menu;
	menu.addAction("50%");
	QAction * selectedItem = menu.exec(globalPos);
	if (selectedItem)
	{
		const int width = (ui->leftPanel->width() + ui->rightPanel->width()) / 2;
		QList<int> sizes;
		sizes.push_back(width);
		sizes.push_back(width);

		ui->splitter->setSizes(sizes);
	}
}

void CMainWindow::copySelectedFiles()
{
	if (_currentFileList && _otherFileList)
		// Some algorithms rely on trailing slash for distinguishing between files and folders for non-existent items
		copyFiles(_controller->items(_currentFileList->panelPosition(), _currentFileList->selectedItemsHashes()), _otherFileList->currentDir() % '/');
}

void CMainWindow::moveSelectedFiles()
{
	if (_currentFileList && _otherFileList)
		// Some algorithms rely on trailing slash for distinguishing between files and folders for non-existent items
		moveFiles(_controller->items(_currentFileList->panelPosition(), _currentFileList->selectedItemsHashes()), _otherFileList->currentDir() % '/');
}

void CMainWindow::deleteFiles()
{
	if (!_currentFileList)
		return;

#ifdef _WIN32
	auto items = _controller->items(_currentFileList->panelPosition(), _currentFileList->selectedItemsHashes());
	std::vector<std::wstring> paths;
	for (auto& item: items)
		paths.emplace_back(toNativeSeparators(item.fullAbsolutePath()).toStdWString());

	if (paths.empty())
		return;

	_controller->execOnWorkerThread([=]() {
		if (!CShell::deleteItems(paths, true, (void*) winId()))
			_controller->execOnUiThread([this]() {
				QMessageBox::warning(this, tr("Error deleting items"), tr("Failed to delete the selected items"));
		});
	});

#else
	deleteFilesIrrevocably();
#endif
}

void CMainWindow::deleteFilesIrrevocably()
{
	if (!_currentFileList)
		return;

	auto items = _controller->items(_currentFileList->panelPosition(), _currentFileList->selectedItemsHashes());
	if (items.empty())
		return;
#ifdef _WIN32
	std::vector<std::wstring> paths;
	for (auto& item: items)
		paths.emplace_back(toNativeSeparators(item.fullAbsolutePath()).toStdWString());

	_controller->execOnWorkerThread([=]() {
		if (!CShell::deleteItems(paths, false, (void*) winId()))
			_controller->execOnUiThread([this]() {
				QMessageBox::warning(this, tr("Error deleting items"), tr("Failed to delete the selected items"));
		});
	});
#else
	if (QMessageBox::question(this, tr("Are you sure?"), tr("Do you want to delete the selected files and folders completely?"), QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
	{
		CDeleteProgressDialog * dialog = new CDeleteProgressDialog(items, _otherFileList->currentDir(), this);
		connect(this, &CMainWindow::closed, dialog, &CDeleteProgressDialog::deleteLater);
		dialog->show();
	}
#endif
}

void CMainWindow::createFolder()
{
	if (!_currentFileList)
		return;

	const auto currentItem = _currentFileList->currentItemHash() != 0 ? _controller->itemByHash(_currentFileList->panelPosition(), _currentFileList->currentItemHash()) : CFileSystemObject();
	const QString currentItemName = !currentItem.isCdUp() ? currentItem.fullName() : QString();
	const QString dirName = QInputDialog::getText(this, tr("New folder"), tr("Enter the name for the new directory"), QLineEdit::Normal, currentItemName);
	if (!dirName.isEmpty())
	{
		if (!_controller->createFolder(_currentFileList->currentDir(), toPosixSeparators(dirName)))
			QMessageBox::warning(this, tr("Failed to create a folder"), tr("Failed to create the folder %1").arg(dirName));
	}
}

void CMainWindow::createFile()
{
	if (!_currentFileList)
		return;

	const auto currentItem = _currentFileList->currentItemHash() != 0 ? _controller->itemByHash(_currentFileList->panelPosition(), _currentFileList->currentItemHash()) : CFileSystemObject();
	const QString currentItemName = !currentItem.isCdUp() ? currentItem.fullName() : QString();
	const QString fileName = QInputDialog::getText(this, tr("New file"), tr("Enter the name for the new file"), QLineEdit::Normal, currentItemName);
	if (!fileName.isEmpty())
	{
		if (!_controller->createFile(_currentFileList->currentDir(), fileName))
			QMessageBox::warning(this, tr("Failed to create a file"), tr("Failed to create the file %1").arg(fileName));
	}
}

void CMainWindow::invertSelection()
{
	if (_currentFileList)
		_currentFileList->invertSelection();
}

// Other UI commands
void CMainWindow::viewFile()
{
	CPluginEngine::get().viewCurrentFile();
}

void CMainWindow::editFile()
{
	QString editorPath = CSettings().value(KEY_EDITOR_PATH).toString();
	if (editorPath.isEmpty() || !QFileInfo(editorPath).exists())
	{
		if (QMessageBox::question(this, tr("Editor not configured"), tr("No editor program has been configured (or the specified path doesn't exist). Do you want to specify the editor now?")) == QMessageBox::Yes)
		{
#ifdef _WIN32
			const QString mask(tr("Executable files (*.exe *.cmd *.bat)"));
#else
			const QString mask;
#endif
			editorPath = QFileDialog::getOpenFileName(this, tr("Browse for editor program"), QString(), mask);
			if (editorPath.isEmpty())
				return;

			CSettings().setValue(KEY_EDITOR_PATH, editorPath);
		}
		else
			return;
	}

	const QString currentFile = _currentFileList ? _controller->itemByHash(_currentFileList->panelPosition(), _currentFileList->currentItemHash()).fullAbsolutePath() : QString();
	if (!currentFile.isEmpty())
	{
#ifdef __APPLE__
		const bool started = std::system((QString("open -n \"") + CSettings().value(KEY_EDITOR_PATH).toString() + "\" --args \"" + currentFile + "\"").toUtf8().constData()) == 0;
#else
		const bool started = QProcess::startDetached(CSettings().value(KEY_EDITOR_PATH).toString(), QStringList() << currentFile);
#endif

		if (!started)
			QMessageBox::information(this, tr("Error"), tr("Cannot launch %1").arg(editorPath));
	}
}

void CMainWindow::showRecycleBInContextMenu(QPoint pos)
{
	const QPoint globalPos = ui->btnDelete->mapToGlobal(pos);
	CShell::recycleBinContextMenu(globalPos.x(), globalPos.y(), (void*)winId());
}

void CMainWindow::toggleQuickView()
{
	if (_quickViewActive)
	{
		_quickViewActive = false;
		assert_r(_currentPanelWidget->count() == 2 || _otherPanelWidget->count() == 2);

		QWidget* viewerWidget = nullptr;
		if (_currentPanelWidget->count() == 2)
		{
			viewerWidget = _currentPanelWidget->widget(1);
			_currentPanelWidget->removeWidget(viewerWidget);
		}
		else
		{
			viewerWidget = _otherPanelWidget->widget(1);
			_otherPanelWidget->removeWidget(viewerWidget);
		}

		if (viewerWidget)
			viewerWidget->deleteLater();

		emit fileQuickVewFinished();
	}
	else
		quickViewCurrentFile();
}

void CMainWindow::currentItemChanged(Panel /*p*/, qulonglong /*itemHash*/)
{
	if (_quickViewActive)
		quickViewCurrentFile();
}

void CMainWindow::toggleFullScreenMode(bool fullscreen)
{
	if (fullscreen)
		showFullScreen();
	else
		showNormal();
}

void CMainWindow::toggleTabletMode(bool tabletMode)
{
	static const int defaultFontSize = QApplication::font().pointSize();

	ui->actionFull_screen_mode->toggle();

	QFont f = QApplication::font();
	f.setPointSize(tabletMode ? 24 : defaultFontSize);
	QApplication::setFont(f);

	auto widgets = QApplication::allWidgets();
	for (auto widget : widgets)
		if (widget)
			widget->setFont(f);
}

bool CMainWindow::executeCommand(QString commandLineText)
{
	if (!_currentFileList || commandLineText.isEmpty())
		return false;

	CShell::executeShellCommand(commandLineText, _currentFileList->currentDir());
	QTimer::singleShot(0, [=](){CSettings().setValue(KEY_LAST_COMMANDS_EXECUTED, ui->_commandLine->items());}); // Saving the list AFTER the combobox actually accepts the newly added item
	clearCommandLineAndRestoreFocus();

	return true;
}

void CMainWindow::selectPreviousCommandInTheCommandLine()
{
	ui->_commandLine->selectPreviousItem();
	ui->_commandLine->setFocus();
}

void CMainWindow::clearCommandLineAndRestoreFocus()
{
	ui->_commandLine->resetToLastSelected(true);
	_currentFileList->setFocusToFileList();
}

void CMainWindow::pasteCurrentFileName()
{
	if (_currentFileList && _currentFileList->currentItemHash() != 0)
	{
		QString textToAdd = _controller->itemByHash(_currentFileList->panelPosition(), _currentFileList->currentItemHash()).fullName();
		if (textToAdd.contains(' '))
			textToAdd = '\"' % textToAdd % '\"';

		const QString newText = ui->_commandLine->lineEdit()->text().isEmpty() ? textToAdd : (ui->_commandLine->lineEdit()->text() % ' ' % textToAdd);
		ui->_commandLine->lineEdit()->setText(newText);
	}
}

void CMainWindow::pasteCurrentFilePath()
{
	if (_currentFileList && _currentFileList->currentItemHash() != 0)
	{
		QString textToAdd = toNativeSeparators(_controller->itemByHash(_currentFileList->panelPosition(), _currentFileList->currentItemHash()).fullAbsolutePath());
		if (textToAdd.contains(' '))
			textToAdd = '\"' % textToAdd % '\"';

		const QString newText = ui->_commandLine->lineEdit()->text().isEmpty() ? textToAdd : (ui->_commandLine->lineEdit()->text() % ' ' % textToAdd);
		ui->_commandLine->lineEdit()->setText(newText);
	}
}

void CMainWindow::refresh()
{
	if (_currentFileList)
		_controller->refreshPanelContents(_currentFileList->panelPosition());
}

void CMainWindow::findFiles()
{
	if (!_currentFileList)
		return;

	auto selectedHashes = _currentFileList->selectedItemsHashes(true);
	std::vector<QString> selectedPaths;
	if (!selectedHashes.empty())
		for (const auto hash: selectedHashes)
			selectedPaths.push_back(_controller->activePanel().itemByHash(hash).fullAbsolutePath());
	else
		selectedPaths.push_back(_currentFileList->currentDir());


	auto fileSearchUi = new CFilesSearchWindow(selectedPaths);
	connect(this, &CMainWindow::closed, fileSearchUi, &CFilesSearchWindow::close);
	fileSearchUi->show();
}

void CMainWindow::showHiddenFiles()
{
	CSettings().setValue(KEY_INTERFACE_SHOW_HIDDEN_FILES, ui->action_Show_hidden_files->isChecked());
	_controller->refreshPanelContents(LeftPanel);
	_controller->refreshPanelContents(RightPanel);
}

void CMainWindow::showAllFilesFromCurrentFolderAndBelow()
{
	if (_currentFileList)
		_controller->showAllFilesFromCurrentFolderAndBelow(_currentFileList->panelPosition());
}

void CMainWindow::openSettingsDialog()
{
	CSettingsDialog settings;
	settings.addSettingsPage(new CSettingsPageInterface);
	settings.addSettingsPage(new CSettingsPageOperations);
	settings.addSettingsPage(new CSettingsPageEdit);
	settings.addSettingsPage(new CSettingsPageOther);
	connect(&settings, &CSettingsDialog::settingsChanged, this, &CMainWindow::settingsChanged);

	settings.adjustSize();

	settings.exec();
}

void CMainWindow::calculateOccupiedSpace()
{
	if (!_currentFileList)
		return;

	const FilesystemObjectsStatistics stats = _controller->calculateStatistics(_currentFileList->panelPosition(), _currentFileList->selectedItemsHashes());
	if (stats.empty())
		return;

	QMessageBox::information(this, tr("Occupied space"), tr("Statistics for the selected items(including subitems):\nFiles: %1\nFolders: %2\nOccupied space: %3").
							 arg(stats.files).arg(stats.folders).arg(fileSizeToString(stats.occupiedSpace)));
}

void CMainWindow::checkForUpdates()
{
	CSettings().setValue(KEY_LAST_UPDATE_CHECK_TIMESTAMP, QDateTime::currentDateTime());
	CUpdaterDialog(this, "https://github.com/VioletGiraffe/file-commander", VERSION_STRING).exec();
}

void CMainWindow::about()
{
	CAboutDialog(this).exec();
}

void CMainWindow::settingsChanged()
{
	_controller->settingsChanged();
	ui->leftPanel->onSettingsChanged();
	ui->rightPanel->onSettingsChanged();
}

void CMainWindow::focusChanged(QWidget * /*old*/, QWidget * now)
{
	if (!now)
		return;

	for (int i = 0; i < ui->leftWidget->count(); ++i)
		if (now == ui->leftWidget || widgetBelongsToHierarchy(now, ui->leftWidget->widget(i)))
			currentPanelChanged(ui->leftWidget);

	for (int i = 0; i < ui->rightWidget->count(); ++i)
		if (now == ui->rightWidget || widgetBelongsToHierarchy(now, ui->rightWidget->widget(i)))
			currentPanelChanged(ui->rightWidget);
}

void CMainWindow::panelContentsChanged(Panel p, FileListRefreshCause /*operation*/)
{
	if (_currentFileList && p == _currentFileList->panelPosition())
		ui->fullPath->setText(_controller->panel(p).currentDirPathNative());

	updateWindowTitleWithCurrentFolderNames();
}

void CMainWindow::itemDiscoveryInProgress(Panel /*p*/, qulonglong /*itemHash*/, size_t /*progress*/, const QString& /*currentDir*/)
{
}

void CMainWindow::initCore()
{
	_controller = std::make_unique<CController>();
	ui->leftPanel->init(_controller.get());
	ui->rightPanel->init(_controller.get());

	_controller->activePanelChanged((Panel)CSettings().value(KEY_LAST_ACTIVE_PANEL, LeftPanel).toInt());

	connect(qApp, &QApplication::focusChanged, this, &CMainWindow::focusChanged);

	_controller->pluginProxy().setToolMenuEntryCreatorImplementation([this](const std::vector<CPluginProxy::MenuTree>& menuEntries) {createToolMenuEntries(menuEntries); });
	// Need to load the plugins only after the menu creator has been set
	_controller->loadPlugins();

	_currentFileList = ui->leftPanel;
	_otherFileList = ui->rightPanel;

	connect(ui->leftPanel->fileListView(), &CFileListView::ctrlEnterPressed, this, &CMainWindow::pasteCurrentFileName);
	connect(ui->rightPanel->fileListView(), &CFileListView::ctrlEnterPressed, this, &CMainWindow::pasteCurrentFileName);
	connect(ui->leftPanel->fileListView(), &CFileListView::ctrlShiftEnterPressed, this, &CMainWindow::pasteCurrentFilePath);
	connect(ui->rightPanel->fileListView(), &CFileListView::ctrlShiftEnterPressed, this, &CMainWindow::pasteCurrentFilePath);

	connect(ui->leftPanel, &CPanelWidget::currentItemChangedSignal, this, &CMainWindow::currentItemChanged);
	connect(ui->rightPanel, &CPanelWidget::currentItemChangedSignal, this, &CMainWindow::currentItemChanged);

	connect(ui->leftPanel, &CPanelWidget::itemActivated, this, &CMainWindow::itemActivated);
	connect(ui->rightPanel, &CPanelWidget::itemActivated, this, &CMainWindow::itemActivated);

	ui->leftPanel->fileListView()->addEventObserver(this);
	ui->rightPanel->fileListView()->addEventObserver(this);

	initButtons();
	initActions();

	ui->leftPanel->setPanelPosition(LeftPanel);
	ui->rightPanel->setPanelPosition(RightPanel);

	ui->fullPath->clear();

	ui->leftWidget->setCurrentIndex(0); // PanelWidget
	ui->rightWidget->setCurrentIndex(0); // PanelWidget

	_controller->panel(LeftPanel).addPanelContentsChangedListener(this);
	_controller->panel(RightPanel).addPanelContentsChangedListener(this);

	connect(&_uiThreadTimer, &QTimer::timeout, this, &CMainWindow::uiThreadTimerTick);
	_uiThreadTimer.start(5);
}

void CMainWindow::createToolMenuEntries(const std::vector<CPluginProxy::MenuTree>& menuEntries)
{
	QMenuBar * menu = menuBar();
	if (!menu)
		return;

	static QMenu * toolMenu = nullptr; // Shouldn't have to be static, but 2 subsequent calls to this method result in "Tools" being added twice. QMenuBar needs event loop to update its children?..
					   // TODO: make it a class member

	for(auto topLevelMenu: menu->findChildren<QMenu*>())
	{
		// TODO: make sure this plays nicely with localization (#145)
		if (topLevelMenu->title().remove('&') == "Tools")
		{
			toolMenu = topLevelMenu;
			break;
		}
	}

	if (!toolMenu)
	{
		// TODO: make sure this plays nicely with localization (#145)
		toolMenu = new QMenu("Tools");
		menu->addMenu(toolMenu);
	}
	else
		toolMenu->addSeparator();

	for(const auto& menuTree: menuEntries)
		addToolMenuEntriesRecursively(menuTree, toolMenu);

	toolMenu->addSeparator();
}

void CMainWindow::addToolMenuEntriesRecursively(const CPluginProxy::MenuTree& entry, QMenu* toolMenu)
{
	assert_r(toolMenu);
	QAction* action = toolMenu->addAction(entry.name);

	if (entry.children.empty())
	{
		const auto handler = entry.handler;
		QObject::connect(action, &QAction::triggered, [handler](bool) {handler();});
	}
	else
	{
		for (const auto& childEntry : entry.children)
			addToolMenuEntriesRecursively(childEntry, toolMenu);
	}
}

bool CMainWindow::fileListReturnPressed()
{
	return _currentFileList ? executeCommand(ui->_commandLine->currentText()) : false;
}

void CMainWindow::quickViewCurrentFile()
{
	if (_quickViewActive)
	{
		assert_r(_otherPanelWidget->count() == 2);
		QWidget* viewerWidget = _otherPanelWidget->widget(1);
		_otherPanelWidget->removeWidget(viewerWidget);
		viewerWidget->deleteLater();
		emit fileQuickVewFinished();
	}

	CPluginWindow * viewerWindow = CPluginEngine::get().createViewerWindowForCurrentFile();
	if (!viewerWindow)
		return;

	connect(this, &CMainWindow::fileQuickVewFinished, viewerWindow, &CPluginWindow::deleteLater);

	_otherPanelWidget->setCurrentIndex(_otherPanelWidget->addWidget(viewerWindow->centralWidget()));
	_quickViewActive = true;
}
