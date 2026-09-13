// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QPlainTextEdit>

#include "Common/CommonTypes.h"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QSyntaxHighlighter;
class QWidget;
class PPCSymbolDB;

class SourceViewWidget final : public QPlainTextEdit
{
  Q_OBJECT

public:
  explicit SourceViewWidget(PPCSymbolDB& symbol_db, QWidget* parent = nullptr);
  ~SourceViewWidget() override;

  bool ShowSource(u32 file_index, u32 line);
  void SetCurrentAddress(std::optional<u32> address);
  void RefreshBreakpoints();
  void Clear();

  bool HasSource() const { return m_file_index.has_value(); }
  std::optional<u32> GetSelectedAddress() const;
  std::vector<u32> GetSelectedAddresses() const;
  std::optional<u32> GetFileIndex() const { return m_file_index; }

signals:
  void BreakpointToggleRequested(u32 address);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  class Gutter final : public QWidget
  {
  public:
    explicit Gutter(SourceViewWidget* source_view);
    QSize sizeHint() const override;

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    SourceViewWidget* m_source_view;
  };

  int GutterWidth() const;
  void PaintGutter(QPaintEvent* event);
  void HandleGutterClick(QMouseEvent* event);
  void UpdateGutterWidth();
  void UpdateGutter(const QRect& rect, int dy);
  void SelectLine(u32 line);
  std::optional<u32> AddressForLine(u32 line) const;
  std::vector<u32> AddressesForLine(u32 line) const;

  PPCSymbolDB& m_symbol_db;
  Gutter* m_gutter;
  std::unique_ptr<QSyntaxHighlighter> m_highlighter;
  std::optional<u32> m_file_index;
  std::optional<u32> m_current_line;
  std::map<u32, std::vector<u32>> m_line_addresses;
  std::string m_path;
};
