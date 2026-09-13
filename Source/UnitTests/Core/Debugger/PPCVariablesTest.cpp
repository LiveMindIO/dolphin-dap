// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/PPCVariables.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DWARF/DwarfTestFixture.h"

namespace
{
class PPCVariablesTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    auto& memory = System().GetMemory();
    memory.Init();
    AddressSpace::Init();
    System().GetCoreTiming().Init();
    Core::DeclareAsCPUThread();
    System().GetCPU().Init(PowerPC::CPUCore::Interpreter);
    auto& state = System().GetPPCState();
    state.msr.IR = 0;
    state.msr.DR = 0;
    System().GetPowerPC().MSRUpdated();
    System().GetPPCSymbolDB().Clear();
  }

  void TearDown() override
  {
    System().GetPPCSymbolDB().Clear();
    System().GetCPU().Shutdown();
    AddressSpace::Shutdown();
    System().GetMemory().Shutdown();
    System().GetCoreTiming().Shutdown();
    Core::UndeclareAsCPUThread();
  }

  void LoadFixture()
  {
    System().GetPPCSymbolDB().SetDwarfDebugInfo(DwarfTestFixture::MakeTypedParseResult());
    auto& state = System().GetPPCState();
    state.pc = DwarfTestFixture::kFunctionAddress;
    state.gpr[1] = DwarfTestFixture::kTypedDataAddress + 8;
    state.gpr[3] = 0x12345678;
    constexpr std::array<u8, 8> point{{0x11, 0x22, 0x33, 0x44, 0, 0, 0x40, 0}};
    System().GetMemory().CopyToEmu(DwarfTestFixture::kTypedDataAddress, point.data(), point.size());
  }

  static Core::System& System() { return Core::System::GetInstance(); }

  Core::Debug::PPCVariableContext Context(Core::Debug::Dwarf::TypeRef type, u32 address,
                                          bool program_static = true)
  {
    auto info = System().GetPPCSymbolDB().GetDwarfDebugInfo();
    return {std::move(info),
            std::move(type),
            Core::Debug::PPCVariableStorageKind::Address,
            address,
            0,
            System().GetCPU().GetExecutionState().GetStopGeneration(),
            std::nullopt,
            program_static};
  }
};

TEST_F(PPCVariablesTest, MaterializesLocalsGlobalsAndChildren)
{
  LoadFixture();
  Core::Debug::PPCVariables variables(System());

  const auto locals = variables.GetVariables(Core::Debug::PPCVariableScope::Locals);
  ASSERT_EQ(locals.size(), 2u);
  EXPECT_EQ(locals[0].value, "0x12345678");
  ASSERT_TRUE(locals[1].children);
  const auto members = variables.GetChildren(*locals[1].children);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(members->size(), 2u);
  EXPECT_EQ((*members)[0].value, "0x11223344");
  EXPECT_EQ((*members)[0].address, DwarfTestFixture::kTypedDataAddress);
  EXPECT_EQ((*members)[0].byte_size, 4u);
  EXPECT_TRUE((*members)[0].writable);
  EXPECT_FALSE((*members)[0].program_static);

  const auto globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_EQ(globals.size(), 2u);
  EXPECT_EQ(globals[0].address, DwarfTestFixture::kTypedDataAddress);
  EXPECT_EQ(globals[0].byte_size, 8u);
  EXPECT_FALSE(globals[0].writable);
  EXPECT_TRUE(globals[0].program_static);
}

TEST_F(PPCVariablesTest, WritesRegisterAndMemoryValuesAndPublishesEvents)
{
  LoadFixture();
  auto& execution = System().GetCPU().GetExecutionState();
  std::vector<std::shared_ptr<const Core::Debug::ExecutionEvent>> events;
  const auto client =
      execution.RegisterClient([&](std::shared_ptr<const Core::Debug::ExecutionEvent> event) {
        if (event->kind == Core::Debug::ExecutionEventKind::ValuesChanged)
          events.emplace_back(std::move(event));
      });
  Core::Debug::PPCVariables variables(System(), client);

  const auto register_write =
      variables.SetValue(Core::Debug::PPCVariableScope::Locals, "argument", "0x89abcdef");
  ASSERT_TRUE(register_write.has_value()) << register_write.error();
  EXPECT_EQ(System().GetPPCState().gpr[3], 0x89abcdefu);

  const auto globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_TRUE(globals[0].children);
  const auto members = variables.GetChildren(*globals[0].children);
  ASSERT_TRUE(members.has_value());
  ASSERT_TRUE((*members)[0].write_context);
  const auto memory_write = variables.SetValue(*(*members)[0].write_context, "0xaabbccdd");
  ASSERT_TRUE(memory_write.has_value()) << memory_write.error();
  EXPECT_EQ(memory_write->value, "0xaabbccdd");
  EXPECT_EQ(System().GetMemory().Read_U32(DwarfTestFixture::kTypedDataAddress), 0xaabbccddu);

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]->origin, client);
  EXPECT_FALSE(events[0]->data_address.has_value());
  EXPECT_EQ(events[1]->data_address, DwarfTestFixture::kTypedDataAddress);
  EXPECT_EQ(events[1]->data_size, 4u);
  execution.UnregisterClient(client);
}

