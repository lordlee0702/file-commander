#include "coperationperformer.h"
#include "filesystemhelperfunctions.h"
#include "directoryscanner.h"
#include "threading/thread_helpers.h"

COperationPerformer::COperationPerformer(Operation operation, const std::vector<CFileSystemObject>& source, QString destination) :
	_source(source),
	_destFileSystemObject(destination),
	_op(operation)
{
}

COperationPerformer::~COperationPerformer()
{
	cancel();

	if (_thread.joinable())
		_thread.join();
}

void COperationPerformer::setWatcher(CFileOperationObserver *watcher)
{
	assert_r(watcher);
	_observer = watcher;
}

bool COperationPerformer::togglePause()
{
	_paused = !_paused;
	return _paused;
}

bool COperationPerformer::paused() const
{
	return _paused;
}

bool COperationPerformer::working() const
{
	return _inProgress;
}

bool COperationPerformer::done() const
{
	return _done;
}

// User can supply a new name (not full path)
void COperationPerformer::userResponse(HaltReason haltReason, UserResponse response, QString newName)
{
	assert_r(_userResponse == urNone); // _userResponse should have been reset after being used
	_newName = newName;

	_userResponse = response;
	if (_userResponse == urSkipAll || _userResponse == urProceedWithAll)
		_globalResponses[haltReason] = response;
	_waitForResponseCondition.notify_one();
}

void COperationPerformer::start()
{
	_thread = std::thread(&COperationPerformer::threadFunc, this);
	while (!_inProgress) std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Waiting for thread to start, not sure what for
}

void COperationPerformer::cancel()
{
	_cancelRequested = true;
}

void COperationPerformer::threadFunc()
{
	setThreadName("COperationPerformer thread");

	switch (_op)
	{
	case operationCopy:
		copyFiles();
		break;
	case operationMove:
		copyFiles();
		break;
	case operationDelete:
		deleteFiles();
		break;
	default:
		assert_unconditional_r("Uknown operation");
		return;
	}
}

void COperationPerformer::waitForResponse()
{
	std::unique_lock<std::mutex> lock(_waitForResponseMutex);
	_totalTimeElapsed.pause();
	while (_userResponse == urNone)
		_waitForResponseCondition.wait(lock);

	_totalTimeElapsed.resume();
}

