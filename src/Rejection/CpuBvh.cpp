#include "Rejection/CpuBvh.h"

#include "Grass/Placement.h"
#include "Rejection/Bounds.h"
#include "Rejection/PrimitiveTests.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace FasterNGIO::Rejection
{
	namespace
	{
		constexpr std::uint32_t kKindShift = 30;
		constexpr std::uint32_t kIndexMask = (1u << kKindShift) - 1;
		constexpr std::uint32_t kBins = 16;
		constexpr std::uint32_t kStackDepth = 64;

		// Binned-SAH builder over primitive boxes. Children of an interior node are allocated as a
		// pair so traversal needs one index per node.
		class Builder
		{
		public:
			Builder(std::span<const Bounds> a_boxes, std::uint32_t a_maxLeaf, std::vector<BvhNode>& a_nodes, std::vector<std::uint32_t>& a_order) :
				_boxes(a_boxes), _maxLeaf(a_maxLeaf), _nodes(a_nodes), _order(a_order)
			{
				_order.resize(a_boxes.size());
				for (std::uint32_t i = 0; i < _order.size(); ++i) {
					_order[i] = i;
				}
				_nodes.clear();
				_nodes.reserve(a_boxes.size() * 2);
				_nodes.push_back({});
				Build(0, 0, static_cast<std::uint32_t>(_order.size()));
			}

		private:
			void SetBounds(BvhNode& a_node, const Bounds& a_box)
			{
				for (int axis = 0; axis < 3; ++axis) {
					a_node.min[axis] = a_box.min[axis];
					a_node.max[axis] = a_box.max[axis];
				}
			}

			void Build(std::uint32_t a_node, std::uint32_t a_first, std::uint32_t a_count)
			{
				Bounds bounds;
				Bounds centroids;
				for (std::uint32_t i = a_first; i < a_first + a_count; ++i) {
					const auto& box = _boxes[_order[i]];
					bounds.Grow(box);
					centroids.Grow(Float3{ box.Centroid(0), box.Centroid(1), box.Centroid(2) }, 0.0f);
				}
				SetBounds(_nodes[a_node], bounds);
				_nodes[a_node].first = a_first;
				_nodes[a_node].count = a_count;
				if (a_count <= _maxLeaf) {
					return;
				}

				// Pick the cheapest split plane over kBins bins on each axis.
				float bestCost = (std::numeric_limits<float>::max)();
				int bestAxis = -1;
				std::uint32_t bestBin = 0;
				for (int axis = 0; axis < 3; ++axis) {
					const float lo = centroids.min[axis];
					const float extent = centroids.max[axis] - lo;
					if (extent <= 0.0f) {
						continue;
					}
					std::array<Bounds, kBins> binBoxes{};
					std::array<std::uint32_t, kBins> binCounts{};
					const float scale = kBins / extent;
					for (std::uint32_t i = a_first; i < a_first + a_count; ++i) {
						const auto& box = _boxes[_order[i]];
						const auto bin = (std::min)(kBins - 1, static_cast<std::uint32_t>((box.Centroid(axis) - lo) * scale));
						binBoxes[bin].Grow(box);
						++binCounts[bin];
					}
					std::array<float, kBins - 1> leftCost{};
					Bounds left;
					std::uint32_t leftCount = 0;
					for (std::uint32_t b = 0; b + 1 < kBins; ++b) {
						left.Grow(binBoxes[b]);
						leftCount += binCounts[b];
						leftCost[b] = leftCount ? left.HalfArea() * static_cast<float>(leftCount) : 0.0f;
					}
					Bounds right;
					std::uint32_t rightCount = 0;
					for (std::uint32_t b = kBins - 1; b > 0; --b) {
						right.Grow(binBoxes[b]);
						rightCount += binCounts[b];
						if (rightCount == 0 || rightCount == a_count) {
							continue;
						}
						const float cost = leftCost[b - 1] + right.HalfArea() * static_cast<float>(rightCount);
						if (cost < bestCost) {
							bestCost = cost;
							bestAxis = axis;
							bestBin = b;
						}
					}
				}

				std::uint32_t split = a_first;
				if (bestAxis >= 0) {
					const float lo = centroids.min[bestAxis];
					const float scale = kBins / (centroids.max[bestAxis] - lo);
					const auto middle = std::partition(_order.begin() + a_first, _order.begin() + a_first + a_count, [&](std::uint32_t a_index) {
						return (std::min)(kBins - 1, static_cast<std::uint32_t>((_boxes[a_index].Centroid(bestAxis) - lo) * scale)) < bestBin;
					});
					split = static_cast<std::uint32_t>(middle - _order.begin());
				}
				if (split == a_first || split == a_first + a_count) {
					// Coincident centroids: split the range in half.
					split = a_first + a_count / 2;
				}

				const auto left = static_cast<std::uint32_t>(_nodes.size());
				_nodes.push_back({});
				_nodes.push_back({});
				_nodes[a_node].first = left;
				_nodes[a_node].count = 0;
				Build(left, a_first, split - a_first);
				Build(left + 1, split, a_first + a_count - split);
			}

			std::span<const Bounds> _boxes;
			std::uint32_t _maxLeaf;
			std::vector<BvhNode>& _nodes;
			std::vector<std::uint32_t>& _order;
		};

		struct QueryBox
		{
			float min[3];
			float max[3];
		};

		[[nodiscard]] QueryBox MakeQueryBox(const Hlsl::float3& a_p, const Hlsl::float3& a_q, float a_radius)
		{
			return {
				{ (std::min)(a_p.x, a_q.x) - a_radius, (std::min)(a_p.y, a_q.y) - a_radius, (std::min)(a_p.z, a_q.z) - a_radius },
				{ (std::max)(a_p.x, a_q.x) + a_radius, (std::max)(a_p.y, a_q.y) + a_radius, (std::max)(a_p.z, a_q.z) + a_radius },
			};
		}

		[[nodiscard]] bool Overlaps(const BvhNode& a_node, const QueryBox& a_box)
		{
			return a_node.min[0] <= a_box.max[0] && a_node.max[0] >= a_box.min[0] && a_node.min[1] <= a_box.max[1] && a_node.max[1] >= a_box.min[1] &&
			       a_node.min[2] <= a_box.max[2] && a_node.max[2] >= a_box.min[2];
		}

		// Visits every leaf range whose node overlaps a_box until a_leaf returns true.
		template <class Leaf>
		[[nodiscard]] bool Traverse(const std::vector<BvhNode>& a_nodes, const QueryBox& a_box, Leaf&& a_leaf)
		{
			if (a_nodes.empty() || !Overlaps(a_nodes[0], a_box)) {
				return false;
			}
			std::uint32_t stack[kStackDepth];
			std::uint32_t depth = 0;
			std::uint32_t node = 0;
			while (true) {
				const auto& current = a_nodes[node];
				if (current.count != 0) {
					if (a_leaf(current.first, current.count)) {
						return true;
					}
				} else {
					const bool left = Overlaps(a_nodes[current.first], a_box);
					const bool right = Overlaps(a_nodes[current.first + 1], a_box);
					if (left && right) {
						if (depth == kStackDepth) {
							throw std::runtime_error("CPU BVH deeper than its traversal stack");
						}
						stack[depth++] = current.first + 1;
						node = current.first;
						continue;
					}
					if (left || right) {
						node = left ? current.first : current.first + 1;
						continue;
					}
				}
				if (depth == 0) {
					return false;
				}
				node = stack[--depth];
			}
		}
	}

	CpuBvh::CpuBvh(const WorldIndex& a_world) :
		_world(a_world)
	{
		const auto begin = std::chrono::steady_clock::now();
		const auto& models = a_world.Models();
		const auto& instances = a_world.Instances();

		std::vector<std::uint8_t> used(models.size(), 0);
		for (const auto& instance : instances) {
			used[instance.model] = 1;
		}
		_models.resize(models.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, models.size(), [&](std::size_t m) {
			if (!used[m]) {
				return;
			}
			const auto& model = models[m].collision;
			std::vector<Bounds> boxes;
			std::vector<std::uint32_t> references;
			boxes.reserve(model.triangles.size() + model.hulls.size() + model.capsules.size());
			for (std::uint32_t kind = 0; kind < kPrimitiveKindCount; ++kind) {
				const auto primitiveKind = static_cast<PrimitiveKind>(kind);
				for (std::uint32_t i = 0; i < PrimitiveCount(model, primitiveKind); ++i) {
					boxes.push_back(PrimitiveBounds(model, primitiveKind, i));
					references.push_back((kind << kKindShift) | i);
				}
			}
			if (boxes.empty()) {
				return;
			}
			auto& bvh = _models[m];
			std::vector<std::uint32_t> order;
			Builder(boxes, 4, bvh.nodes, order);
			bvh.primitives.resize(order.size());
			for (std::size_t i = 0; i < order.size(); ++i) {
				bvh.primitives[i] = references[order[i]];
			}
		});

		std::vector<Bounds> instanceBoxes(instances.size());
		for (std::size_t i = 0; i < instances.size(); ++i) {
			instanceBoxes[i].Grow(instances[i].aabbMin, 0.0f);
			instanceBoxes[i].Grow(instances[i].aabbMax, 0.0f);
		}
		Builder(instanceBoxes, 2, _instanceNodes, _instanceOrder);

		for (std::size_t m = 0; m < _models.size(); ++m) {
			if (!_models[m].nodes.empty()) {
				++_stats.models;
				_stats.primitives += _models[m].primitives.size();
				_stats.modelNodes += _models[m].nodes.size();
			}
		}
		_stats.instances = instances.size();
		_stats.instanceNodes = _instanceNodes.size();
		_stats.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	}

	bool CpuBvh::CapsuleHitsInstance(const Instance& a_instance, const Float3& a_p, const Float3& a_q, float a_radius) const
	{
		const auto& bvh = _models[a_instance.model];
		const auto& model = _world.Models()[a_instance.model].collision;
		const auto p = ToHlsl(a_instance.modelFromWorld.Apply(a_p));
		const auto q = ToHlsl(a_instance.modelFromWorld.Apply(a_q));
		const auto r = a_radius * a_instance.modelFromWorld.scale;
		return Traverse(bvh.nodes, MakeQueryBox(p, q, r), [&](std::uint32_t a_first, std::uint32_t a_count) {
			for (auto i = a_first; i < a_first + a_count; ++i) {
				const auto reference = bvh.primitives[i];
				const auto index = reference & kIndexMask;
				switch (static_cast<PrimitiveKind>(reference >> kKindShift)) {
				case PrimitiveKind::Triangle:
					if (CapsuleOverlapsTriangle(model.triangles[index], p, q, r)) {
						return true;
					}
					break;
				case PrimitiveKind::Hull:
					if (CapsuleOverlapsHull(model, model.hulls[index], p, q, r)) {
						return true;
					}
					break;
				case PrimitiveKind::Capsule:
					if (CapsuleOverlapsCapsule(model.capsules[index], p, q, r)) {
						return true;
					}
					break;
				}
			}
			return false;
		});
	}

	bool CpuBvh::CapsuleHitsWorld(const Float3& a_p, const Float3& a_q, float a_radius) const
	{
		const auto& instances = _world.Instances();
		return Traverse(_instanceNodes, MakeQueryBox(ToHlsl(a_p), ToHlsl(a_q), a_radius), [&](std::uint32_t a_first, std::uint32_t a_count) {
			for (auto i = a_first; i < a_first + a_count; ++i) {
				if (CapsuleHitsInstance(instances[_instanceOrder[i]], a_p, a_q, a_radius)) {
					return true;
				}
			}
			return false;
		});
	}

	VolumeHits CpuBvh::ClassifyCapsule(const Float3& a_p, const Float3& a_q, float a_radius) const
	{
		VolumeHits hits;
		const bool worldHasCliffs = _world.Stats().cliffInstances != 0;
		const auto& instances = _world.Instances();
		(void)Traverse(_instanceNodes, MakeQueryBox(ToHlsl(a_p), ToHlsl(a_q), a_radius), [&](std::uint32_t a_first, std::uint32_t a_count) {
			for (auto i = a_first; i < a_first + a_count; ++i) {
				const auto index = _instanceOrder[i];
				const auto& instance = instances[index];
				const bool known = (instance.role == kRoleOrdinary && hits.ordinary) || (instance.role == kRoleCliff && hits.cliff) ||
				                   (instance.role == kRolePartIgnored && hits.partIgnored != kNoInstance);
				if (known || !CapsuleHitsInstance(instance, a_p, a_q, a_radius)) {
					continue;
				}
				switch (instance.role) {
				case kRoleOrdinary:
					hits.ordinary = true;
					break;
				case kRoleCliff:
					hits.cliff = true;
					break;
				case kRolePartIgnored:
					hits.partIgnored = index;
					break;
				}
				if (hits.ordinary && (hits.cliff || !worldHasCliffs)) {
					return true;
				}
			}
			return false;
		});
		return hits;
	}

	void CpuBvh::SegmentHitsWorld(const Float3& a_p, const Float3& a_q, std::vector<WorldSegmentHit>& a_hits) const
	{
		const auto& instances = _world.Instances();
		(void)Traverse(_instanceNodes, MakeQueryBox(ToHlsl(a_p), ToHlsl(a_q), 0.0f), [&](std::uint32_t a_first, std::uint32_t a_count) {
			for (auto i = a_first; i < a_first + a_count; ++i) {
				const auto index = _instanceOrder[i];
				const auto& instance = instances[index];
				const auto& bvh = _models[instance.model];
				const auto& model = _world.Models()[instance.model].collision;
				const auto p = ToHlsl(instance.modelFromWorld.Apply(a_p));
				const auto q = ToHlsl(instance.modelFromWorld.Apply(a_q));
				(void)Traverse(bvh.nodes, MakeQueryBox(p, q, 0.0f), [&](std::uint32_t a_firstPrimitive, std::uint32_t a_primitiveCount) {
					for (auto j = a_firstPrimitive; j < a_firstPrimitive + a_primitiveCount; ++j) {
						const auto reference = bvh.primitives[j];
						const auto primitive = reference & kIndexMask;
						Hlsl::SegmentHit hit{};
						switch (static_cast<PrimitiveKind>(reference >> kKindShift)) {
						case PrimitiveKind::Triangle:
							hit = SegmentHitTriangle(model.triangles[primitive], p, q);
							break;
						case PrimitiveKind::Hull:
							hit = SegmentHitHull(model, model.hulls[primitive], p, q);
							break;
						case PrimitiveKind::Capsule:
							hit = SegmentHitCapsule(model.capsules[primitive], p, q);
							break;
						}
						if (hit.hit) {
							a_hits.push_back(ToWorldHit(index, instance.worldFromModel, hit));
						}
					}
					return false;
				});
			}
			return false;
		});
	}

	std::vector<std::uint32_t> CpuBvh::RejectCell(const Grass::CellCandidates& a_cell, std::span<const QueryShape> a_shapes) const
	{
		std::vector<std::uint32_t> rejected((a_cell.blades.size() + 31) / 32, 0u);
		for (std::size_t i = 0; i < a_cell.blades.size(); ++i) {
			const auto& blade = a_cell.blades[i];
			const auto& shape = a_shapes[blade.groupIndex];
			const auto [p, q] = BladeSegment(shape, blade.position);
			if (CapsuleHitsWorld(p, q, shape.radius)) {
				rejected[i / 32] |= 1u << (i % 32);
			}
		}
		return rejected;
	}
}
