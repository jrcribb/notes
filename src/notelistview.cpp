#include "notelistview.h"
#include "notelistdelegate.h"
#include <QDebug>
#include <QPainter>
#include <QApplication>
#include <QAbstractItemView>
#include <QPaintEvent>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QScrollBar>
#include <QMenu>
#include <QFile>
#include <QAction>
#include <QDrag>
#include <QMimeData>
#include <QWindow>
#include <QMetaObject>
#include "tagpool.h"
#include "notelistmodel.h"
#include "nodepath.h"
#include "dbmanager.h"
#include "notelistview_p.h"
#include "notelistdelegateeditor.h"
#include "fontloader.h"

NoteListView::NoteListView(QWidget *parent)
    : QListView(parent),
      m_isScrollBarHidden(true),
      m_animationEnabled(true),
      m_isMousePressed(false),
      m_mousePressHandled(false),
      m_rowHeight(38),
      m_tagPool(nullptr),
      m_dbManager(nullptr),
      m_currentFolderId{ INVALID_NODE_ID },
      m_isInTrash{ false },
      m_isDragging{ false },
      m_isDraggingPinnedNotes{ false },
      m_isPinnedNotesCollapsed{ false },
      m_isDraggingInsidePinned{ false }
{
    setAttribute(Qt::WA_MacShowFocusRect, false);

    setupStyleSheet();

#if (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)) || defined(Q_OS_WIN) || defined(Q_OS_WINDOWS)
    QFile scollBarStyleFile(QStringLiteral(":/styles/components/custom-scrollbar.css"));
    scollBarStyleFile.open(QFile::ReadOnly);
    QString scrollbarStyleSheet = QString::fromLatin1(scollBarStyleFile.readAll());
    verticalScrollBar()->setStyleSheet(scrollbarStyleSheet);
#endif

    QTimer::singleShot(0, this, SLOT(init()));
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &NoteListView::onCustomContextMenu);
    m_contextMenu = new QMenu(this);

    m_deleteNoteAction = new QAction(tr("Delete Note"), this);
    connect(m_deleteNoteAction, &QAction::triggered, this, [this] {
        auto indexes = selectedIndexes();
        emit deleteNoteRequested(indexes);
    });
    m_restoreNoteAction = new QAction(tr("Restore Note"), this);
    connect(m_restoreNoteAction, &QAction::triggered, this, [this] {
        auto indexes = selectedIndexes();
        emit restoreNoteRequested(indexes);
    });
    m_pinNoteAction = new QAction(tr("Pin Note"), this);
    connect(m_pinNoteAction, &QAction::triggered, this, [this] {
        auto indexes = selectedIndexes();
        emit setPinnedNoteRequested(indexes, true);
    });
    m_unpinNoteAction = new QAction(tr("Unpin Note"), this);
    connect(m_unpinNoteAction, &QAction::triggered, this, [this] {
        auto indexes = selectedIndexes();
        emit setPinnedNoteRequested(indexes, false);
    });

    m_newNoteAction = new QAction(tr("New Note"), this);
    connect(m_newNoteAction, &QAction::triggered, this, [this] { emit newNoteRequested(); });

    m_dragPixmap.load("qrc:/images/notepad.icns");
    setDragEnabled(true);
    setAcceptDrops(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
}

NoteListView::~NoteListView()
{
    // Make sure any editors are closed before the view is destroyed
    closeAllEditor();
}

void NoteListView::animateAddedRow(const QModelIndexList &indexes)
{
    auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
    if (delegate != nullptr)
        delegate->setState(NoteListState::Insert, indexes);
}

bool NoteListView::isPinnedNotesCollapsed() const
{
    return m_isPinnedNotesCollapsed;
}

void NoteListView::setIsPinnedNotesCollapsed(bool newIsPinnedNotesCollapsed)
{
    m_isPinnedNotesCollapsed = newIsPinnedNotesCollapsed;
    for (int i = 0; i < model()->rowCount(); ++i) {
        auto index = model()->index(i, 0);
        if (index.isValid()) {
            emit itemDelegate()->sizeHintChanged(index);
        }
    }
    update();
    emit pinnedCollapseChanged();
}