void COperationPerformer::copyFiles()
{
	if (_source.empty())
	{
		finalize();
		return;
	}

	assert_r(_op == operationCopy || _op == operationMove);

	_inProgress = true;

	size_t currentItemIndex = 0;

	if (_source.size() == 1)
	{
		// If there's just one file to copy, it is allowed to set a new file name as dest (C:/1.txt) instead of just the path (C:/)
		if ((_source.front().isFile() && !_destFileSystemObject.isDir()))
			_newName = _destFileSystemObject.fullName();
	}

	// Check if source and dest are on the same file system / disk drive, in which case moving is much simpler and faster
	// If the dest folder is empty, moving means renaming the root source folder / file, which is fast and simple
	if (_op == operationMove && (!_destFileSystemObject.exists() || _destFileSystemObject.isEmptyDir()) && _source.front().isMovableTo(_destFileSystemObject))
	{
		_totalTimeElapsed.start();

		// TODO: Assuming that all sources are from the same drive / file system. Can that assumption ever be incorrect?
		for (auto sourceIterator = _source.begin(); sourceIterator != _source.end() && !_cancelRequested; _userResponse = urNone /* needed for normal operation of condition variable */)
		{
			if (sourceIterator->isCdUp())
			{
				++sourceIterator;
				++currentItemIndex;
				continue;
			}

			const QString newFileName = !_newName.isEmpty() ? _newName :  sourceIterator->fullName();
			_newName.clear();
			const auto result = sourceIterator->moveAtomically(_destFileSystemObject.isDir() ? _destFileSystemObject.fullAbsolutePath() : _destFileSystemObject.parentDirPath(), newFileName);
			if (result != rcOk)
			{
				const auto response = getUserResponse(result == rcTargetAlreadyExists ? hrFileExists : hrUnknownError, *sourceIterator, CFileSystemObject(_destFileSystemObject.fullAbsolutePath() % '/' % newFileName), sourceIterator->lastErrorMessage());
				// Handler is identical to that of the main loop
				// esp. for the case of hrFileExists
				if (response == urSkipThis || response == urSkipAll)
				{
					++sourceIterator;
					++currentItemIndex;
					continue;
				}
				else if (response == urAbort)
				{
					finalize();
					return;
				}
				else if (response == urRename)
					// _newName has been set and will be taken into account
					continue;
				else if (response == urRetry)
					continue;
				else
					assert_r(response == urProceedWithThis || response == urProceedWithAll);
			}

			++sourceIterator;
			++currentItemIndex;
		}

		finalize();
		return;
	}

	uint64_t totalSize = 0, sizeProcessed = 0;
	const auto destination = flattenSourcesAndCalcDest(totalSize);
	assert_r(destination.size() == _source.size());

	std::vector<CFileSystemObject> dirsToCleanUp;

	_totalTimeElapsed.start();

	for (auto sourceIterator = _source.begin(); sourceIterator != _source.end() && !_cancelRequested; _userResponse = urNone /* needed for normal operation of condition variable */)
	{
		if (sourceIterator->isCdUp())
		{
			++sourceIterator;
			++currentItemIndex;
			continue;
		}

		qInfo() << __FUNCTION__ << "Processing" << (sourceIterator->isFile() ? "file" : "DIR ") << sourceIterator->fullAbsolutePath();
		if (_observer) _observer->onCurrentFileChangedCallback(sourceIterator->fullName());

		const QFileInfo& sourceFileInfo = sourceIterator->qFileInfo();
		if (!sourceFileInfo.exists())
		{
			const auto response = getUserResponse(hrFileDoesntExit, *sourceIterator, CFileSystemObject(), QString());
			if (response == urSkipThis || response == urSkipAll)
			{
				++sourceIterator;
				++currentItemIndex;
				continue;
			}
			else if (response == urAbort)
			{
				finalize();
				return;
			}
			else
				assert_unconditional_r("Unknown response");
		}

		QFileInfo destInfo(destination[currentItemIndex].absoluteFilePath(_newName.isEmpty() ? sourceIterator->fullName() : _newName));
		_newName.clear();
		if (destInfo.absoluteFilePath() == sourceFileInfo.absoluteFilePath())
		{
			++sourceIterator;
			++currentItemIndex;
			continue;
		}

		if (sourceIterator->isFile())
		{
			NextAction nextAction;
			while ((nextAction = copyItem(*sourceIterator, destInfo, destination[currentItemIndex], sizeProcessed, totalSize, currentItemIndex)) == naRetryOperation);
			switch (nextAction)
			{
			case naProceed:
				break;
			case naSkip:
				++sourceIterator;
				++currentItemIndex;
				sizeProcessed += sourceIterator->size();
				continue;
			case naRetryItem:
				continue;
			case naRetryOperation:
			case naAbort:
				finalize();
				return;
			default:
				assert_unconditional_r(QString("Unexpected deleteItem() return value %1").arg(nextAction).toUtf8().constData());
				continue; // Retry
			}

			if (_op == operationMove) // result == ok
			{
				while ((nextAction = deleteItem(*sourceIterator)) == naRetryOperation);

				switch (nextAction)
				{
				case naProceed:
					break;
				case naSkip:
					++sourceIterator;
					++currentItemIndex;
					continue;
				case naRetryItem:
					continue;
				case naAbort:
					finalize();
					return;
				default:
					qInfo() << QString("Unexpected deleteItem() return value %1").arg(nextAction);
					assert_unconditional_r("Unexpected deleteItem() return value");
					continue; // Retry
				}
			}
		}
		else if (sourceIterator->isDir())
		{
			// Creating the folder - empty folders will not be copied without this code
			CFileSystemObject destObject(destInfo);
			if (!destObject.exists())
			{
				NextAction nextAction;
				while ((nextAction = mkPath(QDir(destObject.fullAbsolutePath()))) == naRetryOperation);
				if (nextAction == naRetryItem)
					continue;
				else if (nextAction == naSkip)
				{
					++currentItemIndex;
					++sourceIterator;
					continue;
				}
				else if (nextAction == naRetryOperation)
				{
					finalize();
					return;
				}
				else if (nextAction != naProceed)
					assert_unconditional_r("Unexpected next action");
			}

			if (_op == operationMove)
			{
				if (sourceIterator->isEmptyDir())
				{
					const auto result = sourceIterator->remove();
					if (result != rcOk)
					{
						const auto action = getUserResponse(hrFailedToDelete, *sourceIterator, CFileSystemObject(), sourceIterator->lastErrorMessage());
						if (action == urSkipThis || action == urSkipAll)
						{
							++sourceIterator;
							++currentItemIndex;
							continue;
						}
						else if (action == urAbort)
						{
							finalize();
							return;
						}
						else if (action == urRetry)
							continue;
						else
						{
							assert_unconditional_r("Unexpected next action");
							continue; // Retry
						}
					}
				}
				else // not empty
					dirsToCleanUp.push_back(*sourceIterator);
			}
		}

		sizeProcessed += sourceIterator->size();

		++sourceIterator;
		++currentItemIndex;
	}

	for (auto& dir: dirsToCleanUp)
		dir.remove();

	qInfo() << __FUNCTION__ << "took" << _totalTimeElapsed.elapsed() << "ms";
	finalize();
}