TEST_F(PPCVariablesTest, WritesStrictScalarValuesInBigEndianOrder)
{
  using namespace Core::Debug::Dwarf;
  System().GetPPCSymbolDB().SetDwarfDebugInfo({});
  Core::Debug::PPCVariables variables(System());
  const u32 address = DwarfTestFixture::kTypedDataAddress;

  auto write = [&](u16 fundamental, std::string_view text, u32 offset = 0) {
    return variables.SetValue(
        Context(TypeRef{FundamentalTypeRef{fundamental}, {}}, address + offset), text);
  };
  ASSERT_TRUE(write(4, "-2").has_value());
  EXPECT_EQ(System().GetMemory().Read_U16(address), 0xfffeu);
  ASSERT_TRUE(write(6, "65535", 2).has_value());
  EXPECT_EQ(System().GetMemory().Read_U16(address + 2), 0xffffu);
  ASSERT_TRUE(write(21, "true", 4).has_value());
  EXPECT_EQ(System().GetMemory().Read_U8(address + 4), 1u);
  ASSERT_TRUE(write(1, R"('A')", 5).has_value());
  EXPECT_EQ(System().GetMemory().Read_U8(address + 5), static_cast<u8>('A'));
  ASSERT_TRUE(write(14, "1.5", 8).has_value());
  EXPECT_EQ(System().GetMemory().Read_U32(address + 8), std::bit_cast<u32>(1.5f));
  ASSERT_TRUE(write(15, "-2.25", 16).has_value());
  EXPECT_EQ(System().GetMemory().Read_U64(address + 16), std::bit_cast<u64>(-2.25));
}

