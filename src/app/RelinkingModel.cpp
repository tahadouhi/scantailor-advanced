// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "RelinkingModel.h"

#include <core/IconProvider.h>
#include <foundation/Hashes.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QWaitCondition>
#include <boost/foreach.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include "OutOfMemoryHandler.h"
#include "PayloadEvent.h"

class RelinkingModel::StatusUpdateResponse {
 public:
  StatusUpdateResponse(const QString& path, int row, Status status) : m_path(path), m_row(row), m_status(status) {}

  const QString& path() const { return m_path; }

  int row() const { return m_row; }

  Status status() const { return m_status; }

 private:
  QString m_path;
  int m_row;
  Status m_status;
};


class RelinkingModel::StatusUpdateThread : private QThread {
 public:
  explicit StatusUpdateThread(RelinkingModel* owner);

  /** This will signal the thread to stop and wait for it to happen. */
  ~StatusUpdateThread() override;

  /**
   * Requests are served from last to first.
   * Requesting the same item multiple times will just move the existing
   * record to the top of the stack.
   */
  void requestStatusUpdate(const QString& path, int row);

 private:
  struct Task {
    QString path;
    int row;

    Task(const QString& p, int r) : path(p), row(r) {}
  };

  class HashedByPathTag;
  class OrderedByPriorityTag;

  using TaskList = boost::multi_index_container<
      Task,
      boost::multi_index::indexed_by<
          boost::multi_index::hashed_unique<boost::multi_index::tag<HashedByPathTag>,
                                            boost::multi_index::member<Task, QString, &Task::path>,
                                            hashes::hash<QString>>,
          boost::multi_index::sequenced<boost::multi_index::tag<OrderedByPriorityTag>>>>;

  using TasksByPath = TaskList::index<HashedByPathTag>::type;
  using TasksByPriority = TaskList::index<OrderedByPriorityTag>::type;

  void run() override;

  RelinkingModel* m_owner;
  TaskList m_tasks;
  TasksByPath& m_tasksByPath;
  TasksByPriority& m_tasksByPriority;
  QString m_pathBeingProcessed;
  QMutex m_mutex;
  QWaitCondition m_cond;
  bool m_exiting;
};


/*============================ RelinkingModel =============================*/

RelinkingModel::RelinkingModel()
    : m_fileIcon(IconProvider::getInstance().getIcon("file").pixmap(16, 16)),
      m_folderIcon(IconProvider::getInstance().getIcon("folder").pixmap(16, 16)),
      m_relinker(new Relinker),
      m_statusUpdateThread(new StatusUpdateThread(this)),
      m_haveUncommittedChanges(true) {}

RelinkingModel::~RelinkingModel() = default;

int RelinkingModel::rowCount(const QModelIndex& parent) const {
  if (!parent.isValid()) {
    return static_cast<int>(m_items.size());
  } else {
    return 0;
  }
}

QVariant RelinkingModel::data(const QModelIndex& index, int role) const {
  const Item& item = m_items[index.row()];

  switch (role) {
    case TypeRole:
      return item.type;
    case UncommittedStatusRole:
      return item.uncommittedStatus;
    case UncommittedPathRole:
      return item.uncommittedPath;
    case Qt::DisplayRole:
      if (item.uncommittedPath.startsWith(QChar('/')) && !item.uncommittedPath.startsWith(QLatin1String("//"))) {
        // "//" indicates a network path
        return item.uncommittedPath;
      } else {
        return QDir::toNativeSeparators(item.uncommittedPath);
      }
    case Qt::DecorationRole:
      return (item.type == RelinkablePath::Dir) ? m_folderIcon : m_fileIcon;
    case Qt::BackgroundRole:
      return QColor(Qt::transparent);
    default:
      break;
  }
  return QVariant();
}

void RelinkingModel::addPath(const RelinkablePath& path) {
  const QString& normalizedPath(path.normalizedPath());

  const std::pair<std::set<QString>::iterator, bool> ins(m_origPathSet.insert(path.normalizedPath()));
  if (!ins.second) {
    // Not inserted because identical path is already there.
    return;
  }

  beginInsertRows(QModelIndex(), static_cast<int>(m_items.size()), static_cast<int>(m_items.size()));
  m_items.emplace_back(path);
  endInsertRows();

  requestStatusUpdate(index(static_cast<int>(m_items.size() - 1)));
}