void NoteListView::setCurrentIndexC(const QModelIndex &index)
{
    setCurrentIndex(index);
    clearSelection();
    setSelectionMode(QAbstractItemView::SingleSelection);
    selectionModel()->setCurrentIndex(index, QItemSelectionModel::SelectCurrent);
}

QModelIndexList NoteListView::getSelectedIndex() const
{
    return selectedIndexes();
}

void NoteListView::onRemoveRowRequested(const QModelIndexList &indexes)
{
    if (!indexes.isEmpty()) {
        for (const auto index : std::as_const(indexes)) {
            m_needRemovedNotes.push_back(index.data(NoteListModel::NoteID).toInt());
        }
        auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
        if (delegate != nullptr) {
            if (m_animationEnabled) {
                delegate->setState(NoteListState::Remove, indexes);
            } else {
                delegate->setState(NoteListState::Normal, indexes);
            }
        }
    }
}

bool NoteListView::isDragging() const
{
    return m_isDragging;
}

void NoteListView::setListViewInfo(const ListViewInfo &newListViewInfo)
{
    m_listViewInfo = newListViewInfo;
}

void NoteListView::setCurrentFolderId(int newCurrentFolderId)
{
    m_currentFolderId = newCurrentFolderId;
}

void NoteListView::openPersistentEditorC(const QModelIndex &index)
{
    if (index.isValid()) {
        auto isHaveTag = static_cast<NoteListModel *>(model())->noteIsHaveTag(index);
        if (isHaveTag) {
            auto id = index.data(NoteListModel::NoteID).toInt();
            m_openedEditor[id] = {};
            openPersistentEditor(index);
        }
    }
}

void NoteListView::closePersistentEditorC(const QModelIndex &index)
{
    if (index.isValid()) {
        auto id = index.data(NoteListModel::NoteID).toInt();
        closePersistentEditor(index);
        m_openedEditor.remove(id);
    }
}

void NoteListView::setEditorWidget(int noteId, QWidget *w)
{
    if (m_openedEditor.contains(noteId)) {
        m_openedEditor[noteId].push_back(w);
    } else {
        qDebug() << __FUNCTION__ << "Error: note id" << noteId << "is not in opened editor list";
    }
}

void NoteListView::unsetEditorWidget(int noteId, QWidget *w)
{
    if (m_openedEditor.contains(noteId)) {
        m_openedEditor[noteId].removeAll(w);
    }
}

void NoteListView::closeAllEditor()
{
    for (const auto &id : m_openedEditor.keys()) {
        auto index = static_cast<NoteListModel *>(model())->getNoteIndex(id);
        closePersistentEditor(index);
    }
    m_openedEditor.clear();
}

void NoteListView::setDbManager(DBManager *newDbManager)
{
    m_dbManager = newDbManager;
}

void NoteListView::setIsInTrash(bool newIsInTrash)
{
    m_isInTrash = newIsInTrash;
}

void NoteListView::setTagPool(TagPool *newTagPool)
{
    m_tagPool = newTagPool;
}

void NoteListView::rowsAboutToBeMoved(const QModelIndexList &source)
{
    auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
    if (delegate != nullptr) {
        if (m_animationEnabled) {
            delegate->setState(NoteListState::MoveOut, source);
        } else {
            delegate->setState(NoteListState::Normal, source);
        }
    }
}

void NoteListView::rowsMoved(const QModelIndexList &dest)
{
    auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
    if (delegate != nullptr) {
        if (m_animationEnabled) {
            delegate->setState(NoteListState::Insert, dest);
        } else {
            delegate->setState(NoteListState::Normal, dest);
        }
    }
}

void NoteListView::onRowsInserted(const QModelIndexList &rows)
{
    animateAddedRow(rows);
}

void NoteListView::init()
{
    setMouseTracking(true);
    setUpdatesEnabled(true);
    viewport()->setAttribute(Qt::WA_Hover);

    setupSignalsSlots();
}

bool NoteListView::isDraggingInsidePinned() const
{
    return m_isDraggingInsidePinned;
}

void NoteListView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_isMousePressed) {
        QListView::mouseMoveEvent(event);
        return;
    }
    if ((event->buttons() & Qt::LeftButton) != 0U) {
        if ((event->position().toPoint() - m_dragStartPosition).manhattanLength() >= QApplication::startDragDistance()) {
            startDrag(Qt::MoveAction);
        }
    }
    //    QListView::mouseMoveEvent(event);
}

