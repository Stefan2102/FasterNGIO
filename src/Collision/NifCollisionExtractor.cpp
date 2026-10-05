#include "Collision/NifCollisionExtractor.h"

#include <NifFile.hpp>
#include <bhk.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <spanstream>
#include <string_view>
#include <unordered_map>

namespace FasterNGIO::Collision
{
	namespace
	{
		// Column-vector affine transform: p' = linear * p + translation.
		struct Affine
		{
			float m[3][4]{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

			[[nodiscard]] Float3 Point(float a_x, float a_y, float a_z) const
			{
				return {
					m[0][0] * a_x + m[0][1] * a_y + m[0][2] * a_z + m[0][3],
					m[1][0] * a_x + m[1][1] * a_y + m[1][2] * a_z + m[1][3],
					m[2][0] * a_x + m[2][1] * a_y + m[2][2] * a_z + m[2][3],
				};
			}

			[[nodiscard]] Float3 Direction(float a_x, float a_y, float a_z) const
			{
				return {
					m[0][0] * a_x + m[0][1] * a_y + m[0][2] * a_z,
					m[1][0] * a_x + m[1][1] * a_y + m[1][2] * a_z,
					m[2][0] * a_x + m[2][1] * a_y + m[2][2] * a_z,
				};
			}

			// Transforms here are similarity transforms, so any column's length is the scale.
			[[nodiscard]] float UniformScale() const { return std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]); }

			[[nodiscard]] Affine operator*(const Affine& a_rhs) const
			{
				Affine result;
				for (int row = 0; row < 3; ++row) {
					for (int col = 0; col < 4; ++col) {
						float value = col == 3 ? m[row][3] : 0.0f;
						for (int k = 0; k < 3; ++k) {
							value += m[row][k] * a_rhs.m[k][col];
						}
						result.m[row][col] = value;
					}
				}
				return result;
			}

			[[nodiscard]] static Affine Scale(float a_x, float a_y, float a_z)
			{
				Affine result;
				result.m[0][0] = a_x;
				result.m[1][1] = a_y;
				result.m[2][2] = a_z;
				return result;
			}

			[[nodiscard]] static Affine FromNiTransform(const nifly::MatTransform& a_transform)
			{
				Affine result;
				for (int row = 0; row < 3; ++row) {
					for (int col = 0; col < 3; ++col) {
						result.m[row][col] = a_transform.rotation[row][col] * a_transform.scale;
					}
					result.m[row][3] = a_transform.translation[row];
				}
				return result;
			}

			[[nodiscard]] static Affine FromQuaternion(const nifly::QuaternionXYZW& a_q, float a_tx, float a_ty, float a_tz)
			{
				const float x = a_q.x, y = a_q.y, z = a_q.z, w = a_q.w;
				Affine result;
				result.m[0][0] = 1 - 2 * (y * y + z * z);
				result.m[0][1] = 2 * (x * y - z * w);
				result.m[0][2] = 2 * (x * z + y * w);
				result.m[1][0] = 2 * (x * y + z * w);
				result.m[1][1] = 1 - 2 * (x * x + z * z);
				result.m[1][2] = 2 * (y * z - x * w);
				result.m[2][0] = 2 * (x * z - y * w);
				result.m[2][1] = 2 * (y * z + x * w);
				result.m[2][2] = 1 - 2 * (x * x + y * y);
				result.m[0][3] = a_tx;
				result.m[1][3] = a_ty;
				result.m[2][3] = a_tz;
				return result;
			}

			// hkTransform memory layout: three rotation columns, then the translation column.
			[[nodiscard]] static Affine FromHavokMatrix(const nifly::Matrix4& a_matrix)
			{
				Affine result;
				for (int row = 0; row < 3; ++row) {
					for (int col = 0; col < 4; ++col) {
						result.m[row][col] = a_matrix[col * 4 + row];
					}
				}
				return result;
			}
		};

