#include "Options.h"

namespace FasterNGIO::App
{
	std::string_view PlacementName(Grass::PlacementMode a_mode)
	{
		return a_mode == Grass::PlacementMode::Vanilla ? "vanilla" : "smooth";
	}

	std::optional<Grass::PlacementMode> ParsePlacement(std::string_view a_name)
	{
		if (a_name == "vanilla") {
			return Grass::PlacementMode::Vanilla;
		}
		if (a_name == "smooth") {
			return Grass::PlacementMode::Smooth;
		}
		return std::nullopt;
	}

	std::string_view RejectChoiceName(RejectChoice a_choice)
	{
		switch (a_choice) {
		case RejectChoice::Gpu:
			return "gpu";
		case RejectChoice::Cpu:
			return "cpu";
		case RejectChoice::None:
			return "none";
		case RejectChoice::Auto:
		default:
			return "auto";
		}
	}

	std::optional<RejectChoice> ParseRejectChoice(std::string_view a_name)
	{
		for (const auto choice : { RejectChoice::Auto, RejectChoice::Gpu, RejectChoice::Cpu, RejectChoice::None }) {
			if (a_name == RejectChoiceName(choice)) {
				return choice;
			}
		}
		return std::nullopt;
	}
}