void NoteListView::mousePressEvent(QMouseEvent *e)
{
    Q_D(NoteListView);
    m_isMousePressed = true;
    auto index = indexAt(e->position().toPoint());
    if (!index.isValid()) {
        emit noteListViewClicked();
        return;
    }
    auto const *noteListModel = static_cast<NoteListModel *>(this->model());
    if ((noteListModel != nullptr) && noteListModel->isFirstPinnedNote(index)) {
        auto rect = visualRect(index);
        auto iconRect = QRect(rect.right() - 25, rect.y() + 2, 20, 20);
        if (iconRect.contains(e->position().toPoint())) {
            setIsPinnedNotesCollapsed(!isPinnedNotesCollapsed());
            m_mousePressHandled = true;
            return;
        }
    }

    if (e->button() == Qt::LeftButton) {
        m_dragStartPosition = e->position().toPoint();
        auto oldIndexes = selectionModel()->selectedIndexes();
        if (!oldIndexes.contains(index)) {
            if (e->modifiers() == Qt::ControlModifier) {
                setSelectionMode(QAbstractItemView::MultiSelection);
                setCurrentIndex(index);
                selectionModel()->setCurrentIndex(index, QItemSelectionModel::SelectCurrent);
                auto selectedIndexes = selectionModel()->selectedIndexes();
                emit notePressed(selectedIndexes);
            } else {
                setCurrentIndexC(index);
                emit notePressed({ index });
            }
            m_mousePressHandled = true;
        }
    } else if (e->button() == Qt::RightButton) {
        auto oldIndexes = selectionModel()->selectedIndexes();
        if (!oldIndexes.contains(index)) {
            setCurrentIndexC(index);
            emit notePressed({ index });
        }
    }
    QPoint offset = d->offset();
    d->pressedPosition = e->position().toPoint() + offset;
}

void NoteListView::mouseReleaseEvent(QMouseEvent *e)
{
    m_isMousePressed = false;
    auto index = indexAt(e->position().toPoint());
    if (!index.isValid()) {
        return;
    }
    if (e->button() == Qt::LeftButton && !m_mousePressHandled) {
        if (e->modifiers() == Qt::ControlModifier) {
            setSelectionMode(QAbstractItemView::MultiSelection);
            auto oldIndexes = selectionModel()->selectedIndexes();
            if (oldIndexes.contains(index) && oldIndexes.size() > 1) {
                selectionModel()->select(index, QItemSelectionModel::Deselect);
            } else {
                setCurrentIndex(index);
                selectionModel()->setCurrentIndex(index, QItemSelectionModel::SelectCurrent);
            }
            auto selectedIndexes = selectionModel()->selectedIndexes();
            emit notePressed(selectedIndexes);
        } else {
            setCurrentIndexC(index);
            emit notePressed({ index });
        }
    }
    m_mousePressHandled = false;
    QListView::mouseReleaseEvent(e);
}

bool NoteListView::viewportEvent(QEvent *e)
{
    if (model() != nullptr) {
        switch (e->type()) {
        case QEvent::Leave: {
            QPoint pt = mapFromGlobal(QCursor::pos());
            QModelIndex index = indexAt(QPoint(10, pt.y()));
            if (index.row() > 0) {
                index = model()->index(index.row() - 1, 0);
                auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
                if (delegate != nullptr) {
                    delegate->setHoveredIndex(QModelIndex());
                    viewport()->update(visualRect(index));
                }
            }
            break;
        }
        default:
            break;
        }
    }

    return QListView::viewportEvent(e);
}

void NoteListView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(NOTE_MIME)) {
        event->acceptProposedAction();
    } else {
        QListView::dragEnterEvent(event);
    }
}

void NoteListView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(NOTE_MIME)) {
        auto index = indexAt(event->position().toPoint());
        auto isPinned = index.data(NoteListModel::NoteIsPinned).toBool();
        if (!index.isValid()) {
            event->ignore();
            return;
        }
        if (!m_isDraggingPinnedNotes && !isPinned) {
            event->ignore();
            return;
        }
        m_isDraggingInsidePinned = isPinned;
        event->acceptProposedAction();
        setDropIndicatorShown(true);
        QListView::dragMoveEvent(event);
        return;
    }
    event->ignore();
}