void COperationPerformer::deleteFiles()
{
	_inProgress = true;

	std::vector<CFileSystemObject> fileSystemObjectsList;
	fileSystemObjectsList.reserve(500);

	for (auto it = _source.begin(); it != _source.end() && !_cancelRequested; ++it, _userResponse = urNone /* needed for normal condition variable operation */)
	{
		if (!it->isCdUp())
		{
			scanDirectory(*it, [&fileSystemObjectsList](const CFileSystemObject& item) {
				fileSystemObjectsList.emplace_back(item);
			});
		}
	}

	_totalTimeElapsed.start();

	const size_t totalNumberOfObjects = fileSystemObjectsList.size();
	size_t currentItemIndex = 0;
	for (auto it = fileSystemObjectsList.begin(); it != fileSystemObjectsList.end() && !_cancelRequested; _userResponse = urNone /* needed for normal condition variable operation */)
	{
		handlePause();

		if (!it->isFile())
			continue;

		qInfo() << __FUNCTION__ << "deleting file" << it->fullAbsolutePath();
		if (_observer) _observer->onCurrentFileChangedCallback(it->fullName());

		const uint64_t speed = (currentItemIndex + 1) * 1000000 / std::max(_totalTimeElapsed.elapsed<std::chrono::microseconds>(), (uint64_t)1);
		const uint32_t secondsRemaining = speed > 0 ? (uint32_t) ((totalNumberOfObjects - currentItemIndex - 1) / speed) : 0;
		if (_observer) _observer->onProgressChangedCallback(currentItemIndex * 100.0f / totalNumberOfObjects, currentItemIndex, totalNumberOfObjects, 0, speed, secondsRemaining);

		if (!it->exists())
		{
			const auto response = getUserResponse(hrFileDoesntExit, *it, CFileSystemObject(), QString());
			if (response == urSkipThis || response == urSkipAll)
			{
				++it;
				++currentItemIndex;
				continue;
			}
			else if (response == urRetry)
				continue;
			else if (response == urAbort)
			{
				finalize();
				return;
			}
			else
				assert_unconditional_r("Unknown response");
		}

		NextAction nextAction;
		while ((nextAction = deleteItem(*it)) == naRetryOperation);

		switch (nextAction)
		{
		case naProceed:
			++it;
			++currentItemIndex;
			break;
		case naSkip:
			++it;
			++currentItemIndex;
			break;
		case naRetryItem:
			continue;
		case naAbort:
			finalize();
			return;
		default:
			qInfo() << QString("Unexpected deleteItem() return value %1").arg(nextAction);
			assert_unconditional_r("Unexpected deleteItem() return value");
			continue;
		}
	}

	// TODO: eliminate code duplication
	// We know that files and directories are being enumerated depth-first, so we need to delete them in reverse order to avoid trying to delete non-empty directories
	for (auto it = fileSystemObjectsList.rbegin(); it != fileSystemObjectsList.rend() && !_cancelRequested; _userResponse = urNone /* needed for normal condition variable operation */)
	{
		handlePause();

		if (!it->isDir())
			continue;

		qInfo() << __FUNCTION__ << "deleting directory" << it->fullAbsolutePath();
		if (_observer) _observer->onCurrentFileChangedCallback(it->fullName());

		const uint64_t speed = (currentItemIndex + 1) * 1000000 / std::max(_totalTimeElapsed.elapsed<std::chrono::microseconds>(), (uint64_t)1);
		const uint32_t secondsRemaining = speed > 0 ? (uint32_t) ((totalNumberOfObjects - currentItemIndex) / speed) : 0;
		if (_observer) _observer->onProgressChangedCallback(currentItemIndex * 100.0f / totalNumberOfObjects, currentItemIndex, totalNumberOfObjects, 0, speed, secondsRemaining);

		if (!it->exists())
		{
			const auto response = getUserResponse(hrFileDoesntExit, *it, CFileSystemObject(), QString());
			if (response == urSkipThis || response == urSkipAll)
			{
				++it;
				++currentItemIndex;
				continue;
			}
			else if (response == urRetry)
				continue;
			else if (response == urAbort)
			{
				finalize();
				return;
			}
			else
				assert_unconditional_r("Unknown response");
		}

		NextAction nextAction;
		while ((nextAction = deleteItem(*it)) == naRetryOperation);

		switch (nextAction)
		{
		case naProceed:
			++it;
			++currentItemIndex;
			break;
		case naSkip:
			++it;
			++currentItemIndex;
			break;
		case naRetryItem:
			continue;
		case naAbort:
			finalize();
			return;
		default:
			assert_unconditional_r(QString("Unexpected deleteItem() return value %1").arg(nextAction).toUtf8().constData());
			continue;
		}
	}

	finalize();
}

