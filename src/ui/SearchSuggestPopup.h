#pragma once
#include <QListWidget>

#include "core/SearchService.h"
#include "ui/Theme.h"

namespace ake {

// The drop-down under the search box (design.md section 7): タグ / 録音 / 全文（発言）. It never takes keyboard focus:
// the search box keeps it and forwards Up/Down/Enter here.
class SearchSuggestPopup : public QListWidget {
    Q_OBJECT
public:
    enum Kind { Header = 1, TagItem, RecordingItem, SegmentItem };
    enum Role { KindRole = Qt::UserRole + 1, IdRole, MsRole, TextRole, SubTextRole, HighlightsRole };

    explicit SearchSuggestPopup(QWidget* parent = nullptr);
    void setTheme(const Theme& t);
    void setSuggestions(const Suggestions& s);
    int itemCountOfKind(Kind k) const;
    bool hasSuggestions() const { return m_hasItems; }

    // Selection moves over the selectable rows only (headers are skipped). -1 = nothing selected.
    void moveSelection(int delta);
    bool hasSelection() const { return currentRow() >= 0 && currentItem() && currentItem()->flags() & Qt::ItemIsSelectable; }
    bool activateCurrent();

    int preferredHeight() const;

signals:
    void tagChosen(qint64 keywordId, const QString& name);
    void recordingChosen(qint64 recordingId);
    void segmentChosen(qint64 recordingId, qint64 startMs);

private:
    void activate(QListWidgetItem* item);
    class Delegate;           // nested: defined in the .cpp
    Delegate* m_delegate;
    bool m_hasItems = false;
};

} // namespace ake