void NoteListView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    auto *listModel = static_cast<NoteListModel *>(model());
    if (listModel == nullptr) {
        return;
    }
    for (int i = 0; i < listModel->rowCount(); ++i) {
        auto index = listModel->index(i, 0);
        if (index.isValid()) {
            auto id = index.data(NoteListModel::NoteID).toInt();
            if (m_openedEditor.contains(id)) {
                auto y = visualRect(index).y();
                auto range = abs(viewport()->height());
                if ((y < -range) || (y > 2 * range)) {
                    m_openedEditor.remove(id);
                    closePersistentEditor(index);
                }
            } else {
                auto y = visualRect(index).y();
                auto range = abs(viewport()->height());
                if (y < -range) {
                    continue;
                }
                if (y > 2 * range) {
                    break;
                }
                openPersistentEditorC(index);
            }
        }
    }
}

void NoteListView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions);
    Q_D(NoteListView);
    auto indexes = selectedIndexes();
    QMimeData *mimeData = d->model->mimeData(indexes);
    if (mimeData == nullptr) {
        return;
    }
    QRect rect;
    QPixmap pixmap;
    if (indexes.size() == 1) {
        auto current = indexes[0];
        auto id = current.data(NoteListModel::NoteID).toInt();
        if (m_openedEditor.contains(id)) {
            QItemViewPaintPairs paintPairs = d->draggablePaintPairs(indexes, &rect);
            Q_UNUSED(paintPairs);
            auto wl = m_openedEditor[id];
            if (!wl.empty()) {
                pixmap = wl.first()->grab();
            } else {
                qDebug() << __FUNCTION__ << "Dragging row" << current.row() << "is in opened editor list but editor widget is null";
            }
        } else {
            pixmap = d->renderToPixmap(indexes, &rect);
        }
        auto const *noteListModel = static_cast<NoteListModel *>(this->model());
        if ((noteListModel != nullptr) && noteListModel->hasPinnedNote()
            && (noteListModel->isFirstPinnedNote(current) || noteListModel->isFirstUnpinnedNote(current))) {
            QRect r(0, 25, rect.width(), rect.height() - 25);
            pixmap = pixmap.copy(r);
            rect.setHeight(rect.height() - 25);
        }
        rect.adjust(horizontalOffset(), verticalOffset(), 0, 0);
    } else {
        pixmap.load(":/images/notepad.ico");
        pixmap = pixmap.scaled(pixmap.width() / 4, pixmap.height() / 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
#ifdef __APPLE__
        QFont displayFont(QFont(QStringLiteral("SF Pro Text")).exactMatch() ? QStringLiteral("SF Pro Text") : QStringLiteral("Roboto"));
#elif _WIN32
        QFont displayFont(QFont(QStringLiteral("Segoe UI")).exactMatch() ? QStringLiteral("Segoe UI") : QStringLiteral("Roboto"));
#else
        QFont displayFont(QStringLiteral("Roboto"));
#endif
        displayFont.setPixelSize(16);
        QFontMetrics fmContent(displayFont);
        QString sz = QString::number(indexes.size());
        QRect szRect = fmContent.boundingRect(sz);
        QPixmap px(pixmap.width() + szRect.width(), pixmap.height());
        px.fill(Qt::transparent);

        QRect nameRect(px.rect());
        QPainter painter(&px);
        painter.setPen(Qt::red);
        painter.drawPixmap(0, 0, pixmap);
        painter.setFont(displayFont);
        painter.drawText(nameRect, Qt::AlignRight | Qt::AlignBottom, sz);
        painter.end();
        std::swap(pixmap, px);
        rect = px.rect();
    }
    m_isDraggingPinnedNotes = false;
    m_isDraggingPinnedNotes =
            std::any_of(indexes.cbegin(), indexes.cend(), [](const QModelIndex &index) { return index.data(NoteListModel::NoteIsPinned).toBool(); });
    auto *drag = new QDrag(this);
    drag->setPixmap(pixmap);
    drag->setMimeData(mimeData);
    if (indexes.size() == 1) {
        drag->setHotSpot(d->pressedPosition - rect.topLeft());
    } else {
        drag->setHotSpot({ 0, 0 });
    }
    auto openedEditors = m_openedEditor.keys();
    m_isDragging = true;
    Qt::DropAction dropAction = drag->exec(Qt::MoveAction);
    /// Delete later, if there is no drop event.
    if (dropAction == Qt::IgnoreAction) {
        drag->deleteLater();
        mimeData->deleteLater();
    }
    d->dropEventMoved = false;
    m_isDragging = false;
    // Reset the drop indicator
    d->dropIndicatorRect = QRect();
    d->dropIndicatorPosition = OnItem;
    closeAllEditor();
    for (const auto &id : std::as_const(openedEditors)) {
        auto index = static_cast<NoteListModel *>(model())->getNoteIndex(id);
        openPersistentEditorC(index);
    }
    scrollContentsBy(0, 0);
}