		[[nodiscard]] float Dot(const Float3& a_lhs, const Float3& a_rhs)
		{
			return a_lhs.x * a_rhs.x + a_lhs.y * a_rhs.y + a_lhs.z * a_rhs.z;
		}

		[[nodiscard]] Float3 Sub(const Float3& a_lhs, const Float3& a_rhs)
		{
			return { a_lhs.x - a_rhs.x, a_lhs.y - a_rhs.y, a_lhs.z - a_rhs.z };
		}

		[[nodiscard]] Float3 Cross(const Float3& a_lhs, const Float3& a_rhs)
		{
			return { a_lhs.y * a_rhs.z - a_lhs.z * a_rhs.y, a_lhs.z * a_rhs.x - a_lhs.x * a_rhs.z, a_lhs.x * a_rhs.y - a_lhs.y * a_rhs.x };
		}

		[[nodiscard]] Float3 Normalize(const Float3& a_value)
		{
			const auto length = std::sqrt(Dot(a_value, a_value));
			return length > 1.0e-12f ? Float3{ a_value.x / length, a_value.y / length, a_value.z / length } : Float3{};
		}

		class Extractor
		{
		public:
			Extractor(nifly::NifFile& a_nif, const ExtractionOptions& a_options, CollisionModel& a_model) :
				_nif(a_nif), _header(a_nif.GetHeader()), _options(a_options), _model(a_model)
			{
				for (std::uint32_t id = 0; id < _header.GetNumBlocks(); ++id) {
					if (auto* node = _header.GetBlock<nifly::NiNode>(id)) {
						for (auto& child : node->childRefs) {
							if (!child.IsEmpty()) {
								_parents.try_emplace(child.index, id);
							}
						}
					}
				}
				_root = _nif.GetRootNode() ? _header.GetBlockID(_nif.GetRootNode()) : nifly::NIF_NPOS;
			}

			void Run()
			{
				for (std::uint32_t id = 0; id < _header.GetNumBlocks(); ++id) {
					auto* object = _header.GetBlock<nifly::NiAVObject>(id);
					if (!object || object->collisionRef.IsEmpty()) {
						continue;
					}
					auto* collision = _header.GetBlock<nifly::bhkNiCollisionObject>(object->collisionRef.index);
					if (!collision) {
						continue;
					}
					++_model.stats.collisionObjects;
					auto* body = _header.GetBlock<nifly::bhkRigidBody>(collision->bodyRef.index);
					if (_options.trace) {
						const auto& t = object->GetTransformToParent();
						auto* bodyObject = _header.GetBlock<nifly::NiObject>(collision->bodyRef.index);
						Trace(std::format(
							"object #{} {} '{}' root={} local t=({:.1f},{:.1f},{:.1f}) s={:.3f}; collision {} body #{} {} layer={}",
							id,
							object->GetBlockName(),
							object->name.get(),
							id == _root,
							t.translation.x,
							t.translation.y,
							t.translation.z,
							t.scale,
							collision->GetBlockName(),
							collision->bodyRef.index,
							bodyObject ? bodyObject->GetBlockName() : "<none>",
							body ? static_cast<int>(body->collisionFilter.layer) : -1));
						if (body) {
							Trace(std::format(
								"  body translation=({:.4f},{:.4f},{:.4f}) rotation=({:.3f},{:.3f},{:.3f},{:.3f}) motion={} quality={}",
								body->translation.x,
								body->translation.y,
								body->translation.z,
								body->rotation.x,
								body->rotation.y,
								body->rotation.z,
								body->rotation.w,
								static_cast<int>(body->motionSystem),
								static_cast<int>(body->qualityType)));
						}
					}
					if (!body) {
						++_model.stats.nonRigidBodies;
						continue;
					}
					const auto layer = static_cast<std::uint32_t>(body->collisionFilter.layer & 0x7Fu);
					if (layer >= 32 || (_options.layerMask & (1u << layer)) == 0) {
						++_model.stats.bodiesFilteredByLayer;
						continue;
					}
					++_model.stats.bodiesKept;

					// bhkRigidBodyT places the body relative to its node; plain bhkRigidBody follows the node.
					Affine bodyTransform = NodeToModel(id);
					if (dynamic_cast<nifly::bhkRigidBodyT*>(body)) {
						bodyTransform = bodyTransform * Affine::FromQuaternion(
															body->rotation,
															body->translation.x * kHavokToGame,
															body->translation.y * kHavokToGame,
															body->translation.z * kHavokToGame);
					}
					AddShape(body->shapeRef.index, bodyTransform * Affine::Scale(kHavokToGame, kHavokToGame, kHavokToGame), 0);
				}
			}

