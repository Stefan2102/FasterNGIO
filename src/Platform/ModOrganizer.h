#pragma once

#include "Platform/GameInstall.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FasterNGIO::Platform
{
	// The Mod Organizer 2 instance and profile this process was started from.
	struct Mo2Instance
	{
		// Empty for the portable instance.
		std::string instanceName;
		std::filesystem::path instanceDirectory;
		std::string profileName;
		std::filesystem::path profileDirectory;
		// The instance's game. pluginsTxt is the profile's, and iniDirectory is the profile when it
		// keeps its own INIs: both right for any store, and readable with or without the VFS.
		GameInstall game;

		[[nodiscard]] std::string Describe() const;
	};

	// Whether an MO2 profile keeps its own game INIs (settings.ini [General] LocalSettings=true).
	[[nodiscard]] bool Mo2ProfileUsesLocalIni(const std::filesystem::path& a_profile);

	// A value as Qt's QSettings writes it to an INI file: @ByteArray(...) unwrapped, surrounding
	// quotes removed and its backslash escapes (\\, \", \0, \t, \n, \r, \xHH...) decoded.
	[[nodiscard]] std::string ParseQtIniValue(std::string_view a_value);

	// Reads the instance whose ModOrganizer.ini is in a_instanceDirectory and validates its game and
	// selected profile. The error is a sentence for the user.
	[[nodiscard]] std::expected<Mo2Instance, std::string> ReadMo2Instance(const std::filesystem::path& a_instanceDirectory, std::string a_instanceName);

	// Mod Organizer 2's install folder when this process runs inside its virtual filesystem (MO2
	// injects usvfs_x64.dll into everything it starts), else nullopt. Always nullopt off Windows.
	[[nodiscard]] std::optional<std::filesystem::path> ModOrganizerDirectory();

	// nullopt when not started from Mod Organizer 2. Otherwise its instance, found from MO2's current
	// instance (or the portable one) and checked against the load order the VFS shows the game, or
	// the reason it could not be read.
	[[nodiscard]] std::optional<std::expected<Mo2Instance, std::string>> DetectModOrganizer();
}