void COperationPerformer::finalize()
{
	_done = true;
	_paused   = false;
	if (_observer) _observer->onProcessFinishedCallback();
}

inline bool isAbsolutePath(const QString& path)
{
#ifdef _WIN32
	return path.contains(':') || path.startsWith("//");
#else
	return path.startsWith('/');
#endif
}

inline QDir destinationFolder(const QString &absoluteSourcePath, const QString &originPath, const QString &destPath, bool /*sourceIsDir*/)
{
	QString localPath = absoluteSourcePath.mid(originPath.length());
	assert_r(!localPath.isEmpty());
	if (localPath.startsWith('\\') || localPath.startsWith('/'))
		localPath = localPath.remove(0, 1);

	assert(isAbsolutePath(destPath));
	return CFileSystemObject(destPath % '/' % localPath).parentDirPath();
}

// Iterates over all dirs in the source vector, and their subdirs, and so on and replaces _sources with a flat list of files. Returns a list of destination folders where each of the files must be copied to according to _dest
// Also counts the total size of all the files to monitor progress

// TODO: refactor to a separate algorithm that iterates recursively over subdirs.
// Then I would no longer need to calculate the total size of all files in the same method just to avoid code duplication.
std::vector<QDir> COperationPerformer::flattenSourcesAndCalcDest(uint64_t &totalSize)
{
	totalSize = 0;
	std::vector<CFileSystemObject> newSourceVector;
	std::vector<QDir> destinations;
	const bool destIsFileName = _source.size() == 1 && !_destFileSystemObject.isDir();
	for (auto& o: _source)
	{
		if (o.isFile())
		{
			totalSize += o.size();
			// Ignoring the new file name here if it was supplied. We're only calculating dest dir here, not the file name
			destinations.emplace_back(destinationFolder(o.fullAbsolutePath(), o.parentDirPath(), destIsFileName ? _destFileSystemObject.parentDirPath() : _destFileSystemObject.fullAbsolutePath(), false));
			newSourceVector.push_back(o);
		}
		else if (o.isDir())
		{
			scanDirectory(o, [&](const CFileSystemObject& item) {
				if (item.isFile())
				{
					totalSize += item.size();
					destinations.emplace_back(destinationFolder(item.fullAbsolutePath(), o.parentDirPath(), _destFileSystemObject.fullAbsolutePath(), item.isDir() /* TODO: 'false' ? */)); 
					newSourceVector.push_back(item);
				}
			});

			destinations.emplace_back(destinationFolder(o.fullAbsolutePath(), o.parentDirPath(), _destFileSystemObject.fullAbsolutePath(), true));
			newSourceVector.push_back(o);
		}
	};

	_source = newSourceVector;
	return destinations;
}