			void ComputeRenderBounds()
			{
				constexpr auto inf = (std::numeric_limits<float>::max)();
				Float3 lo{ inf, inf, inf };
				Float3 hi{ -inf, -inf, -inf };
				std::vector<nifly::Vector3> vertices;
				for (auto* shape : _nif.GetShapes()) {
					vertices.clear();
					if (!_nif.GetVertsForShape(shape, vertices) || vertices.empty()) {
						continue;
					}
					const auto toModel = NodeToModel(_header.GetBlockID(shape));
					for (const auto& v : vertices) {
						const auto p = toModel.Point(v.x, v.y, v.z);
						lo = { (std::min)(lo.x, p.x), (std::min)(lo.y, p.y), (std::min)(lo.z, p.z) };
						hi = { (std::max)(hi.x, p.x), (std::max)(hi.y, p.y), (std::max)(hi.z, p.z) };
					}
				}
				if (lo.x <= hi.x) {
					_model.renderAabbMin = lo;
					_model.renderAabbMax = hi;
				}
			}

			void ComputeRenderShapes()
			{
				if (auto* root = _nif.GetRootNode()) {
					_model.rootName = root->name.get();
				}
				std::vector<nifly::Vector3> vertices;
				for (auto* shape : _nif.GetShapes()) {
					vertices.clear();
					if (!_nif.GetVertsForShape(shape, vertices) || vertices.empty()) {
						continue;
					}
					const auto toModel = NodeToModel(_header.GetBlockID(shape));
					auto& out = _model.renderShapes.emplace_back();
					out.name = shape->name.get();
					out.vertices.reserve(vertices.size());
					for (const auto& v : vertices) {
						out.vertices.push_back(toModel.Point(v.x, v.y, v.z));
					}
				}
			}

			// Replaces the collision with the triangles of the visible, solid render shapes. Models
			// whose render shapes give no triangles keep their collision.
			void UseRenderGeometry()
			{
				std::vector<Triangle> triangles;
				std::vector<nifly::Vector3> vertices;
				std::vector<nifly::Triangle> indices;
				for (auto* shape : _nif.GetShapes()) {
					const auto id = _header.GetBlockID(shape);
					if (!SolidRenderShape(*shape, id)) {
						continue;
					}
					vertices.clear();
					indices.clear();
					if (!_nif.GetVertsForShape(shape, vertices) || vertices.empty() || !shape->GetTriangles(indices)) {
						continue;
					}
					const auto toModel = NodeToModel(id);
					std::vector<Float3> points;
					points.reserve(vertices.size());
					for (const auto& v : vertices) {
						points.push_back(toModel.Point(v.x, v.y, v.z));
					}
					for (const auto& tri : indices) {
						if (tri.p1 < points.size() && tri.p2 < points.size() && tri.p3 < points.size()) {
							triangles.push_back(Triangle{ .vertices = { points[tri.p1], points[tri.p2], points[tri.p3] } });
						}
					}
				}
				if (triangles.empty()) {
					return;
				}
				_model.triangles = std::move(triangles);
				_model.hulls.clear();
				_model.hullPlanes.clear();
				_model.hullTriangles.clear();
				_model.capsules.clear();
				_model.stats.renderGeometry = true;
			}

