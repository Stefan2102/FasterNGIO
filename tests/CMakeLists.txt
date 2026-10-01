find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(FasterNGIOTests
	GeometryTests.cpp
	NgioCacheWriterTests.cpp
	PipelineTests.cpp
)
fasterngio_configure_target(FasterNGIOTests)
target_link_libraries(FasterNGIOTests PRIVATE FasterNGIOCore GTest::gtest_main)
gtest_discover_tests(FasterNGIOTests DISCOVERY_MODE PRE_TEST)
