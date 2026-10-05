#include "Gpu/ModelPacking.h"

#include "Rejection/HlslShim.h"

#include <cstring>

namespace FasterNGIO::Gpu
{
	namespace
	{
		using Collision::Float3;
		using Rejection::kPrimitiveKindCount;
		using Rejection::PrimitiveKind;
		namespace Layout = Rejection::Hlsl;

		[[nodiscard]] std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		class Writer
		{
		public:
			explicit Writer(std::vector<std::byte>& a_bytes) :
				_bytes(a_bytes) {}

			[[nodiscard]] std::uint64_t Offset() const { return _bytes.size(); }
			void Align(std::uint64_t a_alignment) { _bytes.resize(AlignUp(_bytes.size(), a_alignment)); }

			template <class T>
			void Put(const T& a_value)
			{
				const auto offset = _bytes.size();
				_bytes.resize(offset + sizeof(T));
				std::memcpy(_bytes.data() + offset, &a_value, sizeof(T));
			}

			void PutPoint(const Float3& a_point)
			{
				Put(a_point.x);
				Put(a_point.y);
				Put(a_point.z);
			}

			template <class T>
			void PutAt(std::uint64_t a_offset, const T& a_value)
			{
				std::memcpy(_bytes.data() + a_offset, &a_value, sizeof(T));
			}

		private:
			std::vector<std::byte>& _bytes;
		};

		// The records PackModel writes, field by field, against RejectionLayout.hlsli.
		static_assert(Layout::kTriangleStride == (3 * 3 + 1) * sizeof(float));
		static_assert(Layout::kHullPlaneStride == 4 * sizeof(float));
		static_assert(Layout::kHullFaceStride == 3 * 3 * sizeof(float));
		static_assert(Layout::kCapsuleStride == (2 * 3 + 2) * sizeof(float));
		static_assert(Layout::kHullStride >= 5 * sizeof(std::uint32_t) && Layout::kModelHeaderBytes >= 6 * sizeof(std::uint32_t));

	}

	PackedModel PackModel(const Collision::CollisionModel& a_model, float a_inflation)
	{
		PackedModel packed;
		Writer out(packed.bytes);
		for (std::uint32_t i = 0; i < Layout::kModelHeaderBytes; i += 4) {
			out.Put(std::uint32_t{ 0 });
		}

		const auto trianglesOffset = out.Offset();
		for (const auto& tri : a_model.triangles) {
			for (const auto& v : tri.vertices) {
				out.PutPoint(v);
			}
			out.Put(tri.radius);
		}

		const auto hullsOffset = out.Offset();
		const auto hullRecords = out.Offset();
		for (std::size_t i = 0; i < a_model.hulls.size(); ++i) {
			for (std::uint32_t w = 0; w < Layout::kHullStride; w += 4) {
				out.Put(std::uint32_t{ 0 });
			}
		}
		for (std::size_t i = 0; i < a_model.hulls.size(); ++i) {
			const auto& hull = a_model.hulls[i];
			out.Align(16);
			const auto planeOffset = out.Offset();
			for (std::uint32_t p = 0; p < hull.planeCount; ++p) {
				const auto& plane = a_model.hullPlanes[hull.firstPlane + p];
				out.Put(plane.x);
				out.Put(plane.y);
				out.Put(plane.z);
				out.Put(plane.w);
			}
			const auto triangleOffset = out.Offset();
			for (std::uint32_t t = 0; t < hull.triangleCount; ++t) {
				for (const auto& v : a_model.hullTriangles[hull.firstTriangle + t].vertices) {
					out.PutPoint(v);
				}
			}
			const auto record = hullRecords + i * Layout::kHullStride;
			out.PutAt(record + 0, static_cast<std::uint32_t>(planeOffset));
			out.PutAt(record + 4, hull.planeCount);
			out.PutAt(record + 8, static_cast<std::uint32_t>(triangleOffset));
			out.PutAt(record + 12, hull.triangleCount);
			out.PutAt(record + 16, hull.radius);
		}

		out.Align(16);
		const auto capsulesOffset = out.Offset();
		for (const auto& capsule : a_model.capsules) {
			out.PutPoint(capsule.p0);
			out.PutPoint(capsule.p1);
			out.Put(capsule.radius);
			out.Put(0.0f);
		}

		out.PutAt(Layout::kModelTrianglesOffset, static_cast<std::uint32_t>(trianglesOffset));
		out.PutAt(Layout::kModelTriangleCount, static_cast<std::uint32_t>(a_model.triangles.size()));
		out.PutAt(Layout::kModelHullsOffset, static_cast<std::uint32_t>(hullsOffset));
		out.PutAt(Layout::kModelHullCount, static_cast<std::uint32_t>(a_model.hulls.size()));
		out.PutAt(Layout::kModelCapsulesOffset, static_cast<std::uint32_t>(capsulesOffset));
		out.PutAt(Layout::kModelCapsuleCount, static_cast<std::uint32_t>(a_model.capsules.size()));

		for (std::uint32_t kind = 0; kind < kPrimitiveKindCount; ++kind) {
			const auto primitiveKind = static_cast<PrimitiveKind>(kind);
			const auto count = Rejection::PrimitiveCount(a_model, primitiveKind);
			if (count == 0) {
				continue;
			}
			out.Align(16);
			packed.geometries.push_back({ primitiveKind, out.Offset(), count });
			packed.kindMask |= 1u << kind;
			for (std::uint32_t i = 0; i < count; ++i) {
				out.Put(Rejection::PrimitiveBounds(a_model, primitiveKind, i, a_inflation));
			}
		}
		return packed;
	}
}