		private:
			// The engine replaces the root node's transform with the reference's, so it is skipped.
			[[nodiscard]] Affine NodeToModel(std::uint32_t a_blockID) const
			{
				Affine result;
				auto current = a_blockID;
				while (current != _root && current != nifly::NIF_NPOS) {
					if (auto* object = _header.GetBlock<nifly::NiAVObject>(current)) {
						result = Affine::FromNiTransform(object->GetTransformToParent()) * result;
					}
					const auto parentIt = _parents.find(current);
					if (parentIt == _parents.end()) {
						break;
					}
					current = parentIt->second;
				}
				return result;
			}

			// A shape that is drawn as an opaque or alpha-tested surface: not hidden (itself or a
			// parent), not skinned, and lit by the lighting shader without the decal flags. Effect,
			// water and sky shapes, decals and editor markers are skipped.
			[[nodiscard]] bool SolidRenderShape(nifly::NiShape& a_shape, std::uint32_t a_blockID) const
			{
				if (a_shape.IsSkinned()) {
					return false;
				}
				for (auto current = a_blockID; current != nifly::NIF_NPOS;) {
					if (auto* object = _header.GetBlock<nifly::NiAVObject>(current)) {
						if ((object->flags & 1u) != 0 || std::string_view(object->name.get()).starts_with("EditorMarker")) {
							return false;
						}
					}
					const auto parentIt = _parents.find(current);
					current = parentIt == _parents.end() ? nifly::NIF_NPOS : parentIt->second;
				}
				auto* shader = _nif.GetShader(std::addressof(a_shape));
				if (!shader) {
					return true;
				}
				auto* lighting = dynamic_cast<nifly::BSLightingShaderProperty*>(shader);
				if (!lighting) {
					return false;
				}
				return (lighting->shaderFlags1 & (nifly::SLSF1_DECAL | nifly::SLSF1_DYNAMIC_DECAL)) == 0;
			}

			void Count(ShapeType a_type) { ++_model.stats.shapes[static_cast<std::size_t>(a_type)]; }

			void Trace(std::string a_line) { _model.stats.trace.push_back(std::move(a_line)); }

