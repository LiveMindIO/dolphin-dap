// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/SourceViewWidget.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>

#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Settings.h"

namespace
{
constexpr qint64 MAX_HIGHLIGHTED_SOURCE_SIZE = 2 * 1024 * 1024;
constexpr qint64 MAX_SOURCE_FILE_SIZE = 16 * 1024 * 1024;

class CppSyntaxHighlighter final : public QSyntaxHighlighter
{
public:
  explicit CppSyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document)
  {
    const bool dark = Settings::Instance().IsThemeDark();
    AddRule(
        QStringLiteral(
            R"(\b(alignas|alignof|asm|auto|bool|break|case|catch|char|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|extern|false|float|for|friend|goto|if|inline|int|long|namespace|new|nullptr|operator|private|protected|public|register|reinterpret_cast|return|short|signed|sizeof|static|static_assert|static_cast|struct|switch|template|this|throw|true|try|typedef|typename|union|unsigned|using|virtual|void|volatile|while)\b)"),
        dark ? QColor(130, 170, 255) : QColor(0, 0, 180), true);
    AddRule(QStringLiteral(R"(\b(0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+(\.[0-9]+)?)\b)"),
            dark ? QColor(230, 170, 110) : QColor(150, 70, 0));
    AddRule(QStringLiteral(R"(^\s*#\s*[A-Za-z_]+)"),
            dark ? QColor(210, 140, 220) : QColor(130, 0, 130));
    m_literal_format.setForeground(dark ? QColor(160, 220, 140) : QColor(0, 120, 0));
    m_comment_format.setForeground(dark ? QColor(140, 170, 140) : QColor(90, 120, 90));
  }

protected:
  void highlightBlock(const QString& text) override
  {
    const int text_size = static_cast<int>(text.size());
    std::vector<ProtectedSpan> protected_spans;
    protected_spans.reserve(4);

    // Scan once from left to right so delimiters in escaped literals (for
    // example "https://example.com" or "/* text */") remain literal text and
    // quotes inside comments remain comments.
    bool in_block_comment = previousBlockState() == BLOCK_COMMENT_STATE;
    int position = 0;
    while (position < text_size)
    {
      if (in_block_comment)
      {
        const int end = text.indexOf(QStringLiteral("*/"), position);
        if (end < 0)
        {
          protected_spans.push_back({position, text_size - position, &m_comment_format});
          position = text_size;
          break;
        }

        protected_spans.push_back({position, end - position + 2, &m_comment_format});
        position = end + 2;
        in_block_comment = false;
        continue;
      }

      const QChar current = text.at(position);
      if (current == QLatin1Char('/') && position + 1 < text_size)
      {
        const QChar next = text.at(position + 1);
        if (next == QLatin1Char('/'))
        {
          protected_spans.push_back({position, text_size - position, &m_comment_format});
          position = text_size;
          break;
        }
        if (next == QLatin1Char('*'))
        {
          in_block_comment = true;
          continue;
        }
      }

      if (current == QLatin1Char('"') || current == QLatin1Char('\''))
      {
        const QChar quote = current;
        const int start = position++;
        while (position < text_size)
        {
          if (text.at(position) == QLatin1Char('\\'))
          {
            position += position + 1 < text_size ? 2 : 1;
            continue;
          }
          if (text.at(position++) == quote)
            break;
        }
        protected_spans.push_back({start, position - start, &m_literal_format});
        continue;
      }

      ++position;
    }

    setCurrentBlockState(in_block_comment ? BLOCK_COMMENT_STATE : NORMAL_STATE);

    for (const Rule& rule : m_rules)
    {
      auto matches = rule.expression.globalMatch(text);
      size_t span_index = 0;
      while (matches.hasNext())
      {
        const QRegularExpressionMatch match = matches.next();
        const int match_start = match.capturedStart();
        const int match_end = match_start + match.capturedLength();
        while (span_index < protected_spans.size() &&
               protected_spans[span_index].start + protected_spans[span_index].length <=
                   match_start)
        {
          ++span_index;
        }
        if (span_index == protected_spans.size() || protected_spans[span_index].start >= match_end)
          setFormat(match_start, match.capturedLength(), rule.format);
      }
    }

    for (const ProtectedSpan& span : protected_spans)
      setFormat(span.start, span.length, *span.format);
  }

private:
  static constexpr int NORMAL_STATE = 0;
  static constexpr int BLOCK_COMMENT_STATE = 1;

  struct Rule
  {
    QRegularExpression expression;
    QTextCharFormat format;
  };

  struct ProtectedSpan
  {
    int start;
    int length;
    const QTextCharFormat* format;
  };

  void AddRule(QString pattern, const QColor& color, const bool bold = false)
  {
    QTextCharFormat format;
    format.setForeground(color);
    if (bold)
      format.setFontWeight(QFont::Bold);
    m_rules.push_back({QRegularExpression(std::move(pattern)), std::move(format)});
  }