UserResponse COperationPerformer::getUserResponse(HaltReason hr, const CFileSystemObject& src, const CFileSystemObject& dst, const QString& message)
{
	auto globalResponse = _globalResponses.find(hr);
	if (globalResponse != _globalResponses.end())
		return globalResponse->second;

	if (_observer) _observer->onProcessHaltedCallback(hr, src, dst, message);
	waitForResponse();
	const auto response = _userResponse;
	_userResponse = urNone;
	return response;
}

COperationPerformer::NextAction COperationPerformer::deleteItem(CFileSystemObject& item)
{
	if (item.isFile())
	{
		if (!item.isWriteable())
		{
			const auto response = getUserResponse(hrSourceFileIsReadOnly, item, CFileSystemObject(), item.lastErrorMessage());
			if (response == urSkipThis || response == urSkipAll)
				return naSkip;
			else if (response == urAbort)
				return naAbort;
			else if (response == urRetry)
				return naRetryOperation;
			else
				assert_r((response == urProceedWithThis || response == urProceedWithAll) && _newName.isEmpty());

			NextAction nextAction;
			while ((nextAction = makeItemWriteable(item)) == naRetryOperation);
			if (nextAction != naProceed)
				return nextAction;
		}
	}

	if (item.remove() != rcOk)
	{
		qInfo() << "Error removing" << (item.isFile() ? "file" : "folder") << item.fullAbsolutePath() << ", error: " << item.lastErrorMessage();
		const auto response = getUserResponse(hrFailedToDelete, item, CFileSystemObject(), item.lastErrorMessage());
		if (response == urSkipThis || response == urSkipAll)
			return naSkip;
		else if (response == urAbort)
			return naAbort;
		else if (response == urRetry)
			return naRetryOperation;
		else
		{
			assert_unconditional_r("Unexpected user response");
			return naRetryOperation;
		}
	}

	return naProceed;
}

COperationPerformer::NextAction COperationPerformer::makeItemWriteable(CFileSystemObject& item)
{
	if (!item.makeWritable())
	{
		qInfo() << "Error making file" << item.fullAbsolutePath() << "writable, retrying";
		const auto response = getUserResponse(hrFailedToMakeItemWritable, item, CFileSystemObject(), item.lastErrorMessage());
		if (response == urSkipThis || response == urSkipAll)
			return naSkip;
		else if (response == urAbort)
			return naAbort;
		else if (response == urRetry)
			return naRetryOperation;
		else
		{
			assert_unconditional_r("Unexpected user response");
			return naRetryOperation;
		}
	}

	return naProceed;
}