			void AddShape(std::uint32_t a_shapeID, const Affine& a_transform, int a_depth)
			{
				if (a_shapeID == nifly::NIF_NPOS || a_depth > 32) {
					return;
				}
				auto* shape = _header.GetBlock<nifly::NiObject>(a_shapeID);
				if (!shape) {
					return;
				}
				if (_options.trace) {
					const auto origin = a_transform.Point(0, 0, 0);
					Trace(std::format(
						"{}shape #{} {} origin=({:.1f},{:.1f},{:.1f}) scale={:.3f}",
						std::string(static_cast<std::size_t>(a_depth + 1) * 2, ' '),
						a_shapeID,
						shape->GetBlockName(),
						origin.x,
						origin.y,
						origin.z,
						a_transform.UniformScale()));
				}

				if (auto* mopp = dynamic_cast<nifly::bhkMoppBvTreeShape*>(shape)) {
					Count(ShapeType::Mopp);
					AddShape(mopp->shapeRef.index, a_transform, a_depth + 1);
				} else if (auto* list = dynamic_cast<nifly::bhkListShape*>(shape)) {
					Count(ShapeType::List);
					for (auto& child : list->subShapeRefs) {
						AddShape(child.index, a_transform, a_depth + 1);
					}
				} else if (auto* convexList = dynamic_cast<nifly::bhkConvexListShape*>(shape)) {
					Count(ShapeType::ConvexList);
					for (auto& child : convexList->shapeRefs) {
						AddShape(child.index, a_transform, a_depth + 1);
					}
				} else if (auto* transformShape = dynamic_cast<nifly::bhkTransformShape*>(shape)) {
					Count(ShapeType::Transform);
					AddShape(transformShape->shapeRef.index, a_transform * Affine::FromHavokMatrix(transformShape->xform), a_depth + 1);
				} else if (auto* cms = dynamic_cast<nifly::bhkCompressedMeshShape*>(shape)) {
					Count(ShapeType::CompressedMesh);
					AddCompressedMesh(*cms, a_transform);
				} else if (auto* packed = dynamic_cast<nifly::bhkPackedNiTriStripsShape*>(shape)) {
					Count(ShapeType::PackedStrips);
					AddPackedStrips(*packed, a_transform);
				} else if (auto* convex = dynamic_cast<nifly::bhkConvexVerticesShape*>(shape)) {
					Count(ShapeType::ConvexVertices);
					std::vector<Float3> vertices;
					vertices.reserve(convex->verts.size());
					for (const auto& v : convex->verts) {
						vertices.push_back(a_transform.Point(v.x, v.y, v.z));
					}
					std::vector<Float3> normals;
					normals.reserve(convex->normals.size());
					for (const auto& n : convex->normals) {
						normals.push_back(Normalize(a_transform.Direction(n.x, n.y, n.z)));
					}
					AddHull(vertices, normals, convex->radius * a_transform.UniformScale());
				} else if (auto* box = dynamic_cast<nifly::bhkBoxShape*>(shape)) {
					Count(ShapeType::Box);
					const auto& d = box->dimensions;
					std::vector<Float3> vertices;
					for (int corner = 0; corner < 8; ++corner) {
						vertices.push_back(a_transform.Point((corner & 1) ? d.x : -d.x, (corner & 2) ? d.y : -d.y, (corner & 4) ? d.z : -d.z));
					}
					std::vector<Float3> normals{
						Normalize(a_transform.Direction(1, 0, 0)),
						Normalize(a_transform.Direction(-1, 0, 0)),
						Normalize(a_transform.Direction(0, 1, 0)),
						Normalize(a_transform.Direction(0, -1, 0)),
						Normalize(a_transform.Direction(0, 0, 1)),
						Normalize(a_transform.Direction(0, 0, -1)),
					};
					AddHull(vertices, normals, box->radius * a_transform.UniformScale());
				} else if (auto* capsule = dynamic_cast<nifly::bhkCapsuleShape*>(shape)) {
					Count(ShapeType::Capsule);
					_model.capsules.push_back(Capsule{
						.p0 = a_transform.Point(capsule->point1.x, capsule->point1.y, capsule->point1.z),
						.p1 = a_transform.Point(capsule->point2.x, capsule->point2.y, capsule->point2.z),
						.radius = capsule->radius * a_transform.UniformScale(),
					});
				} else if (auto* sphere = dynamic_cast<nifly::bhkSphereShape*>(shape)) {
					Count(ShapeType::Sphere);
					const auto center = a_transform.Point(0, 0, 0);
					_model.capsules.push_back(Capsule{ .p0 = center, .p1 = center, .radius = sphere->radius * a_transform.UniformScale() });
				} else if (auto* multiSphere = dynamic_cast<nifly::bhkMultiSphereShape*>(shape)) {
					Count(ShapeType::MultiSphere);
					for (const auto& s : multiSphere->spheres) {
						const auto center = a_transform.Point(s.center.x, s.center.y, s.center.z);
						_model.capsules.push_back(Capsule{ .p0 = center, .p1 = center, .radius = s.radius * a_transform.UniformScale() });
					}
				} else {
					Count(ShapeType::Unsupported);
					_model.stats.unsupportedShapes.emplace_back(shape->GetBlockName());
				}
			}

			void AddTriangle(const Float3& a_v0, const Float3& a_v1, const Float3& a_v2, float a_radius)
			{
				_model.triangles.push_back(Triangle{ .vertices = { a_v0, a_v1, a_v2 }, .radius = a_radius });
			}

