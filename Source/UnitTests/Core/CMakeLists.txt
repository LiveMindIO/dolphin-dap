add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)
add_dolphin_test(SerialInterfaceTest SerialInterfaceTest.cpp)

if(NOT WIN32)
  add_dolphin_test(PipesTest PipesTest.cpp)
endif()

add_dolphin_test(DapFramingTest Debugger/DAP/DapFramingTest.cpp)
add_dolphin_test(DapJsonTest Debugger/DAP/DapJsonTest.cpp)
add_dolphin_test(DapProtocolTest Debugger/DAP/DapProtocolTest.cpp)
add_dolphin_test(DapControllerTest Debugger/DAP/DapControllerTest.cpp)

target_sources(DapControllerTest PRIVATE Debugger/DWARF/DwarfTestFixture.h)
add_dolphin_test(DapSessionTest Debugger/DAP/DapSessionTest.cpp)

target_sources(DapSessionTest PRIVATE Debugger/DWARF/DwarfTestFixture.h)
add_dolphin_test(RealtimeWatchTest Debugger/DAP/RealtimeWatchTest.cpp)

add_dolphin_test(DwarfReaderTest Debugger/DWARF/DwarfReaderTest.cpp)
add_dolphin_test(PPCSymbolDBLineTest Debugger/DWARF/PPCSymbolDBLineTest.cpp)

add_dolphin_test(EntrypointsImportTest Debugger/Entrypoints/EntrypointsImportTest.cpp)
target_sources(EntrypointsImportTest PRIVATE Debugger/DWARF/DwarfTestFixture.h)

target_sources(DwarfReaderTest PRIVATE Debugger/DWARF/DwarfTestFixture.h)
target_sources(PPCSymbolDBLineTest PRIVATE Debugger/DWARF/DwarfTestFixture.h)

add_dolphin_test(DSPAcceleratorTest DSP/DSPAcceleratorTest.cpp)
add_dolphin_test(DSPAssemblyTest
  DSP/DSPAssemblyTest.cpp
  DSP/DSPTestBinary.cpp
  DSP/DSPTestText.cpp
  DSP/HermesBinary.cpp
  DSP/HermesText.cpp
)

add_dolphin_test(ESFormatsTest IOS/ES/FormatsTest.cpp)

add_dolphin_test(FileSystemTest IOS/FS/FileSystemTest.cpp)

add_dolphin_test(SkylandersTest IOS/USB/SkylandersTest.cpp)

if(_M_X86_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/Jit64Common/ConvertDoubleToSingle.cpp
    PowerPC/Jit64Common/Fres.cpp
    PowerPC/Jit64Common/Frsqrte.cpp
  )
elseif(_M_ARM_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/JitArm64/ConvertSingleDouble.cpp
    PowerPC/JitArm64/FPRF.cpp
    PowerPC/JitArm64/Fres.cpp
    PowerPC/JitArm64/Frsqrte.cpp
    PowerPC/JitArm64/MovI2R.cpp
  )
else()
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
  )
endif()

target_sources(PowerPCTest PRIVATE
  PowerPC/TestValues.h
  StubJit.h
)