void NoteListView::setCurrentRowActive(bool isActive)
{
    auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
    if (delegate == nullptr)
        return;

    delegate->setActive(isActive);
    viewport()->update(visualRect(currentIndex()));
}

void NoteListView::setAnimationEnabled(bool isEnabled)
{
    m_animationEnabled = isEnabled;
}

void NoteListView::setupSignalsSlots()
{
    // remove/add separator
    // current selectected row changed
    connect(selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex &current, const QModelIndex &previous) {
        if (model()) {
            if (current.row() < previous.row()) {
                if (current.row() > 0) {
                    QModelIndex prevIndex = model()->index(current.row() - 1, 0);
                    viewport()->update(visualRect(prevIndex));
                }
            }

            if (current.row() > 1) {
                QModelIndex prevPrevIndex = model()->index(current.row() - 2, 0);
                viewport()->update(visualRect(prevPrevIndex));
            }
        }
    });

    // row was entered
    connect(this, &NoteListView::entered, this, [this](const QModelIndex &index) {
        if (model()) {
            if (index.row() > 1) {
                QModelIndex prevPrevIndex = model()->index(index.row() - 2, 0);
                viewport()->update(visualRect(prevPrevIndex));

                QModelIndex prevIndex = model()->index(index.row() - 1, 0);
                viewport()->update(visualRect(prevIndex));

            } else if (index.row() > 0) {
                QModelIndex prevIndex = model()->index(index.row() - 1, 0);
                viewport()->update(visualRect(prevIndex));
            }

            auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
            if (delegate)
                delegate->setHoveredIndex(index);
        }
    });

    // viewport was entered
    connect(this, &NoteListView::viewportEntered, this, [this]() {
        if (model() && model()->rowCount() > 1) {
            auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
            if (delegate)
                delegate->setHoveredIndex(QModelIndex());

            QModelIndex lastIndex = model()->index(model()->rowCount() - 2, 0);
            viewport()->update(visualRect(lastIndex));
        }
    });

    // remove/add offset right side
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int min, int max) {
        Q_UNUSED(min)

        auto *delegate = static_cast<NoteListDelegate *>(itemDelegate());
        if (delegate) {
            if (max > 0) {
                delegate->setRowRightOffset(2);
            } else {
                delegate->setRowRightOffset(0);
            }
            viewport()->update();
        }
    });
}

/**
 * @brief setup styleSheet
 */
void NoteListView::setupStyleSheet()
{
    QFile file(":/styles/notelistview.css");
    file.open(QFile::ReadOnly);
    setStyleSheet(file.readAll());
}

void NoteListView::addNotesToTag(QSet<int> const &notesId, int tagId)
{
    for (const auto &id : std::as_const(notesId)) {
        auto const *noteListModel = static_cast<NoteListModel *>(this->model());
        if (noteListModel != nullptr) {
            auto index = noteListModel->getNoteIndex(id);
            if (index.isValid()) {
                emit addTagRequested(index, tagId);
            }
        }
    }
}

void NoteListView::removeNotesFromTag(QSet<int> const &notesId, int tagId)
{
    for (const auto &id : std::as_const(notesId)) {
        auto const *noteListModel = static_cast<NoteListModel *>(this->model());
        if (noteListModel != nullptr) {
            auto index = noteListModel->getNoteIndex(id);
            if (index.isValid()) {
                emit removeTagRequested(index, tagId);
            }
        }
    }
}

void NoteListView::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    QListView::selectionChanged(selected, deselected);
    QSet<int> ids;
    for (const auto &index : selectedIndexes()) {
        ids.insert(index.data(NoteListModel::NoteID).toInt());
    }
    emit saveSelectedNote(ids);
}

