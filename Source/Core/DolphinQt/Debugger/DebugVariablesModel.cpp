// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/DebugVariablesModel.h"

#include <algorithm>
#include <iterator>

#include <QBrush>
#include <QColor>

#include "Core/Core.h"
#include "Core/System.h"

DebugVariablesModel::DebugVariablesModel(
    Core::System& system, const Core::Debug::PPCVariableScope scope,
    const std::optional<Core::Debug::ExecutionState::ClientId> origin, QObject* parent)
    : QAbstractItemModel(parent), m_variables(system, origin), m_scope(scope)
{
}

QModelIndex DebugVariablesModel::index(const int row, const int column,
                                       const QModelIndex& parent_index) const
{
  if (row < 0 || column < 0 || column >= columnCount(parent_index))
    return {};
  const Node* parent_node = NodeForIndex(parent_index);
  const auto& children = parent_node ? parent_node->children : m_roots;
  if (static_cast<size_t>(row) >= children.size())
    return {};
  return createIndex(row, column, children[row].get());
}

QModelIndex DebugVariablesModel::parent(const QModelIndex& child) const
{
  const Node* node = NodeForIndex(child);
  if (!node || !node->parent)
    return {};
  const Node* parent_node = node->parent;
  const auto& siblings = parent_node->parent ? parent_node->parent->children : m_roots;
  const auto it = std::ranges::find(siblings, parent_node, &std::unique_ptr<Node>::get);
  if (it == siblings.end())
    return {};
  return createIndex(static_cast<int>(std::distance(siblings.begin(), it)), 0,
                     const_cast<Node*>(parent_node));
}

int DebugVariablesModel::rowCount(const QModelIndex& parent_index) const
{
  if (parent_index.column() > 0)
    return 0;
  const Node* node = NodeForIndex(parent_index);
  return static_cast<int>(node ? node->children.size() : m_roots.size());
}

int DebugVariablesModel::columnCount(const QModelIndex&) const
{
  return 4;
}

bool DebugVariablesModel::hasChildren(const QModelIndex& parent_index) const
{
  const Node* node = NodeForIndex(parent_index);
  return node ? node->variable.children.has_value() : !m_roots.empty();
}

QVariant DebugVariablesModel::data(const QModelIndex& index, const int role) const
{
  const Node* node = NodeForIndex(index);
  if (!node)
    return {};
  if (role == Qt::BackgroundRole && node->changed)
    return QBrush(QColor(255, 245, 170));
  if (role != Qt::DisplayRole && role != Qt::EditRole)
    return {};
  switch (index.column())
  {
  case 0:
    return QString::fromStdString(node->variable.name);
  case 1:
    return QString::fromStdString(node->variable.value);
  case 2:
    return QString::fromStdString(node->variable.type);
  case 3:
    if (node->variable.address)
      return QStringLiteral("0x%1").arg(*node->variable.address, 8, 16, QLatin1Char('0'));
    return {};
  default:
    return {};
  }
}

QVariant DebugVariablesModel::headerData(const int section, const Qt::Orientation orientation,
                                         const int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};
  static constexpr const char* headers[] = {"Name", "Value", "Type", "Address"};
  if (section < 0 || section >= static_cast<int>(std::size(headers)))
    return {};
  return tr(headers[section]);
}

Qt::ItemFlags DebugVariablesModel::flags(const QModelIndex& index) const
{
  const Node* node = NodeForIndex(index);
  Qt::ItemFlags result = QAbstractItemModel::flags(index);
  if (node && index.column() == 1 && node->variable.write_context)
    result |= Qt::ItemIsEditable;
  return result;
}

bool DebugVariablesModel::setData(const QModelIndex& index, const QVariant& value, const int role)
{
  Node* node = NodeForIndex(index);
  if (role != Qt::EditRole || index.column() != 1 || !node || !node->variable.write_context)
    return false;
  auto result = m_variables.SetValue(*node->variable.write_context, value.toString().toStdString());
  if (!result)
  {
    emit Error(QString::fromStdString(result.error()));
    return false;
  }
  Refresh();
  return true;
}

bool DebugVariablesModel::canFetchMore(const QModelIndex& parent_index) const
{
  const Node* node = NodeForIndex(parent_index);
  return node && node->variable.children && !node->children_fetched;
}

void DebugVariablesModel::fetchMore(const QModelIndex& parent_index)
{
  Node* node = NodeForIndex(parent_index);
  if (!node || !node->variable.children || node->children_fetched)
    return;
  node->children_fetched = true;
  auto result = m_variables.GetChildren(*node->variable.children);
  if (!result)
  {
    emit Error(QString::fromStdString(result.error()));
    return;
  }
  if (result->empty())
    return;
  beginInsertRows(parent_index, 0, static_cast<int>(result->size()) - 1);
  for (int row = 0; row < static_cast<int>(result->size()); ++row)
  {
    auto child = std::make_unique<Node>();
    child->variable = std::move((*result)[row]);
    child->parent = node;
    child->key = MakeKey(node, child->variable, row);
    const auto previous = m_previous_values.find(child->key);
    child->changed = child->variable.program_static && previous != m_previous_values.end() &&
                     previous->second != child->variable.value;
    if (child->variable.program_static)
      m_previous_values[child->key] = child->variable.value;
    node->children.emplace_back(std::move(child));
  }
  endInsertRows();
}

void DebugVariablesModel::Refresh()
{
  ReplaceRoots(m_variables.GetVariables(m_scope), true);
}

void DebugVariablesModel::Clear()
{
  ReplaceRoots({}, false);
}

std::optional<u32> DebugVariablesModel::Address(const QModelIndex& index) const
{
  const Node* node = NodeForIndex(index);
  return node ? node->variable.address : std::nullopt;
}

std::optional<u32> DebugVariablesModel::ByteSize(const QModelIndex& index) const
{
  const Node* node = NodeForIndex(index);
  return node ? node->variable.byte_size : std::nullopt;
}

bool DebugVariablesModel::IsProgramStatic(const QModelIndex& index) const
{
  const Node* node = NodeForIndex(index);
  return node && node->variable.program_static;
}

QString DebugVariablesModel::Name(const QModelIndex& index) const
{
  const Node* node = NodeForIndex(index);
  return node ? QString::fromStdString(node->variable.name) : QString{};
}

DebugVariablesModel::Node* DebugVariablesModel::NodeForIndex(const QModelIndex& index) const
{
  return index.isValid() ? static_cast<Node*>(index.internalPointer()) : nullptr;
}

std::string DebugVariablesModel::MakeKey(const Node* parent,
                                         const Core::Debug::PPCVariable& variable, const int row)
{
  std::string key = parent ? parent->key + "/" : std::string{};
  key += variable.name;
  key += ':';
  key += variable.type;
  if (variable.address)
    key += ':' + std::to_string(*variable.address);
  else
    key += ':' + std::to_string(row);
  return key;
}

void DebugVariablesModel::ReplaceRoots(std::vector<Core::Debug::PPCVariable> variables,
                                       const bool mark_changes)
{
  beginResetModel();
  m_roots.clear();
  for (int row = 0; row < static_cast<int>(variables.size()); ++row)
  {
    auto node = std::make_unique<Node>();
    node->variable = std::move(variables[row]);
    node->key = MakeKey(nullptr, node->variable, row);
    const auto previous = m_previous_values.find(node->key);
    node->changed = mark_changes && node->variable.program_static &&
                    previous != m_previous_values.end() && previous->second != node->variable.value;
    if (node->variable.program_static)
      m_previous_values[node->key] = node->variable.value;
    m_roots.emplace_back(std::move(node));
  }
  endResetModel();
}