			void AddCompressedMesh(nifly::bhkCompressedMeshShape& a_shape, const Affine& a_transform)
			{
				auto* data = _header.GetBlock<nifly::bhkCompressedMeshShapeData>(a_shape.dataRef.index);
				if (!data) {
					return;
				}
				const auto radius = a_shape.radius * a_transform.UniformScale();

				std::vector<Float3> bigVertices;
				bigVertices.reserve(data->bigVerts.size());
				for (const auto& v : data->bigVerts) {
					bigVertices.push_back(a_transform.Point(v.x, v.y, v.z));
				}
				for (const auto& tri : data->bigTris) {
					if (tri.triangle1 < bigVertices.size() && tri.triangle2 < bigVertices.size() && tri.triangle3 < bigVertices.size()) {
						AddTriangle(bigVertices[tri.triangle1], bigVertices[tri.triangle2], bigVertices[tri.triangle3], radius);
					}
				}

				// Chunk vertices are quantized to `error` steps relative to the chunk origin, then
				// placed by the chunk's transform.
				const auto quantum = data->error > 0.0f ? data->error : 0.001f;
				std::vector<Float3> chunkVertices;
				for (auto& chunk : data->chunks) {
					Affine chunkTransform = a_transform;
					if (chunk.transformIndex < data->transforms.size()) {
						const auto& t = data->transforms[chunk.transformIndex];
						chunkTransform = a_transform * Affine::FromQuaternion(t.rotation, t.translation.x, t.translation.y, t.translation.z);
					}
					chunkVertices.clear();
					for (std::uint32_t i = 0; i + 2 < chunk.verts.size(); i += 3) {
						chunkVertices.push_back(chunkTransform.Point(
							chunk.translation.x + static_cast<float>(chunk.verts[i]) * quantum,
							chunk.translation.y + static_cast<float>(chunk.verts[i + 1]) * quantum,
							chunk.translation.z + static_cast<float>(chunk.verts[i + 2]) * quantum));
					}
					const auto vertexAt = [&](std::uint16_t a_index) -> const Float3* {
						return a_index < chunkVertices.size() ? std::addressof(chunkVertices[a_index]) : nullptr;
					};

					std::uint32_t cursor = 0;
					for (const auto stripLength : chunk.strips) {
						for (std::uint32_t k = 0; k + 2 < stripLength && cursor + k + 2 < chunk.indices.size(); ++k) {
							const auto* a = vertexAt(chunk.indices[cursor + k]);
							const auto* b = vertexAt(chunk.indices[cursor + k + 1]);
							const auto* c = vertexAt(chunk.indices[cursor + k + 2]);
							if (a && b && c) {
								AddTriangle(*a, *b, *c, radius);
							}
						}
						cursor += stripLength;
					}
					for (; cursor + 2 < chunk.indices.size(); cursor += 3) {
						const auto* a = vertexAt(chunk.indices[cursor]);
						const auto* b = vertexAt(chunk.indices[cursor + 1]);
						const auto* c = vertexAt(chunk.indices[cursor + 2]);
						if (a && b && c) {
							AddTriangle(*a, *b, *c, radius);
						}
					}
				}
			}

			void AddPackedStrips(nifly::bhkPackedNiTriStripsShape& a_shape, const Affine& a_transform)
			{
				auto* data = _header.GetBlock<nifly::hkPackedNiTriStripsData>(a_shape.dataRef.index);
				if (!data) {
					return;
				}
				const auto& s = a_shape.scaling;
				const auto transform = a_transform * Affine::Scale(s.x != 0.0f ? s.x : 1.0f, s.y != 0.0f ? s.y : 1.0f, s.z != 0.0f ? s.z : 1.0f);
				const auto radius = a_shape.radius * a_transform.UniformScale();
				std::vector<Float3> vertices;
				vertices.reserve(data->compressedVertData.size());
				for (const auto& v : data->compressedVertData) {
					vertices.push_back(transform.Point(v.x, v.y, v.z));
				}
				const auto addTriangle = [&](const nifly::Triangle& a_tri) {
					if (a_tri.p1 < vertices.size() && a_tri.p2 < vertices.size() && a_tri.p3 < vertices.size()) {
						AddTriangle(vertices[a_tri.p1], vertices[a_tri.p2], vertices[a_tri.p3], radius);
					}
				};
				for (const auto& tri : data->triData) {
					addTriangle(tri.tri);
				}
				for (const auto& tri : data->triNormData) {
					addTriangle(tri.tri);
				}
			}

