find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One file per module; TestSupport.h has the shared temp-folder and file helpers.
add_executable(FasterNGIOTests
	CellCacheTests.cpp
	CellPipelineTests.cpp
	CommandLineTests.cpp
	FileWriterPoolTests.cpp
	GameDataTests.cpp
	GameIniTests.cpp
	GameInstallTests.cpp
	GeometryTests.cpp
	IniFileTests.cpp
	ModOrganizerTests.cpp
	MpscQueueTests.cpp
	NgioCacheWriterTests.cpp
	PlacementTests.cpp
	RejectionTests.cpp
	TestSupport.h
	UserSettingsTests.cpp
)
fasterngio_configure_target(FasterNGIOTests)
# zlib writes the compressed records of the test plugins.
target_link_libraries(FasterNGIOTests PRIVATE FasterNGIOApp ZLIB::ZLIB GTest::gtest_main)
gtest_discover_tests(FasterNGIOTests DISCOVERY_MODE PRE_TEST)