/**
 * @brief Set theme color for noteView
 */
void NoteListView::setTheme(Theme::Value theme)
{
    setCSSThemeAndUpdate(this, theme);
}

void NoteListView::onCustomContextMenu(QPoint point)
{
    QModelIndex index = indexAt(point);
    if (index.isValid()) {
        auto indexList = selectionModel()->selectedIndexes();
        if (!indexList.contains(index)) {
            setCurrentIndexC(index);
            indexList = selectionModel()->selectedIndexes();
        }
        QSet<int> notes;
        for (const auto &idx : std::as_const(indexList)) {
            notes.insert(idx.data(NoteListModel::NoteID).toInt());
        }
        m_contextMenu->clear();
        if (m_tagPool != nullptr) {
            m_tagsMenu = m_contextMenu->addMenu("Tags ...");
            for (auto *action : std::as_const(m_noteTagActions)) {
                delete action;
            }
            m_noteTagActions.clear();
            auto createTagIcon = [](const QString &color) -> QIcon {
                QPixmap pix{ 32, 32 };
                pix.fill(Qt::transparent);
                QPainter painter{ &pix };
                painter.setRenderHint(QPainter::Antialiasing);
                auto iconRect = QRect((pix.width() - 30) / 2, (pix.height() - 30) / 2, 30, 30);
                painter.setPen(QColor(color));
#ifdef __APPLE__
                int iconPointSizeOffset = 0;
#else
                int iconPointSizeOffset = -4;
#endif
                painter.setFont(font_loader::loadFont("Font Awesome 6 Free Solid", "", 24 + iconPointSizeOffset));
                painter.drawText(iconRect, u8"\uf111"); // fa-circle
                return QIcon{ pix };
            };
            QSet<int> tagInNote;
            const auto tagIds = m_tagPool->tagIds();
            for (const auto &id : tagIds) {
                bool all = true;
                for (const auto &selectedIndex : std::as_const(indexList)) {
                    auto tags = selectedIndex.data(NoteListModel::NoteTagsList).value<QSet<int>>();
                    if (!tags.contains(id)) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    tagInNote.insert(id);
                }
            }
            for (auto id : std::as_const(tagInNote)) {
                auto tag = m_tagPool->getTag(id);
                auto *tagAction = new QAction(QStringLiteral("✓ Remove tag %1").arg(tag.name()), this);
                connect(tagAction, &QAction::triggered, this, [this, id, notes] { removeNotesFromTag(notes, id); });
                tagAction->setIcon(createTagIcon(tag.color()));
                m_tagsMenu->addAction(tagAction);
                m_noteTagActions.append(tagAction);
            }
            m_tagsMenu->addSeparator();
            for (auto id : tagIds) {
                if (tagInNote.contains(id)) {
                    continue;
                }
                auto tag = m_tagPool->getTag(id);
                auto *tagAction = new QAction(QStringLiteral(" %1").arg(tag.name()), this);
                connect(tagAction, &QAction::triggered, this, [this, id, notes] { addNotesToTag(notes, id); });
                tagAction->setIcon(createTagIcon(tag.color()));
                m_tagsMenu->addAction(tagAction);
                m_noteTagActions.append(tagAction);
            }
        }
        if (m_isInTrash) {
            if (notes.size() > 1) {
                m_restoreNoteAction->setText(tr("Restore Notes"));
            } else {
                m_restoreNoteAction->setText(tr("Restore Note"));
            }
            m_contextMenu->addAction(m_restoreNoteAction);
        }
        if (notes.size() > 1) {
            m_deleteNoteAction->setText(tr("Delete Notes"));
        } else {
            m_deleteNoteAction->setText(tr("Delete Note"));
        }
        m_contextMenu->addAction(m_deleteNoteAction);
        if ((!m_listViewInfo.isInTag) && (m_listViewInfo.parentFolderId != TRASH_FOLDER_ID)) {
            m_contextMenu->addSeparator();
            if (notes.size() > 1) {
                m_pinNoteAction->setText(tr("Pin Notes"));
                m_unpinNoteAction->setText(tr("Unpin Notes"));
                ShowAction a = ShowAction::NotInit;
                for (const auto &idx : std::as_const(indexList)) {
                    if (idx.data(NoteListModel::NoteIsPinned).toBool()) {
                        if (a == ShowAction::ShowPin) {
                            a = ShowAction::ShowBoth;
                            break;
                        }
                        a = ShowAction::ShowUnpin;

                    } else {
                        if (a == ShowAction::ShowUnpin) {
                            a = ShowAction::ShowBoth;
                            break;
                        }
                        a = ShowAction::ShowPin;
                    }
                }
                switch (a) {
                case ShowAction::ShowPin:
                    m_contextMenu->addAction(m_pinNoteAction);
                    break;
                case ShowAction::ShowUnpin:
                    m_contextMenu->addAction(m_unpinNoteAction);
                    break;
                default:
                    m_contextMenu->addAction(m_pinNoteAction);
                    m_contextMenu->addAction(m_unpinNoteAction);
                }
            } else {
                m_pinNoteAction->setText(tr("Pin Note"));
                m_unpinNoteAction->setText(tr("Unpin Note"));
                auto isPinned = index.data(NoteListModel::NoteIsPinned).toBool();
                if (!isPinned) {
                    m_contextMenu->addAction(m_pinNoteAction);
                } else {
                    m_contextMenu->addAction(m_unpinNoteAction);
                }
            }
        }
        m_contextMenu->addSeparator();
        if (m_dbManager != nullptr) {
            for (auto *action : std::as_const(m_folderActions)) {
                delete action;
            }
            m_folderActions.clear();
            auto *m = m_contextMenu->addMenu("Move to");
            FolderListType folders;
            QMetaObject::invokeMethod(m_dbManager, "getFolderList", Qt::BlockingQueuedConnection, Q_RETURN_ARG(FolderListType, folders));
            for (const auto &id : folders.keys()) {
                if (id == m_currentFolderId) {
                    continue;
                }
                auto *action = new QAction(folders[id], this);
                connect(action, &QAction::triggered, this, [this, id] {
                    auto indexes = selectedIndexes();
                    for (const auto &selectedIndex : std::as_const(indexes)) {
                        if (selectedIndex.isValid()) {
                            emit moveNoteRequested(selectedIndex.data(NoteListModel::NoteID).toInt(), id);
                        }
                    }
                });
                m->addAction(action);
                m_folderActions.append(action);
            }
            m_contextMenu->addSeparator();
        }
        if (!m_isInTrash) {
            m_contextMenu->addAction(m_newNoteAction);
        }
        m_contextMenu->exec(viewport()->mapToGlobal(point));
    }
}

