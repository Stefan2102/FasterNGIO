#include "Pipeline/FileWriterPool.h"
#include "Platform/WholeFile.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace FasterNGIO;
	using Tests::TempDirectory;
}

TEST(FileWriterPool, WritesEveryFileFromManyProducers)
{
	TempDirectory directory;
	auto tally = std::make_shared<Pipeline::WriteTally>();
	{
		Pipeline::FileWriterPool writer(3, 1ull << 30);
		std::vector<std::thread> producers;
		for (int p = 0; p < 4; ++p) {
			producers.emplace_back([&, p] {
				for (int i = 0; i < 50; ++i) {
					const auto id = p * 50 + i;
					writer.Submit(directory.Path() / (std::to_string(id) + ".bin"), std::vector<std::uint8_t>(static_cast<std::size_t>(id), static_cast<std::uint8_t>(id)), tally);
				}
			});
		}
		for (auto& producer : producers) {
			producer.join();
		}
		writer.Drain();
		EXPECT_EQ(writer.PendingFiles(), 0u);
	}
	EXPECT_EQ(tally->written.load(), 200u);
	EXPECT_EQ(tally->failed.load(), 0u);
	for (int id = 0; id < 200; ++id) {
		const auto bytes = Tests::ReadAll(directory.Path() / (std::to_string(id) + ".bin"));
		ASSERT_EQ(bytes.size(), static_cast<std::size_t>(id));
		for (const auto byte : bytes) {
			ASSERT_EQ(byte, static_cast<std::uint8_t>(id));
		}
	}
}

TEST(FileWriterPool, AdmitsAgainOnceTheBacklogDrains)
{
	TempDirectory directory;
	auto tally = std::make_shared<Pipeline::WriteTally>();
	std::atomic<std::uint64_t> woken{ 0 };
	{
		// Budget of one byte: any queued file puts the writer over it.
		Pipeline::FileWriterPool writer(1, 1);
		const auto notify = [&](std::uint64_t a_identity) {
			woken.store(a_identity);
			woken.notify_all();
		};
		EXPECT_EQ(writer.AdmitOrWait(notify), 0u);
		for (int i = 0; i < 20; ++i) {
			writer.Submit(directory.Path() / (std::to_string(i) + ".bin"), std::vector<std::uint8_t>(4096, 1), tally);
		}
		// Either the files are already written (admitted) or the waiter is woken once they are.
		if (const auto identity = writer.AdmitOrWait(notify); identity != 0) {
			for (auto seen = woken.load(); seen != identity; seen = woken.load()) {
				woken.wait(seen);
			}
		}
		writer.Drain();
	}
	EXPECT_EQ(tally->written.load(), 20u);
}

TEST(FileWriterPool, FailedWritesAreCounted)
{
	TempDirectory directory;
	auto tally = std::make_shared<Pipeline::WriteTally>();
	{
		Pipeline::FileWriterPool writer(1, 1ull << 20);
		writer.Submit(directory.Path() / "no-such-folder" / "file.bin", { 1, 2, 3 }, tally);
	}
	EXPECT_EQ(tally->written.load(), 0u);
	EXPECT_EQ(tally->failed.load(), 1u);
}

TEST(FileWriterPool, WritesWholeFilesOrNothing)
{
	TempDirectory directory;
	const auto path = directory.Path() / "cell.cgid";
	Tests::WriteText(path, "old");
	Platform::WriteWholeFile(path, std::vector<std::uint8_t>{ 1, 2, 3, 4 });
	EXPECT_EQ(Tests::ReadAll(path), (std::vector<std::uint8_t>{ 1, 2, 3, 4 }));
	// Written beside the target and renamed over it: nothing is left under another name.
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.Path()), std::filesystem::directory_iterator{}), 1);

	// A file that cannot be created leaves neither it nor a temporary behind.
	const auto missing = directory.Path() / "missing" / "cell.cgid";
	EXPECT_THROW(Platform::WriteWholeFile(missing, std::vector<std::uint8_t>{ 1 }), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(missing.parent_path()));

	// Nor does one whose rename fails (a folder holds the name).
	const auto folder = directory.Path() / "folder.cgid";
	std::filesystem::create_directory(folder);
	EXPECT_THROW(Platform::WriteWholeFile(folder, std::vector<std::uint8_t>{ 1 }), std::runtime_error);
	EXPECT_TRUE(std::filesystem::is_directory(folder));
	EXPECT_FALSE(std::filesystem::exists(directory.Path() / "folder.cgid.tmp"));
}