void RelinkingModel::replacePrefix(const QString& prefix, const QString& replacement, RelinkablePath::Type type) {
  QString slashTerminatedPrefix(prefix);
  ensureEndsWithSlash(slashTerminatedPrefix);

  int modifiedRowspanBegin = -1;

  int row = -1;
  for (Item& item : m_items) {
    ++row;
    bool modified = false;

    if (type == RelinkablePath::File) {
      if ((item.type == RelinkablePath::File) && (item.uncommittedPath == prefix)) {
        item.uncommittedPath = replacement;
        modified = true;
      }
    } else {
      assert(type == RelinkablePath::Dir);
      if (item.uncommittedPath.startsWith(slashTerminatedPrefix)) {
        const int suffixLen = item.uncommittedPath.length() - slashTerminatedPrefix.length() + 1;
        item.uncommittedPath = replacement + item.uncommittedPath.right(suffixLen);
        modified = true;
      } else if (item.uncommittedPath == prefix) {
        item.uncommittedPath = replacement;
        modified = true;
      }
    }

    if (modified) {
      m_haveUncommittedChanges = true;
      if (modifiedRowspanBegin == -1) {
        modifiedRowspanBegin = row;
      }
      emit dataChanged(index(modifiedRowspanBegin), index(row));
      requestStatusUpdate(index(row));  // This sets item.changedStatus to StatusUpdatePending.
    } else {
      if (modifiedRowspanBegin != -1) {
        emit dataChanged(index(modifiedRowspanBegin), index(row));
        modifiedRowspanBegin = -1;
      }
    }
  }

  if (modifiedRowspanBegin != -1) {
    emit dataChanged(index(modifiedRowspanBegin), index(row));
  }
}  // RelinkingModel::replacePrefix

bool RelinkingModel::checkForMerges() const {
  std::vector<QString> newPaths;
  newPaths.reserve(m_items.size());

  for (const Item& item : m_items) {
    newPaths.push_back(item.uncommittedPath);
  }

  std::sort(newPaths.begin(), newPaths.end());
  return std::adjacent_find(newPaths.begin(), newPaths.end()) != newPaths.end();
}

void RelinkingModel::commitChanges() {
  if (!m_haveUncommittedChanges) {
    return;
  }

  Relinker newRelinker;
  int modifiedRowspanBegin = -1;

  int row = -1;
  for (Item& item : m_items) {
    ++row;

    if (item.committedPath != item.uncommittedPath) {
      item.committedPath = item.uncommittedPath;
      item.committedStatus = item.uncommittedStatus;
      newRelinker.addMapping(item.origPath, item.committedPath);
      if (modifiedRowspanBegin == -1) {
        modifiedRowspanBegin = row;
      }
    } else {
      if (modifiedRowspanBegin != -1) {
        emit dataChanged(index(modifiedRowspanBegin), index(row));
        modifiedRowspanBegin = -1;
      }
    }
  }

  if (modifiedRowspanBegin != -1) {
    emit dataChanged(index(modifiedRowspanBegin), index(row));
  }

  m_relinker->swap(newRelinker);
  m_haveUncommittedChanges = false;
}  // RelinkingModel::commitChanges

void RelinkingModel::rollbackChanges() {
  if (!m_haveUncommittedChanges) {
    return;
  }

  int modifiedRowspanBegin = -1;

  int row = -1;
  for (Item& item : m_items) {
    ++row;

    if (item.uncommittedPath != item.committedPath) {
      item.uncommittedPath = item.committedPath;
      item.uncommittedStatus = item.committedStatus;
      if (modifiedRowspanBegin == -1) {
        modifiedRowspanBegin = row;
      }
    } else {
      if (modifiedRowspanBegin != -1) {
        emit dataChanged(index(modifiedRowspanBegin), index(row));
        modifiedRowspanBegin = -1;
      }
    }
  }

  if (modifiedRowspanBegin != -1) {
    emit dataChanged(index(modifiedRowspanBegin), index(row));
  }

  m_haveUncommittedChanges = false;
}  // RelinkingModel::rollbackChanges

void RelinkingModel::ensureEndsWithSlash(QString& str) {
  if (!str.endsWith(QChar('/'))) {
    str += QChar('/');
  }
}