			// Builds a solid hull from its vertices and face normals. Plane offsets are recomputed from
			// the vertices, and each face is triangulated as a fan of the vertices lying on it.
			void AddHull(const std::vector<Float3>& a_vertices, const std::vector<Float3>& a_normals, float a_radius)
			{
				if (a_vertices.size() < 4 || a_normals.size() < 4) {
					return;
				}
				Hull hull;
				hull.radius = a_radius;
				hull.firstPlane = static_cast<std::uint32_t>(_model.hullPlanes.size());
				hull.firstTriangle = static_cast<std::uint32_t>(_model.hullTriangles.size());
				hull.aabbMin = a_vertices.front();
				hull.aabbMax = a_vertices.front();
				for (const auto& v : a_vertices) {
					hull.aabbMin = { (std::min)(hull.aabbMin.x, v.x), (std::min)(hull.aabbMin.y, v.y), (std::min)(hull.aabbMin.z, v.z) };
					hull.aabbMax = { (std::max)(hull.aabbMax.x, v.x), (std::max)(hull.aabbMax.y, v.y), (std::max)(hull.aabbMax.z, v.z) };
				}
				const auto extent = Sub(hull.aabbMax, hull.aabbMin);
				const auto epsilon = (std::max)(0.05f, 1.0e-3f * std::sqrt(Dot(extent, extent)));

				std::vector<Float3> uniqueNormals;
				for (const auto& normal : a_normals) {
					if (Dot(normal, normal) < 0.5f) {
						continue;
					}
					if (std::ranges::any_of(uniqueNormals, [&](const Float3& existing) { return Dot(existing, normal) > 0.99999f; })) {
						continue;
					}
					uniqueNormals.push_back(normal);
				}

				std::vector<std::pair<float, Float3>> face;
				for (const auto& normal : uniqueNormals) {
					float support = -(std::numeric_limits<float>::max)();
					for (const auto& v : a_vertices) {
						support = (std::max)(support, Dot(normal, v));
					}
					_model.hullPlanes.push_back(Float4{ normal.x, normal.y, normal.z, -support });

					face.clear();
					Float3 centroid{};
					for (const auto& v : a_vertices) {
						if (support - Dot(normal, v) <= epsilon) {
							face.emplace_back(0.0f, v);
							centroid = { centroid.x + v.x, centroid.y + v.y, centroid.z + v.z };
						}
					}
					if (face.size() < 3) {
						continue;
					}
					const auto inv = 1.0f / static_cast<float>(face.size());
					centroid = { centroid.x * inv, centroid.y * inv, centroid.z * inv };
					const auto axisU = Normalize(Sub(face.front().second, centroid));
					const auto axisV = Cross(normal, axisU);
					for (auto& [angle, v] : face) {
						const auto offset = Sub(v, centroid);
						angle = std::atan2(Dot(offset, axisV), Dot(offset, axisU));
					}
					std::ranges::sort(face, {}, &std::pair<float, Float3>::first);
					for (std::size_t i = 1; i + 1 < face.size(); ++i) {
						_model.hullTriangles.push_back(Triangle{ .vertices = { face[0].second, face[i].second, face[i + 1].second }, .radius = a_radius });
					}
				}
				hull.planeCount = static_cast<std::uint32_t>(_model.hullPlanes.size()) - hull.firstPlane;
				hull.triangleCount = static_cast<std::uint32_t>(_model.hullTriangles.size()) - hull.firstTriangle;
				if (hull.triangleCount > 0) {
					_model.hulls.push_back(hull);
				} else {
					_model.hullPlanes.resize(hull.firstPlane);
				}
			}

