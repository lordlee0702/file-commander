#include "cfilelistsortfilterproxymodel.h"
#include "ccontroller.h"
#include "../../columns.h"

#include <assert.h>

CFileListSortFilterProxyModel::CFileListSortFilterProxyModel(QObject *parent) :
	QSortFilterProxyModel(parent),
	_controller(CController::get()),
	_panel(UnknownPanel),
	_sorter(CNaturalSorting(nsaQCollator, SortingOptions()))
{
}

// Sets the position (left or right) of a panel that this model represents
void CFileListSortFilterProxyModel::setPanelPosition(Panel p)
{
	assert(_panel == UnknownPanel); // Doesn't make sense to call this method more than once
	_panel = p;
}

void CFileListSortFilterProxyModel::setSortingOptions(SortingOptions options)
{
	_sorter.setSortingOptions(options);
}

bool CFileListSortFilterProxyModel::canDropMimeData(const QMimeData * data, Qt::DropAction action, int row, int column, const QModelIndex & parent) const
{
	QModelIndex srcIndex = mapToSource(index(row, column));
	return sourceModel()->canDropMimeData(data, action, srcIndex.row(), srcIndex.column(), parent);
}

void CFileListSortFilterProxyModel::sort(int column, Qt::SortOrder order)
{
	QSortFilterProxyModel::sort(column, order);
	emit sorted();
}

bool CFileListSortFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
	assert(left.column() == right.column());
	assert(left.isValid() && right.isValid());
	const int sortColumn = left.column();

	QStandardItemModel * srcModel = dynamic_cast<QStandardItemModel*>(sourceModel());
	QStandardItem * l = srcModel->item(left.row(), left.column());
	QStandardItem * r = srcModel->item(right.row(), right.column());

	if (!l)
		return true;
	if (!r)
		return false;

	const qulonglong leftHash = l->data(Qt::UserRole).toULongLong();
	const qulonglong rightHash = r->data(Qt::UserRole).toULongLong();

	const CFileSystemObject& leftItem = _controller.itemByHash(_panel, leftHash), rightItem = _controller.itemByHash(_panel, rightHash);

	const bool descendingOrder = sortOrder() == Qt::DescendingOrder;
	// Folders always before files, no matter the sorting column and direction
	if (leftItem.isDir() && !rightItem.isDir())
		return !descendingOrder;  // always keep directory on top
	else if (!leftItem.isDir() && rightItem.isDir())
		return descendingOrder;   // always keep directory on top

	// [..] is always on top
	if (leftItem.isCdUp())
		return !descendingOrder;
	else if (rightItem.isCdUp())
		return descendingOrder;

	switch (sortColumn)
	{
	case NameColumn:
		// File name and extension sort is case-insensitive
		return _sorter.lessThan(leftItem.fullName(), rightItem.fullName());
		break;
	case ExtColumn:
		if (leftItem.isDir() && rightItem.isDir()) // Sorting directories by name, files - by extension
			return _sorter.lessThan(leftItem.name(), rightItem.name());
		else if (!leftItem.isDir() && !rightItem.isDir() && leftItem.extension().isEmpty() && rightItem.extension().isEmpty())
			return _sorter.lessThan(leftItem.name(), rightItem.name());
		else
		{
			QString leftExt = leftItem.extension(), rightExt = rightItem.extension();
			QString leftName = leftItem.name(), rightName = rightItem.name();
			if (rightName.isEmpty())
			{
				rightName = rightExt;
				rightExt.clear();
			}

			if (leftName.isEmpty())
			{
				leftName = leftExt;
				leftExt.clear();
			}

			if (_sorter.lessThan(leftExt, rightExt))
				return true;
			else if (_sorter.lessThan(rightExt, leftExt)) // check if extensions are the same
				return false;
			else // if they are - compare by names
				return _sorter.lessThan(leftName, rightName);
		}
		break;
	case SizeColumn:
		return leftItem.size() < rightItem.size();
		break;
	case DateColumn:
		return leftItem.properties().modificationDate < rightItem.properties().modificationDate;
		break;
	default:
		break;
	}

	assert (false);
	return false;
}