  std::vector<Rule> m_rules;
  QTextCharFormat m_literal_format;
  QTextCharFormat m_comment_format;
};
}  // namespace

SourceViewWidget::Gutter::Gutter(SourceViewWidget* source_view)
    : QWidget(source_view), m_source_view(source_view)
{
  setCursor(Qt::PointingHandCursor);
}

QSize SourceViewWidget::Gutter::sizeHint() const
{
  return QSize(m_source_view->GutterWidth(), 0);
}

void SourceViewWidget::Gutter::mousePressEvent(QMouseEvent* event)
{
  m_source_view->HandleGutterClick(event);
}

void SourceViewWidget::Gutter::paintEvent(QPaintEvent* event)
{
  m_source_view->PaintGutter(event);
}

SourceViewWidget::SourceViewWidget(PPCSymbolDB& symbol_db, QWidget* parent)
    : QPlainTextEdit(parent), m_symbol_db(symbol_db), m_gutter(new Gutter(this))
{
  setReadOnly(true);
  setLineWrapMode(QPlainTextEdit::NoWrap);
  setFont(Settings::Instance().GetDebugFont());
  setPlaceholderText(tr("No source available for the selected address."));

  connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { UpdateGutterWidth(); });
  connect(this, &QPlainTextEdit::updateRequest, this, &SourceViewWidget::UpdateGutter);
  connect(&Settings::Instance(), &Settings::DebugFontChanged, this, [this](const QFont& font) {
    setFont(font);
    m_gutter->setFont(font);
    UpdateGutterWidth();
  });
  connect(&Settings::Instance(), &Settings::ThemeChanged, this, [this] {
    if (m_highlighter)
      m_highlighter = std::make_unique<CppSyntaxHighlighter>(document());
    viewport()->update();
    m_gutter->update();
  });
  UpdateGutterWidth();
}

SourceViewWidget::~SourceViewWidget() = default;

bool SourceViewWidget::ShowSource(const u32 file_index, const u32 line)
{
  const std::optional<std::string> path = m_symbol_db.GetResolvedSourceFile(file_index);
  if (!path)
    return false;

  const std::map<u32, std::vector<u32>> line_addresses =
      m_symbol_db.GetExactLineAddresses(file_index);
  if (!line_addresses.contains(line))
    return false;

  if (!m_file_index || *m_file_index != file_index || m_path != *path)
  {
    QFile file(QString::fromStdString(*path));
    if (!file.open(QIODevice::ReadOnly))
      return false;
    const qint64 size = file.size();
    if (size < 0 || size > MAX_SOURCE_FILE_SIZE)
      return false;
    const QByteArray contents = file.read(MAX_SOURCE_FILE_SIZE + 1);
    if (file.error() != QFileDevice::NoError || contents.size() != size || file.size() != size ||
        !file.atEnd())
      return false;

    m_highlighter.reset();
    setPlainText(QString::fromUtf8(contents));
    if (line == 0 || line > static_cast<u32>(blockCount()))
    {
      Clear();
      return false;
    }
    if (size <= MAX_HIGHLIGHTED_SOURCE_SIZE)
      m_highlighter = std::make_unique<CppSyntaxHighlighter>(document());

    m_file_index = file_index;
    m_path = *path;
  }

  m_line_addresses = line_addresses;
  SelectLine(line);
  RefreshBreakpoints();
  return true;
}

void SourceViewWidget::SetCurrentAddress(const std::optional<u32> address)
{
  m_current_line.reset();
  if (address && m_file_index)
  {
    const std::optional<PPCSymbolDB::SourceLine> source = m_symbol_db.GetSourceLine(*address);
    if (source && source->file_index == *m_file_index && m_line_addresses.contains(source->line))
    {
      m_current_line = source->line;
    }
  }
  m_gutter->update();
}

void SourceViewWidget::RefreshBreakpoints()
{
  m_gutter->update();
}

void SourceViewWidget::Clear()
{
  m_highlighter.reset();
  m_file_index.reset();
  m_current_line.reset();
  m_line_addresses.clear();
  m_path.clear();
  QPlainTextEdit::clear();
}

std::optional<u32> SourceViewWidget::GetSelectedAddress() const
{
  return AddressForLine(static_cast<u32>(textCursor().blockNumber() + 1));
}

std::vector<u32> SourceViewWidget::GetSelectedAddresses() const
{
  return AddressesForLine(static_cast<u32>(textCursor().blockNumber() + 1));
}

int SourceViewWidget::GutterWidth() const
{
  int digits = 1;
  for (int count = std::max(1, blockCount()); count >= 10; count /= 10)
    ++digits;
  const int marker_width = fontMetrics().height() * 2;
  return 8 + marker_width + digits * fontMetrics().horizontalAdvance(QLatin1Char('9'));
}