COperationPerformer::NextAction COperationPerformer::copyItem(CFileSystemObject& item, const QFileInfo& destInfo, const QDir& destDir, uint64_t sizeProcessedPreviously, uint64_t totalSize, size_t currentItemIndex)
{
	if (!item.isFile())
		return naProceed;

	CFileSystemObject destFile(destInfo);

	if (destFile.exists() && destFile.isFile())
	{
		auto response = getUserResponse(hrFileExists, item, destFile, QString());
		if (response == urSkipThis || response == urSkipAll)
			return naSkip;
		else if (response == urAbort)
			return naAbort;
		else if (response == urRetry)
			return naRetryItem;
		else if (response == urRename)
		{
			assert_r(!_newName.isEmpty());
			// Continue - the new name will be accounted for
		}
		else if (response != urProceedWithThis && response != urProceedWithAll)
		{
			assert_unconditional_r("Unexpected user response");
			return naRetryItem;
		}

		// Only call isWriteable for existing items!
		if (!destFile.isWriteable())
		{
			response = getUserResponse(hrDestFileIsReadOnly, destFile, CFileSystemObject(), QString());
			if (response == urSkipThis || response == urSkipAll)
				return naSkip;
			else if (response == urAbort)
				return naAbort;
			else if (response == urRetry)
				return naRetryOperation;
			else
				assert_r((response == urProceedWithThis || response == urProceedWithAll) && _newName.isEmpty());

			NextAction nextAction;
			while ((nextAction = makeItemWriteable(destFile)) == naRetryOperation);
			if (nextAction != naProceed)
				return nextAction;
		}
	}

	if (!destDir.exists())
	{
		NextAction nextAction;
		while ((nextAction = mkPath(destDir)) == naRetryOperation);
		if (nextAction != naProceed)
			return nextAction;
	}

	const int chunkSize = 5 * 1024 * 1024;
	const QString destPath = destDir.absolutePath() + '/';
	FileOperationResultCode result = rcFail;

	do
	{
		handlePause();

		result = item.copyChunk(chunkSize, destPath, _newName.isEmpty() ? (!destFile.isDir() ? destFile.fullName() : QString()) : _newName);
		// Error handling
		if (result != rcOk)
			break;

		const auto actualSizeProcessed = sizeProcessedPreviously + item.bytesCopied();
		const float totalPercentage = totalSize > 0 ? actualSizeProcessed * 100.0f / totalSize : 0.0f; // Bytes
		const float filePercentage = item.size() > 0 ? item.bytesCopied() * 100.0f / item.size() : 0.0f;

		const uint64_t meanSpeed = uint64_t(totalPercentage / 100.0f * actualSizeProcessed * 1e6f) / std::max(_totalTimeElapsed.elapsed<std::chrono::microseconds>(), (uint64_t)1); // Bytes / sec
		const uint32_t secondsRemaining = (uint32_t)((100.0f - totalPercentage) / 100.0f * totalSize / meanSpeed);
		if (_observer) _observer->onProgressChangedCallback(totalPercentage, currentItemIndex, _source.size(), filePercentage, meanSpeed, secondsRemaining);

		// TODO: why isn't this block at the start of 'do-while'?
		if (_cancelRequested)
		{
			assert_message_r(item.cancelCopy() == rcOk, "Failed to cancel item copying");
			result = rcOk;
			break;
		}
	} while (item.copyOperationInProgress());

	if (result != rcOk)
	{
		item.cancelCopy();
		qInfo() << "Error copying file " << item.fullAbsolutePath() << " to " << destPath + (_newName.isEmpty() ? (destInfo.isFile() ? destInfo.fileName() : QString()) : _newName) << ", error: " << item.lastErrorMessage();
		const auto action = getUserResponse(hrUnknownError, item, CFileSystemObject(), item.lastErrorMessage());
		if (action == urSkipThis || action == urSkipAll)
			return naSkip;
		else if (action == urAbort)
			return naAbort;
		else if (action == urRetry)
			return naRetryOperation;
		else
		{
			assert_unconditional_r("Unexpected user response");
			return naRetryOperation;
		}
	}

	return naProceed;
}

COperationPerformer::NextAction COperationPerformer::mkPath(const QDir& dir)
{
	if (dir.mkpath(".") || dir.exists())
		return naProceed;

	const auto response = getUserResponse(hrCreatingFolderFailed, CFileSystemObject(dir), CFileSystemObject(), QString());
	if (response == urSkipThis || response == urSkipAll)
		return naSkip;
	else if (response == urAbort)
		return naAbort;
	else if (response == urRetry)
		return naRetryOperation;
	else
	{
		assert_unconditional_r("Unexpected user response");
		return naRetryItem;
	}
}

void COperationPerformer::handlePause()
{
	if (_paused) // This code is not strictly thread-safe (the value of _paused may change between 'if' and 'while'), but in this context I'm OK with that
	{
		_totalTimeElapsed.pause();
		while (_paused)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

		_totalTimeElapsed.resume();
	}
}