void NoteListView::onAnimationFinished(NoteListState state)
{
    if (state == NoteListState::Remove) {
        auto *noteListModel = static_cast<NoteListModel *>(this->model());
        if (noteListModel != nullptr) {
            for (const auto id : std::as_const(m_needRemovedNotes)) {
                auto index = noteListModel->getNoteIndex(id);
                noteListModel->removeRow(index.row());
            }
            m_needRemovedNotes.clear();
        }
    }
}

QPixmap NoteListViewPrivate::renderToPixmap(const QModelIndexList &indexes, QRect *r) const
{
    Q_ASSERT(r);
    QItemViewPaintPairs paintPairs = draggablePaintPairs(indexes, r);
    if (paintPairs.isEmpty())
        return {};
    QWindow *window = windowHandle(WindowHandleMode::Closest);
    const qreal scale = (window != nullptr) ? window->devicePixelRatio() : qreal(1);

    QPixmap pixmap(r->size() * scale);
    pixmap.setDevicePixelRatio(scale);

    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QStyleOptionViewItem option = viewOptionsV1();
    option.state |= QStyle::State_Selected;
    for (int j = 0; j < paintPairs.count(); ++j) {
        option.rect = paintPairs.at(j).rect.translated(-r->topLeft());
        const QModelIndex &current = paintPairs.at(j).index;
        Q_Q(const QAbstractItemView);
        adjustViewOptionsForIndex(&option, current);
        q->itemDelegateForIndex(current)->paint(&painter, option, current);
    }
    return pixmap;
}

QStyleOptionViewItem NoteListViewPrivate::viewOptionsV1() const
{
    Q_Q(const NoteListView);
    QStyleOptionViewItem option;
    q->initViewItemOption(&option);
    return option;
}