void SourceViewWidget::PaintGutter(QPaintEvent* event)
{
  QPainter painter(m_gutter);
  painter.fillRect(event->rect(), palette().alternateBase());

  const auto snapshot = Core::System::GetInstance().GetPowerPC().GetBreakPoints().GetSnapshot();
  QTextBlock block = firstVisibleBlock();
  int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  int bottom = top + qRound(blockBoundingRect(block).height());
  const int marker_size = std::max(4, fontMetrics().height() / 2);
  const int marker_x = 4 + marker_size / 2;
  const int number_left = fontMetrics().height() * 2;

  while (block.isValid() && top <= event->rect().bottom())
  {
    if (block.isVisible() && bottom >= event->rect().top())
    {
      const u32 line = static_cast<u32>(block.blockNumber() + 1);
      const auto address = AddressForLine(line);
      const int center_y = top + fontMetrics().height() / 2;
      if (address)
      {
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().mid());
        painter.drawEllipse(QPoint(marker_x, center_y), marker_size / 3, marker_size / 3);
        const auto& addresses = m_line_addresses.at(line);
        if (std::ranges::any_of(addresses, [&snapshot](const u32 row_address) {
              return snapshot->GetRegularBreakpoint(row_address) != nullptr;
            }))
        {
          painter.setBrush(QColor(220, 45, 45));
          painter.drawEllipse(QPoint(marker_x, center_y), marker_size, marker_size);
        }
      }
      if (m_current_line == line)
      {
        const int x = marker_x + marker_size + 3;
        painter.setBrush(QColor(40, 180, 80));
        painter.setPen(Qt::NoPen);
        const std::array<QPoint, 3> arrow{{QPoint(x, center_y - marker_size),
                                           QPoint(x + marker_size, center_y),
                                           QPoint(x, center_y + marker_size)}};
        painter.drawPolygon(arrow.data(), static_cast<int>(arrow.size()));
      }

      painter.setPen(palette().text().color());
      painter.drawText(number_left, top, m_gutter->width() - number_left - 4,
                       fontMetrics().height(), Qt::AlignRight,
                       QString::number(block.blockNumber() + 1));
    }
    block = block.next();
    top = bottom;
    bottom = top + qRound(blockBoundingRect(block).height());
  }
}

void SourceViewWidget::HandleGutterClick(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton)
    return;
  const QTextCursor cursor = cursorForPosition(QPoint(0, event->position().toPoint().y()));
  const u32 line = static_cast<u32>(cursor.blockNumber() + 1);
  if (const std::optional<u32> address = AddressForLine(line))
  {
    SelectLine(line);
    const std::vector<u32> addresses = AddressesForLine(line);
    const auto snapshot = Core::System::GetInstance().GetPowerPC().GetBreakPoints().GetSnapshot();
    bool removed_existing = false;
    for (const u32 row_address : addresses)
    {
      if (snapshot->GetRegularBreakpoint(row_address) != nullptr)
      {
        emit BreakpointToggleRequested(row_address);
        removed_existing = true;
      }
    }
    if (!removed_existing)
      emit BreakpointToggleRequested(*address);
  }
}

void SourceViewWidget::UpdateGutterWidth()
{
  setViewportMargins(GutterWidth(), 0, 0, 0);
}

void SourceViewWidget::UpdateGutter(const QRect& rect, const int dy)
{
  if (dy != 0)
    m_gutter->scroll(0, dy);
  else
    m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
  if (rect.contains(viewport()->rect()))
    UpdateGutterWidth();
}

void SourceViewWidget::SelectLine(const u32 line)
{
  if (line == 0 || line > static_cast<u32>(blockCount()))
    return;
  QTextCursor cursor(document()->findBlockByNumber(static_cast<int>(line - 1)));
  setTextCursor(cursor);
  centerCursor();

  QList<QTextEdit::ExtraSelection> selections;
  QTextEdit::ExtraSelection selection;
  selection.cursor = cursor;
  selection.cursor.clearSelection();
  selection.format.setBackground(palette().alternateBase());
  selection.format.setProperty(QTextFormat::FullWidthSelection, true);
  selections.push_back(selection);
  setExtraSelections(selections);
}

std::optional<u32> SourceViewWidget::AddressForLine(const u32 line) const
{
  const auto it = m_line_addresses.find(line);
  return it == m_line_addresses.end() || it->second.empty() ?
             std::nullopt :
             std::make_optional(it->second.front());
}

std::vector<u32> SourceViewWidget::AddressesForLine(const u32 line) const
{
  const auto it = m_line_addresses.find(line);
  return it == m_line_addresses.end() ? std::vector<u32>{} : it->second;
}

void SourceViewWidget::resizeEvent(QResizeEvent* event)
{
  QPlainTextEdit::resizeEvent(event);
  const QRect rect = contentsRect();
  m_gutter->setGeometry(QRect(rect.left(), rect.top(), GutterWidth(), rect.height()));
}
