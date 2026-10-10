// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <QAbstractItemModel>

#include "Common/CommonTypes.h"
#include "Core/Debugger/PPCVariables.h"

namespace Core
{
class System;
}

class DebugVariablesModel final : public QAbstractItemModel
{
  Q_OBJECT

public:
  explicit DebugVariablesModel(Core::System& system, Core::Debug::PPCVariableScope scope,
                               std::optional<Core::Debug::ExecutionState::ClientId> origin,
                               QObject* parent = nullptr);

  QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
  QModelIndex parent(const QModelIndex& index) const override;
  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& parent = {}) const override;
  bool hasChildren(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
  bool canFetchMore(const QModelIndex& parent) const override;
  void fetchMore(const QModelIndex& parent) override;

  void Refresh();
  void Clear();
  std::optional<u32> Address(const QModelIndex& index) const;
  std::optional<u32> ByteSize(const QModelIndex& index) const;
  bool IsProgramStatic(const QModelIndex& index) const;
  QString Name(const QModelIndex& index) const;

signals:
  void Error(const QString& message);

private:
  struct Node
  {
    Core::Debug::PPCVariable variable;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    std::string key;
    bool children_fetched = false;
    bool changed = false;
  };

  Node* NodeForIndex(const QModelIndex& index) const;
  static std::string MakeKey(const Node* parent, const Core::Debug::PPCVariable& variable, int row);
  void ReplaceRoots(std::vector<Core::Debug::PPCVariable> variables, bool mark_changes);

  Core::Debug::PPCVariables m_variables;
  Core::Debug::PPCVariableScope m_scope;
  std::vector<std::unique_ptr<Node>> m_roots;
  std::unordered_map<std::string, std::string> m_previous_values;
};