TEST_F(PPCVariablesTest, RejectsInvalidConstAggregatePointerAndStaleWritesWithoutMutation)
{
  using namespace Core::Debug::Dwarf;
  auto info = DwarfTestFixture::MakeTypedParseResult();
  System().GetPPCSymbolDB().SetDwarfDebugInfo(std::move(info));
  Core::Debug::PPCVariables variables(System());
  const u32 address = DwarfTestFixture::kTypedDataAddress;
  constexpr std::array<u8, 8> original{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
  System().GetMemory().CopyToEmu(address, original.data(), original.size());

  auto const_context = Context(TypeRef{FundamentalTypeRef{7}, {TypeModifier::Const}}, address);
  EXPECT_FALSE(variables.SetValue(const_context, "1").has_value());
  auto aggregate_context =
      Context(TypeRef{UserTypeRef{DwarfTestFixture::kTypedStructOffset}, {}}, address);
  EXPECT_FALSE(variables.SetValue(aggregate_context, "1").has_value());
  auto pointer_context = Context(TypeRef{FundamentalTypeRef{7}, {TypeModifier::Pointer}}, address);
  EXPECT_FALSE(variables.SetValue(pointer_context, "0xffffffff").has_value());
  auto integer_context = Context(TypeRef{FundamentalTypeRef{7}, {}}, address);
  EXPECT_FALSE(variables.SetValue(integer_context, "2147483648").has_value());
  EXPECT_EQ(System().GetMemory().Read_U64(address), 0x1122334455667788u);

  System().GetCPU().GetExecutionState().PublishStopped(
      {.cause = Core::Debug::ExecutionStopCause::Unknown});
  EXPECT_FALSE(variables.SetValue(integer_context, "1").has_value());
  EXPECT_EQ(System().GetMemory().Read_U64(address), 0x1122334455667788u);
}

TEST_F(PPCVariablesTest, RetainsExactDebugInfoAndEnforcesDepthChildAndMemberBounds)
{
  using namespace Core::Debug::Dwarf;
  ParseResult parsed = DwarfTestFixture::MakeTypedParseResult();
  parsed.types[0].members.push_back(
      {"outside", TypeRef{FundamentalTypeRef{7}, {}}, {LocationKind::MemberOffset, 7, 0}});
  for (u32 i = 0; i < 1001; ++i)
  {
    parsed.types[0].members.push_back({std::to_string(i),
                                       TypeRef{FundamentalTypeRef{3}, {}},
                                       {LocationKind::MemberOffset, i % 8, 0}});
  }
  System().GetPPCSymbolDB().SetDwarfDebugInfo(parsed);
  Core::Debug::PPCVariables variables(System());
  const auto globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_TRUE(globals[0].children);
  const auto context = *globals[0].children;

  const auto children = variables.GetChildren(context);
  ASSERT_TRUE(children.has_value());
  EXPECT_EQ(children->size(), 1000u);
  ASSERT_GE(children->size(), 3u);
  EXPECT_EQ((*children)[2].name, "outside");
  EXPECT_EQ((*children)[2].value, "<unavailable>");
  EXPECT_FALSE((*children)[2].writable);

  auto deep = context;
  deep.depth = 32;
  const auto depth_limited = variables.GetChildren(deep);
  ASSERT_TRUE(depth_limited.has_value());
  EXPECT_TRUE(depth_limited->empty());

  ASSERT_TRUE((*children)[0].write_context);
  const auto write_context = *(*children)[0].write_context;
  const u32 original = System().GetMemory().Read_U32(DwarfTestFixture::kTypedDataAddress);
  System().GetPPCSymbolDB().SetDwarfDebugInfo({});
  const auto stale_children = variables.GetChildren(context);
  ASSERT_FALSE(stale_children.has_value());
  EXPECT_EQ(stale_children.error(), "stale variable context");
  const auto stale_write = variables.SetValue(write_context, "1");
  ASSERT_FALSE(stale_write.has_value());
  EXPECT_EQ(stale_write.error(), "stale variable context");
  EXPECT_EQ(System().GetMemory().Read_U32(DwarfTestFixture::kTypedDataAddress), original);
}

TEST_F(PPCVariablesTest, DoesNotOfferUnsupportedWideRegisterWrites)
{
  using namespace Core::Debug::Dwarf;
  ParseResult parsed;
  parsed.variables.push_back({"wide",
                              TypeRef{FundamentalTypeRef{0x8208}, {}},
                              {LocationKind::Register, 3, 0},
                              VariableKind::Local,
                              DwarfTestFixture::kFunctionAddress,
                              DwarfTestFixture::kFunctionAddress + 4});
  System().GetPPCSymbolDB().SetDwarfDebugInfo(std::move(parsed));
  System().GetPPCState().pc = DwarfTestFixture::kFunctionAddress;

  Core::Debug::PPCVariables variables(System());
  const auto locals = variables.GetVariables(Core::Debug::PPCVariableScope::Locals);
  ASSERT_EQ(locals.size(), 1U);
  EXPECT_FALSE(locals[0].writable);
  EXPECT_FALSE(locals[0].write_context);
}

TEST_F(PPCVariablesTest, RetainsContextsAfterWritesAndRejectsThemAfterExecutionChanges)
{
  LoadFixture();
  Core::Debug::PPCVariables variables(System());
  const auto globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_TRUE(globals[0].children);
  const auto stale = *globals[0].children;

  System().GetCPU().GetExecutionState().PublishValuesChanged();
  const auto after_value_change = variables.GetChildren(stale);
  ASSERT_TRUE(after_value_change.has_value());

  const auto refreshed = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_TRUE(refreshed[0].children);
  auto& execution = System().GetCPU().GetExecutionState();
  const auto client = execution.RegisterClient();
  const auto operation =
      execution.BeginOperation(client, Core::Debug::ExecutionOperationKind::Continue);
  ASSERT_TRUE(operation.has_value());
  ASSERT_TRUE(execution.PublishContinued(client, *operation).has_value());
  const auto while_running = variables.GetChildren(*refreshed[0].children);
  ASSERT_FALSE(while_running.has_value());
  EXPECT_EQ(while_running.error(), "stale variable context");
  execution.PublishStopped({.cause = Core::Debug::ExecutionStopCause::Unknown});
  const auto after_stop = variables.GetChildren(*refreshed[0].children);
  ASSERT_FALSE(after_stop.has_value());
  EXPECT_EQ(after_stop.error(), "stale variable context");
  execution.UnregisterClient(client);
}

TEST_F(PPCVariablesTest, FormatsAndWritesEnumerationsAndRejectsBitfields)
{
  using namespace Core::Debug::Dwarf;
  constexpr u32 enum_offset = 0x100;
  constexpr u32 struct_offset = 0x200;
  const u32 address = DwarfTestFixture::kTypedDataAddress;

  ParseResult parsed;
  Type enumeration;
  enumeration.die_offset = enum_offset;
  enumeration.kind = TypeKind::Enumeration;
  enumeration.name = "Mode";
  enumeration.byte_size = 4;
  enumeration.enumerators = {{"Previous", -1}, {"Next", 2}};
  parsed.types.push_back(std::move(enumeration));

  Type structure;
  structure.die_offset = struct_offset;
  structure.kind = TypeKind::Structure;
  structure.name = "Flags";
  structure.byte_size = 4;
  structure.members.push_back(
      {"enabled", TypeRef{FundamentalTypeRef{9}, {}}, {LocationKind::MemberOffset, 0, 0}, 1, 31});
  parsed.types.push_back(std::move(structure));
  parsed.variables.push_back({"mode",
                              TypeRef{UserTypeRef{enum_offset}, {}},
                              {LocationKind::Address, address, 0},
                              VariableKind::Global});
  parsed.variables.push_back({"flags",
                              TypeRef{UserTypeRef{struct_offset}, {}},
                              {LocationKind::Address, address + 4, 0},
                              VariableKind::Global});
  System().GetPPCSymbolDB().SetDwarfDebugInfo(parsed);
  System().GetMemory().Write_U32(2, address);
  Core::Debug::PPCVariables variables(System());

  auto globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_EQ(globals.size(), 2U);
  EXPECT_EQ(globals[0].value, "Next (0x00000002)");
  ASSERT_TRUE(variables.SetValue(Core::Debug::PPCVariableScope::Globals, "mode", "Previous"));
  EXPECT_EQ(System().GetMemory().Read_U32(address), 0xffffffffU);
  ASSERT_TRUE(variables.SetValue(Core::Debug::PPCVariableScope::Globals, "mode", "0x2"));
  EXPECT_EQ(System().GetMemory().Read_U32(address), 2U);

  Type unsigned_enumeration;
  unsigned_enumeration.die_offset = 0x300;
  unsigned_enumeration.kind = TypeKind::Enumeration;
  unsigned_enumeration.name = "ByteMode";
  unsigned_enumeration.byte_size = 1;
  unsigned_enumeration.enumeration_is_unsigned = true;
  unsigned_enumeration.enumerators = {{"Maximum", 255}};
  ParseResult unsigned_parsed;
  unsigned_parsed.types.push_back(std::move(unsigned_enumeration));
  unsigned_parsed.variables.push_back({"byte_mode",
                                       TypeRef{UserTypeRef{0x300}, {}},
                                       {LocationKind::Address, address + 8, 0},
                                       VariableKind::Global});
  System().GetPPCSymbolDB().SetDwarfDebugInfo(std::move(unsigned_parsed));
  System().GetMemory().Write_U8(0xff, address + 8);
  globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_EQ(globals.size(), 1U);
  EXPECT_EQ(globals[0].value, "Maximum (0xff)");
  ASSERT_TRUE(variables.SetValue(Core::Debug::PPCVariableScope::Globals, "byte_mode", "255"));
  EXPECT_EQ(System().GetMemory().Read_U8(address + 8), 0xffU);

  System().GetPPCSymbolDB().SetDwarfDebugInfo(std::move(parsed));
  globals = variables.GetVariables(Core::Debug::PPCVariableScope::Globals);
  ASSERT_TRUE(globals[1].children);
  const auto members = variables.GetChildren(*globals[1].children);
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 1U);
  EXPECT_EQ((*members)[0].value, "<unavailable>");
  EXPECT_EQ((*members)[0].address, address + 4);
  EXPECT_EQ((*members)[0].byte_size, 4U);
  EXPECT_FALSE((*members)[0].writable);
  EXPECT_FALSE((*members)[0].write_context);
}
}  // namespace