			nifly::NifFile& _nif;
			nifly::NiHeader& _header;
			const ExtractionOptions& _options;
			CollisionModel& _model;
			std::unordered_map<std::uint32_t, std::uint32_t> _parents;
			std::uint32_t _root{ nifly::NIF_NPOS };
		};

		void ComputeBounds(CollisionModel& a_model)
		{
			constexpr auto inf = (std::numeric_limits<float>::max)();
			Float3 lo{ inf, inf, inf };
			Float3 hi{ -inf, -inf, -inf };
			const auto include = [&](const Float3& a_point, float a_radius) {
				lo = { (std::min)(lo.x, a_point.x - a_radius), (std::min)(lo.y, a_point.y - a_radius), (std::min)(lo.z, a_point.z - a_radius) };
				hi = { (std::max)(hi.x, a_point.x + a_radius), (std::max)(hi.y, a_point.y + a_radius), (std::max)(hi.z, a_point.z + a_radius) };
			};
			for (const auto& tri : a_model.triangles) {
				for (const auto& v : tri.vertices) {
					include(v, tri.radius);
				}
			}
			for (const auto& hull : a_model.hulls) {
				include(hull.aabbMin, hull.radius);
				include(hull.aabbMax, hull.radius);
			}
			for (const auto& capsule : a_model.capsules) {
				include(capsule.p0, capsule.radius);
				include(capsule.p1, capsule.radius);
			}
			a_model.aabbMin = lo;
			a_model.aabbMax = hi;
		}
	}

	CollisionModel ExtractCollision(std::span<const std::uint8_t> a_nifBytes, const ExtractionOptions& a_options)
	{
		CollisionModel model;
		nifly::NifFile nif;
		try {
			std::ispanstream stream(std::span<const char>(reinterpret_cast<const char*>(a_nifBytes.data()), a_nifBytes.size()));
			if (nif.Load(stream) != 0) {
				model.status = ExtractionStatus::LoadFailed;
				return model;
			}
			Extractor extractor(nif, a_options, model);
			extractor.Run();
			if (a_options.renderBounds) {
				extractor.ComputeRenderBounds();
			}
			if (a_options.renderShapes) {
				extractor.ComputeRenderShapes();
			}
			if (a_options.renderGeometry && !model.Empty()) {
				extractor.UseRenderGeometry();
			}
		} catch (const std::exception&) {
			model = {};
			model.status = ExtractionStatus::LoadFailed;
			return model;
		}

		if (!model.Empty()) {
			model.status = ExtractionStatus::HasCollision;
			ComputeBounds(model);
		} else if (model.stats.collisionObjects > 0) {
			model.status = ExtractionStatus::FilteredOut;
		} else {
			model.status = ExtractionStatus::NoCollision;
		}
		return model;
	}

	const RenderShape* NearestRenderShape(const CollisionModel& a_model, const Float3& a_point)
	{
		const RenderShape* best = nullptr;
		float bestDistance = (std::numeric_limits<float>::max)();
		for (const auto& shape : a_model.renderShapes) {
			for (const auto& v : shape.vertices) {
				const float dx = v.x - a_point.x;
				const float dy = v.y - a_point.y;
				const float dz = v.z - a_point.z;
				const float distance = dx * dx + dy * dy + dz * dz;
				if (distance < bestDistance) {
					bestDistance = distance;
					best = std::addressof(shape);
				}
			}
		}
		return best;
	}

	bool RenderShapeNamed(const CollisionModel& a_model, const RenderShape& a_shape, std::string_view a_name)
	{
		if (a_shape.name == a_name) {
			return true;
		}
		const std::string_view root = a_model.rootName;
		return a_shape.name.size() == root.size() + 1 + a_name.size() && a_shape.name.starts_with(root) && a_shape.name[root.size()] == ':' &&
		       std::string_view(a_shape.name).substr(root.size() + 1) == a_name;
	}
}
