#pragma once

#include "fileoperationresultcode.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <functional>
#include <stdint.h>
#include <vector>
#include <memory>

class QFile;

enum FileSystemObjectType { UnknownType, Directory, File };

struct CFileSystemObjectProperties {
	uint64_t size = 0;
	qulonglong hash = 0;
	QString completeBaseName;
	QString extension;
	QString fullName;
	QString parentFolder;
	QString fullPath;
	time_t creationDate = std::numeric_limits<time_t>::max();
	time_t modificationDate = std::numeric_limits<time_t>::max();
	FileSystemObjectType type = UnknownType;
	bool isCdUp = false;
	bool exists = false;
};

class QIcon;
class CFileSystemObject
{
public:
	CFileSystemObject() = default;
	explicit CFileSystemObject(const QFileInfo & fileInfo);
	explicit CFileSystemObject(const QString& path);

	inline explicit CFileSystemObject(const QDir& dir) : CFileSystemObject(QString(dir.absolutePath())) {}

	template <typename T, typename U>
	explicit CFileSystemObject(QStringBuilder<T, U>&& stringBuilder) : CFileSystemObject((QString)stringBuilder) {}

	CFileSystemObject& operator=(const QString& path);

	void refreshInfo();
	void setPath(const QString& path);

	bool operator==(const CFileSystemObject& other) const;

// Information about this object
	bool isValid() const;

	bool exists() const;
	const CFileSystemObjectProperties& properties() const;
	FileSystemObjectType type() const;
	bool isFile() const;
	bool isDir() const;
	bool isEmptyDir() const;
	bool isCdUp() const; // returns true if it's ".." item
	bool isExecutable() const;
	bool isReadable() const;
	// Apparently, it will return false for non-existing files
	bool isWriteable() const;
	bool isHidden() const;
	// Returns true if this object is a child of parent, either direct or indirect
	bool isChildOf(const CFileSystemObject& parent) const;
	QString fullAbsolutePath() const;
	QString parentDirPath() const;
	const QIcon& icon() const;
	uint64_t size() const;
	qulonglong hash() const;
	const QFileInfo& qFileInfo() const;
	const QDir& qDir() const; // TODO: this method needs documentation. What's it for?
	static std::vector<QString> pathHierarchy(const QString& path);
	uint64_t rootFileSystemId() const;
	bool isNetworkObject() const;

	bool isMovableTo(const CFileSystemObject& dest) const;

	// A hack to store the size of a directory after it's calculated
	void setDirSize(uint64_t size);

	// File name without suffix, or folder name
	QString name() const;
	// Filename + suffix for files, same as name() for folders
	QString fullName() const;
	QString extension() const;
	QString sizeString() const;
	QString modificationDateString() const;

// Operations
	FileOperationResultCode copyAtomically(const QString& destFolder, const QString& newName = QString());
	FileOperationResultCode moveAtomically(const QString& destFolder, const QString& newName = QString());

// Non-blocking file copy API
	// Requests copying the next (or the first if copyOperationInProgress() returns false) chunk of the file.
	FileOperationResultCode copyChunk(size_t chunkSize, const QString& destFolder, const QString& newName = QString());
	FileOperationResultCode moveChunk(uint64_t chunkSize, const QString& destFolder, const QString& newName = QString());
	bool copyOperationInProgress() const;
	uint64_t bytesCopied() const;
	FileOperationResultCode cancelCopy();

	bool                    makeWritable(bool writeable = true);
	FileOperationResultCode remove();

	QString lastErrorMessage() const;

private:
	static QString expandEnvironmentVariables(const QString& string);

private:
	CFileSystemObjectProperties _properties;
	// For copying / moving
	std::shared_ptr<QFile>      _thisFile;
	std::shared_ptr<QFile>      _destFile;
	uint64_t                    _pos = 0;
	// Can be used to determine whether 2 objects are on the same drive
	mutable uint64_t            _rootFileSystemId = std::numeric_limits<uint64_t>::max();
	QFileInfo                   _fileInfo;
	QDir                        _dir; // TODO: this item needs documentation. What's it for?
	mutable QString             _lastErrorMessage;
};