void RelinkingModel::requestStatusUpdate(const QModelIndex& index) {
  assert(index.isValid());

  Item& item = m_items[index.row()];
  item.uncommittedStatus = StatusUpdatePending;

  m_statusUpdateThread->requestStatusUpdate(item.uncommittedPath, index.row());
}

void RelinkingModel::customEvent(QEvent* event) {
  using ResponseEvent = PayloadEvent<StatusUpdateResponse>;
  auto* evt = dynamic_cast<ResponseEvent*>(event);
  assert(evt);

  const StatusUpdateResponse& response = evt->payload();
  if ((response.row() < 0) || (response.row() >= int(m_items.size()))) {
    return;
  }

  Item& item = m_items[response.row()];
  if (item.uncommittedPath == response.path()) {
    item.uncommittedStatus = response.status();
  }
  if (item.committedPath == response.path()) {
    item.committedStatus = response.status();
  }

  emit dataChanged(index(response.row()), index(response.row()));
}

/*========================== StatusUpdateThread =========================*/

RelinkingModel::StatusUpdateThread::StatusUpdateThread(RelinkingModel* owner)
    : QThread(owner),
      m_owner(owner),
      m_tasks(),
      m_tasksByPath(m_tasks.get<HashedByPathTag>()),
      m_tasksByPriority(m_tasks.get<OrderedByPriorityTag>()),
      m_exiting(false) {}

RelinkingModel::StatusUpdateThread::~StatusUpdateThread() {
  {
    QMutexLocker locker(&m_mutex);
    m_exiting = true;
  }

  m_cond.wakeAll();
  wait();
}

void RelinkingModel::StatusUpdateThread::requestStatusUpdate(const QString& path, int row) {
  const QMutexLocker locker(&m_mutex);
  if (m_exiting) {
    return;
  }

  if (path == m_pathBeingProcessed) {
    // This task is currently in progress.
    return;
  }

  const std::pair<TasksByPath::iterator, bool> ins(m_tasksByPath.insert(Task(path, row)));

  // Whether inserted or being already there, move it to the front of priority queue.
  m_tasksByPriority.relocate(m_tasksByPriority.end(), m_tasks.project<OrderedByPriorityTag>(ins.first));

  if (!isRunning()) {
    start();
  }

  m_cond.wakeOne();
}

void RelinkingModel::StatusUpdateThread::run() try {
  const QMutexLocker locker(&m_mutex);

  class MutexUnlocker {
   public:
    explicit MutexUnlocker(QMutex* mutex) : m_mutex(mutex) { mutex->unlock(); }

    ~MutexUnlocker() { m_mutex->lock(); }

   private:
    QMutex* const m_mutex;
  };


  while (true) {
    if (m_exiting) {
      break;
    }

    if (m_tasks.empty()) {
      m_cond.wait(&m_mutex);
    }

    if (m_tasks.empty()) {
      continue;
    }

    const Task task(m_tasksByPriority.front());
    m_pathBeingProcessed = task.path;
    m_tasksByPriority.pop_front();

    {
      const MutexUnlocker unlocker(&m_mutex);

      const bool exists = QFile::exists(task.path);
      const StatusUpdateResponse response(task.path, task.row, exists ? Exists : Missing);
      QCoreApplication::postEvent(m_owner, new PayloadEvent<StatusUpdateResponse>(response));
    }

    m_pathBeingProcessed.clear();
  }
}  // RelinkingModel::StatusUpdateThread::run

catch (const std::bad_alloc&) {
  OutOfMemoryHandler::instance().handleOutOfMemorySituation();
}

/*================================ Item =================================*/

RelinkingModel::Item::Item(const RelinkablePath& path)
    : origPath(path.normalizedPath()),
      committedPath(path.normalizedPath()),
      uncommittedPath(path.normalizedPath()),
      type(path.type()),
      committedStatus(StatusUpdatePending),
      uncommittedStatus(StatusUpdatePending) {}

/*============================== Relinker ================================*/

void RelinkingModel::Relinker::addMapping(const QString& from, const QString& to) {
  m_mappings[from] = to;
}

QString RelinkingModel::Relinker::substitutionPathFor(const RelinkablePath& path) const {
  const auto it(m_mappings.find(path.normalizedPath()));
  if (it != m_mappings.end()) {
    return it->second;
  } else {
    return path.normalizedPath();
  }
}

void RelinkingModel::Relinker::swap(Relinker& other) {
  m_mappings.swap(other.m_mappings);
}
