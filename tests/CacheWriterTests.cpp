#include "Grass/NgioCacheWriter.h"
#include "Pipeline/CacheWriter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace FasterNGIO;

	std::filesystem::path MakeTempDirectory()
	{
		static std::atomic<int> counter{ 0 };
		auto path = std::filesystem::temp_directory_path() / ("fasterngio-writer-test-" + std::to_string(counter++) + "-" + std::to_string(std::rand()));
		std::filesystem::create_directories(path);
		return path;
	}

	std::vector<std::uint8_t> ReadAll(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}
}

TEST(CacheWriter, WritesEveryFileFromManyProducers)
{
	const auto directory = MakeTempDirectory();
	auto tally = std::make_shared<Pipeline::WriteTally>();
	{
		Pipeline::CacheWriter writer(3, 1ull << 30);
		std::vector<std::thread> producers;
		for (int p = 0; p < 4; ++p) {
			producers.emplace_back([&, p] {
				for (int i = 0; i < 50; ++i) {
					const auto id = p * 50 + i;
					writer.Submit(directory / (std::to_string(id) + ".bin"), std::vector<std::uint8_t>(static_cast<std::size_t>(id), static_cast<std::uint8_t>(id)), tally);
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
		const auto bytes = ReadAll(directory / (std::to_string(id) + ".bin"));
		ASSERT_EQ(bytes.size(), static_cast<std::size_t>(id));
		for (const auto byte : bytes) {
			ASSERT_EQ(byte, static_cast<std::uint8_t>(id));
		}
	}
	std::filesystem::remove_all(directory);
}

TEST(CacheWriter, AdmitsAgainOnceTheBacklogDrains)
{
	const auto directory = MakeTempDirectory();
	auto tally = std::make_shared<Pipeline::WriteTally>();
	std::atomic<std::uint64_t> woken{ 0 };
	{
		// Budget of one byte: any queued file puts the writer over it.
		Pipeline::CacheWriter writer(1, 1);
		const auto notify = [&](std::uint64_t a_identity) {
			woken.store(a_identity);
			woken.notify_all();
		};
		EXPECT_EQ(writer.AdmitOrWait(notify), 0u);
		for (int i = 0; i < 20; ++i) {
			writer.Submit(directory / (std::to_string(i) + ".bin"), std::vector<std::uint8_t>(4096, 1), tally);
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
	std::filesystem::remove_all(directory);
}

TEST(CacheWriter, FailedWritesAreCounted)
{
	auto tally = std::make_shared<Pipeline::WriteTally>();
	{
		Pipeline::CacheWriter writer(1, 1ull << 20);
		writer.Submit(std::filesystem::temp_directory_path() / "fasterngio-no-such-folder" / "x" / "file.bin", { 1, 2, 3 }, tally);
	}
	EXPECT_EQ(tally->written.load(), 0u);
	EXPECT_EQ(tally->failed.load(), 1u);
}

TEST(NgioCacheWriter, SerializesTheSameBytesItWrites)
{
	Grass::NgioCellCache cache;
	auto& group = cache.groups.emplace_back();
	group.modelPath = "meshes\\grass\\a.nif";
	group.grassFormID = 0x12345678;
	group.fitToSlope = true;
	auto& block = group.blocks.emplace_back();
	block.descriptorWords.fill(0xA1B2C3D4u);
	block.payloadWords = { 0x0102, 0xFFEE };

	const auto bytes = Grass::SerializeNgioCellCache(cache);
	ASSERT_EQ(bytes.size(), 4u + 4u + group.modelPath.size() + 1u + 4u + 4u + 3u + 4u + 9u * 4u + 2u * 2u);
	EXPECT_EQ(bytes[0], 1u);
	EXPECT_EQ(bytes[bytes.size() - 4], 0x02u);
	EXPECT_EQ(bytes[bytes.size() - 3], 0x01u);
	EXPECT_EQ(bytes[bytes.size() - 2], 0xEEu);
	EXPECT_EQ(bytes[bytes.size() - 1], 0xFFu);

	const auto path = std::filesystem::temp_directory_path() / "fasterngio_serialize.cgid";
	Grass::WriteNgioCellCache(path, cache);
	EXPECT_EQ(ReadAll(path), bytes);
	std::filesystem::remove(path);
}
