#include "Gpu/GpuRejector.h"

#include "Concurrency/MpscQueue.h"
#include "Gpu/GpuShaders.h"
#include "Gpu/ModelPacking.h"
#include "Grass/Placement.h"
#include "Rejection/Bounds.h"
#include "Rejection/HlslShim.h"
#include "Rejection/WorldIndex.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/Runtime/RuntimeDevice.h>
#include <Render/Runtime/ThreadPoolTaskService.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>
#include <rhi_feature_info.h>
#include <rhi_helpers.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <variant>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace FasterNGIO::Gpu
{
	namespace
	{
		using Collision::CollisionModel;
		using Collision::Float3;

		using Rejection::Bounds;
		using Rejection::kPrimitiveKindCount;
		using Rejection::PrimitiveKind;
		namespace Layout = Rejection::Hlsl;

		constexpr std::uint64_t kAccelerationStructureAlignment = 256;  // D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT
		constexpr std::uint64_t kBlasScratchBudget = 512ull << 20;
		// One raw SRV per collision model plus a few per frame set. D3D12 caps a shader-visible heap at
		// 1,000,000 descriptors; a Vulkan descriptor heap is a buffer, so stay well inside both.
		constexpr std::uint32_t kDescriptorCapacity = 1u << 17;
		// Queries traced in one DispatchRays, before the device's own limit.
		constexpr std::uint32_t kMaxQueriesPerFrame = 1u << 22;
		constexpr std::uint32_t kRootConstantCount = sizeof(Layout::RootConstants) / sizeof(std::uint32_t);

		[[nodiscard]] std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		void Check(rhi::Result a_result, const char* a_what)
		{
			if (!rhi::IsOk(a_result)) {
				throw std::runtime_error(std::string("GPU: ") + a_what + " failed (rhi::Result " + std::to_string(static_cast<std::uint32_t>(a_result)) + ")");
			}
		}

		static_assert(sizeof(Layout::RootConstants) % sizeof(std::uint32_t) == 0, "root constants are pushed as 32-bit values");
		static_assert(Layout::kVolumeRoleWords == kVolumeRoleWords && Layout::kCliffResultWords == kCliffResultWords);
		static_assert(Rejection::kRoleOrdinary == Layout::kRoleOrdinary && Rejection::kRoleCliff == Layout::kRoleCliff &&
					  Rejection::kRolePartIgnored == Layout::kRolePartIgnored);

		// ---- GPU resources ------------------------------------------------------------------

		struct MappedBuffer
		{
			rhi::ResourcePtr resource;
			std::byte* mapped{ nullptr };
			std::uint64_t bytes{ 0 };
		};

		struct GpuModel
		{
			rhi::ResourcePtr data;
			rhi::ResourcePtr staging;
			std::uint64_t dataBytes{ 0 };
			rhi::ResourcePtr blasStorage;
			rhi::AccelerationStructurePtr blas;
			std::uint64_t blasAddress{ 0 };
			std::uint32_t srv{ 0 };
			std::uint32_t kindMask{ 0 };
			std::vector<rhi::RayTracingGeometryDesc> geometries;
			rhi::AccelerationStructurePrebuildInfo prebuild{};
		};

		// Everything one frame's queries need; reused once the frame has completed on the GPU.
		struct FrameSet
		{
			MappedBuffer candidates;
			rhi::ResourcePtr output;
			std::uint64_t outputCapacity{ 0 };
			MappedBuffer readback;
			std::uint32_t candidatesSrv{ 0 };
			std::uint32_t outputUav{ 0 };
			// Diagnostics (FASTERNGIO_DEBUG_CANDIDATE): per-invocation records for one candidate.
			rhi::ResourcePtr debug;
			MappedBuffer debugZero;
			MappedBuffer debugReadback;
			std::uint32_t debugUav{ 0 };
			struct Pending
			{
				std::shared_ptr<TraceJob> job;
				std::uint64_t offset;
				std::uint64_t count;
			};
			std::vector<Pending> pending;
			std::uint64_t queryCount{ 0 };
			TraceKind kind{ TraceKind::Volume };
			// Result words per query.
			std::uint32_t resultWords{ 1 };
			bool busy{ false };
		};

		// What the rejection pass records this frame.
		struct FrameWork
		{
			std::vector<rhi::AccelerationStructureBuildDesc> blasBuilds;
			std::vector<std::pair<rhi::ResourceHandle, std::pair<rhi::ResourceHandle, std::uint64_t>>> stagingCopies;
			std::optional<rhi::AccelerationStructureBuildDesc> tlasBuild;
			FrameSet* frame{ nullptr };
		};

		struct PassContext
		{
			rhi::DescriptorHeapHandle heap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			rhi::PipelineLayoutHandle layout{};
			rhi::PipelineHandle pipeline{};
			rhi::RayTracingShaderTableRegion rayGen{};
			rhi::RayTracingShaderTableRegion cliffRayGen{};
			rhi::RayTracingShaderTableRegion miss{};
			rhi::RayTracingShaderTableRegion hit{};
			std::uint32_t cliffHitGroupOffset{ 0 };
			std::uint32_t tlasSrv{ 0 };
			// The world's roles beyond kRoleOrdinary, and the per-instance info buffer.
			std::uint32_t roles{ 0 };
			std::uint32_t instanceInfoSrv{ 0 };
			float segmentLength{ 0.0f };
			std::uint32_t debugCandidate{ Layout::kNoDebugCandidate };
			std::shared_ptr<FrameWork> current;
		};

		void GlobalBarrier(rhi::CommandList& a_commands, rhi::ResourceSyncState a_beforeSync, rhi::ResourceAccessType a_beforeAccess,
			rhi::ResourceSyncState a_afterSync, rhi::ResourceAccessType a_afterAccess)
		{
			rhi::GlobalBarrier barrier{};
			barrier.beforeSync = a_beforeSync;
			barrier.beforeAccess = a_beforeAccess;
			barrier.afterSync = a_afterSync;
			barrier.afterAccess = a_afterAccess;
			rhi::BarrierBatch batch{};
			batch.globals = { &barrier, 1 };
			a_commands.Barriers(batch);
		}

		struct RejectionFrame
		{
			std::shared_ptr<FrameWork> work;
			PassContext context;
		};

		struct RejectionBindings
		{
		};

		// Records BLAS builds, the TLAS build and the frame's one DispatchRays. All
		// synchronisation inside the frame is explicit; the graph only orders whole frames.
		class RejectionPass final : public org::TypedRenderGraphPass<RejectionPass, RejectionFrame, RejectionBindings>
		{
		public:
			RejectionPass(std::shared_ptr<PassContext> a_context, std::shared_ptr<org::Buffer> a_heartbeat) :
				_context(std::move(a_context)), _heartbeat(std::move(a_heartbeat)) {}

			RejectionBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				a_builder.UnorderedAccess(_heartbeat);
				return {};
			}

			RejectionFrame Prepare(const RejectionBindings&, const org::PassPrepareContext&) const
			{
				return RejectionFrame{ .work = _context->current, .context = *_context };
			}

			static void Record(const RejectionBindings&, const RejectionFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.work) {
					return;
				}
				const auto& work = *a_frame.work;
				const auto& context = a_frame.context;
				auto& commands = a_recording.Commands();
				using Sync = rhi::ResourceSyncState;
				using Access = rhi::ResourceAccessType;

				// Earlier frames' copies and dispatches may still read/write what this frame touches.
				GlobalBarrier(commands, Sync::All, Access::Common, Sync::All, Access::Common);
				commands.SetDescriptorHeaps(context.heap, context.samplerHeap);

				if (!work.stagingCopies.empty()) {
					for (const auto& [destination, source] : work.stagingCopies) {
						commands.CopyBufferRegion(destination, 0, source.first, 0, source.second);
					}
					GlobalBarrier(commands, Sync::Copy, Access::CopyDest, Sync::BuildRaytracingAccelerationStructure | Sync::Raytracing,
						Access::ShaderResource);
				}
				if (!work.blasBuilds.empty()) {
					commands.BuildAccelerationStructures(work.blasBuilds.data(), static_cast<std::uint32_t>(work.blasBuilds.size()));
					GlobalBarrier(commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
						Sync::BuildRaytracingAccelerationStructure | Sync::Raytracing, Access::RaytracingAccelerationStructureRead);
				}
				if (work.tlasBuild) {
					commands.BuildAccelerationStructures(&*work.tlasBuild, 1);
					GlobalBarrier(commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
						Sync::Raytracing, Access::RaytracingAccelerationStructureRead);
				}
				if (work.frame && work.frame->queryCount > 0) {
					const auto& frame = *work.frame;
					commands.BindLayout(context.layout);
					commands.BindPipeline(context.pipeline);
					const bool debug = context.debugCandidate != Layout::kNoDebugCandidate && frame.debug;
					if (debug) {
						commands.CopyBufferRegion(frame.debug->GetHandle(), 0, frame.debugZero.resource->GetHandle(), 0, Layout::kDebugBufferBytes);
						GlobalBarrier(commands, Sync::Copy, Access::CopyDest, Sync::Raytracing, Access::UnorderedAccess);
					}
					const bool cliff = frame.kind == TraceKind::Cliff;
					const Layout::RootConstants constants{
						.tlas = context.tlasSrv,
						.candidates = frame.candidatesSrv,
						.output = frame.outputUav,
						.candidateCount = static_cast<std::uint32_t>(frame.queryCount),
						.segmentLength = context.segmentLength,
						.debugCandidate = debug && !cliff ? context.debugCandidate : Layout::kNoDebugCandidate,
						.debugBuffer = frame.debugUav,
						.pass = cliff ? Layout::kPassCliff : Layout::kPassVolume,
						.roles = context.roles,
						.instanceInfo = context.instanceInfoSrv,
						.cliffHitGroupOffset = context.cliffHitGroupOffset,
					};
					const auto words = std::bit_cast<std::array<std::uint32_t, kRootConstantCount>>(constants);
					commands.PushConstants(rhi::ShaderStage::All, 0, 0, 0, kRootConstantCount, words.data());
					rhi::RayTracingDispatchDesc dispatch{};
					dispatch.rayGenerationShaderTable = cliff ? context.cliffRayGen : context.rayGen;
					dispatch.missShaderTable = context.miss;
					dispatch.hitGroupTable = context.hit;
					dispatch.width = static_cast<std::uint32_t>(frame.queryCount);
					commands.TraceRays(dispatch);
					GlobalBarrier(commands, Sync::Raytracing, Access::UnorderedAccess, Sync::Copy, Access::CopySource);
					commands.CopyBufferRegion(frame.readback.resource->GetHandle(), 0, frame.output->GetHandle(), 0,
						frame.queryCount * frame.resultWords * sizeof(std::uint32_t));
					if (debug) {
						commands.CopyBufferRegion(frame.debugReadback.resource->GetHandle(), 0, frame.debug->GetHandle(), 0, Layout::kDebugBufferBytes);
					}
				}
			}

		private:
			std::shared_ptr<PassContext> _context;
			std::shared_ptr<org::Buffer> _heartbeat;
		};

		class RejectionExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			RejectionExtension(std::shared_ptr<PassContext> a_context, std::shared_ptr<org::Buffer> a_heartbeat) :
				_context(std::move(a_context)), _heartbeat(std::move(a_heartbeat)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("fasterngio.heartbeat"), _heartbeat);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("fasterngio.reject",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<RejectionPass>(_context, _heartbeat)))
						.PreferQueue(org::QueueKind::Graphics));
			}

		private:
			std::shared_ptr<PassContext> _context;
			std::shared_ptr<org::Buffer> _heartbeat;
		};
	}

	class GpuRejector::Impl
	{
	public:
		explicit Impl(GpuRejectorDesc a_desc) :
			_desc(std::move(a_desc))
		{
			// Startup is the only time a caller waits on the render thread.
			std::promise<void> ready;
			auto started = ready.get_future();
			_thread = std::thread([this, &ready] {
				try {
					Initialize();
					ready.set_value();
				} catch (...) {
					ready.set_exception(std::current_exception());
					return;
				}
				Loop();
				Shutdown();
			});
			try {
				started.get();
			} catch (...) {
				_thread.join();
				throw;
			}
		}

		~Impl()
		{
			_stopping.store(true, std::memory_order_release);
			Wake();
			if (_thread.joinable()) {
				_thread.join();
			}
		}

		void PostWorld(std::shared_ptr<const Rejection::WorldIndex> a_world)
		{
			// The counters describe one world. Nothing of the previous world is still running (see ReleaseWorld).
			for (auto* counter : { &_statModels, &_statInstances, &_statBlasBytes, &_statModelBytes, &_statFrames, &_statQueries }) {
				counter->store(0, std::memory_order_relaxed);
			}
			_statWorldSeconds.store(0.0, std::memory_order_relaxed);
			_inbox.Push(Command{ std::move(a_world) });
			Wake();
		}

		void Post(std::shared_ptr<TraceJob> a_job)
		{
			_inbox.Push(Command{ std::move(a_job) });
			Wake();
		}

		[[nodiscard]] GpuRejectorStats Stats() const
		{
			GpuRejectorStats stats;
			stats.adapter = _adapterName;
			stats.models = _statModels.load(std::memory_order_relaxed);
			stats.instances = _statInstances.load(std::memory_order_relaxed);
			stats.blasBytes = _statBlasBytes.load(std::memory_order_relaxed);
			stats.modelBytes = _statModelBytes.load(std::memory_order_relaxed);
			stats.framesExecuted = _statFrames.load(std::memory_order_relaxed);
			stats.queries = _statQueries.load(std::memory_order_relaxed);
			stats.worldBuildSeconds = _statWorldSeconds.load(std::memory_order_relaxed);
			return stats;
		}

	private:
		using Command = std::variant<std::shared_ptr<TraceJob>, std::shared_ptr<const Rejection::WorldIndex>>;

		void Wake()
		{
			_signal.fetch_add(1, std::memory_order_release);
			_signal.notify_one();
		}

		// ---- Render thread ------------------------------------------------------------------

		void Initialize()
		{
			CreateDeviceAndCheckFeatures();

			rhi::DescriptorHeapDesc heapDesc{ rhi::DescriptorHeapType::CbvSrvUav, kDescriptorCapacity, true, "FasterNGIO descriptors" };
			Check(_device->CreateDescriptorHeap(heapDesc, _heap), "descriptor heap");
			// Bindless root signatures index the sampler heap too, so one must be bound.
			rhi::DescriptorHeapDesc samplerDesc{ rhi::DescriptorHeapType::Sampler, 16, true, "FasterNGIO samplers" };
			Check(_device->CreateDescriptorHeap(samplerDesc, _samplerHeap), "sampler heap");

			CreatePipeline();

			_host = std::make_unique<org::PersistentGraphHost>(org::PersistentGraphHost::Desc{
				.device = _device.Get(),
				.backend = _backend,
				.tasks = std::make_shared<org::runtime::ThreadPoolTaskService>(2),
				.framesInFlight = 3,
				.epochOrder = {},
			});
			_heartbeat = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, 256, true);
			org::BufferBase::DescriptorRequirements requirements{};
			requirements.createUAV = true;
			requirements.uavDesc = { .dimension = rhi::UavDim::Buffer,
				.formatOverride = rhi::Format::R32_Typeless,
				.buffer = { .kind = rhi::BufferViewKind::Raw, .firstElement = 0, .numElements = 64 } };
			_heartbeat->SetDescriptorRequirements(requirements);
			_heartbeat->SetName("fasterngio.heartbeat");
			_passContext = std::make_shared<PassContext>();
			_passContext->heap = _heap->GetHandle();
			_passContext->samplerHeap = _samplerHeap->GetHandle();
			_passContext->layout = _layout->GetHandle();
			_passContext->pipeline = _pipeline->GetHandle();
			_passContext->rayGen = { _shaderTable.resource->GetHandle(), _rayGenOffset, _recordStride, _recordStride };
			_passContext->cliffRayGen = { _shaderTable.resource->GetHandle(), _cliffRayGenOffset, _recordStride, _recordStride };
			_passContext->miss = { _shaderTable.resource->GetHandle(), _missOffset, _recordStride * 2, _recordStride };
			_passContext->hit = { _shaderTable.resource->GetHandle(), _hitOffset, _recordStride * _hitCount * 2, _recordStride };
			_passContext->cliffHitGroupOffset = _hitCount;
			_passContext->segmentLength = _desc.segmentLength;
			if (const auto debugCandidate = DebugCandidate()) {
				_passContext->debugCandidate = *debugCandidate;
			}
			auto context = _passContext;
			auto heartbeat = _heartbeat;
			_host->AddExtension("fasterngio", [context, heartbeat] { return std::make_unique<RejectionExtension>(context, heartbeat); });
			_host->SetCompletedFrameCallback([this](std::uint64_t a_frame, const org::runtime::IStatisticsService&) { OnFrameCompleted(a_frame); });
		}

		void Shutdown()
		{
			if (_device) {
				_device->WaitIdle();
			}
			_inFlight.clear();
			// ORG resources (the heartbeat buffer) must be gone before the host's destructor shuts
			// the runtime device down.
			if (_host) {
				_host->RemoveExtension("fasterngio");
				_host->DestroyGraph();
			}
			_heartbeat.reset();
			_passContext.reset();
			_frames.clear();
			_models.clear();
			_tlas.Reset();
			_tlasStorage.Reset();
			_tlasScratch.Reset();
			_tlasInstances = {};
			_instanceInfo = {};
			_blasScratch.Reset();
			_shaderTable = {};
			_pipeline.Reset();
			_layout.Reset();
			_heap.Reset();
			_samplerHeap.Reset();
			_host.reset();
			_device.Reset();
		}

		// Everything the ray-tracing path needs, checked up front so an unsuitable adapter falls back
		// to the CPU before any work is posted rather than failing mid-run.
		void CreateDeviceAndCheckFeatures()
		{
			_backend = _desc.api == GpuApi::D3D12 ? rhi::Backend::D3D12 : rhi::Backend::Vulkan;
			rhi::DeviceCreateInfo create{};
			create.backend = _backend;
			create.framesInFlight = 3;
			create.enableDebug = _desc.debugLayer;
			rhi::Result created = rhi::Result::Unsupported;
#if defined(_WIN32)
			if (_desc.api == GpuApi::D3D12) {
				created = rhi::CreateD3D12Device(create, _device);
			}
#endif
			if (_desc.api == GpuApi::Vulkan) {
				created = rhi::CreateVulkanDevice(create, _device);
			}
			if (!rhi::IsOk(created) || !_device) {
				throw GpuUnsupportedError(std::string("no ") + GpuApiName(_desc.api) + " device could be created");
			}

			AdapterFeatureInfo adapter{};
			ShaderFeatureInfo shader{};
			RayTracingFeatureInfo rayTracing{};
			ResourceAllocationFeatureInfo allocation{};
			adapter.header.pNext = &shader.header;
			shader.header.pNext = &rayTracing.header;
			rayTracing.header.pNext = &allocation.header;
			Check(_device->QueryFeatureInfo(&adapter.header), "feature query");
			_adapterName = adapter.name;

			const bool vulkan = _desc.api == GpuApi::Vulkan;
			std::vector<std::string> missing;
			if (shader.maxShaderModel < ShaderModel::SM_6_6) {
				missing.emplace_back("shader model 6.6");
			}
			if (!shader.unifiedResourceHeaps || !shader.unboundedDescriptorTables) {
				missing.emplace_back(vulkan ? "bindless descriptor heaps (VK_EXT_descriptor_heap)" : "bindless descriptor heaps (resource binding tier 3)");
			}
			if (!rayTracing.pipeline) {
				missing.emplace_back(vulkan ? "ray-tracing pipelines (VK_KHR_ray_tracing_pipeline)" : "ray-tracing pipelines (DXR 1.0)");
			}
			if (!rayTracing.accelerationStructure) {
				missing.emplace_back(vulkan ? "acceleration structures (VK_KHR_acceleration_structure)" : "acceleration structures");
			}
			if (rayTracing.pipeline && rayTracing.maxRayRecursionDepth < 1) {
				missing.emplace_back("ray recursion depth 1");
			}
			if (rayTracing.pipeline && rayTracing.maxRayDispatchWidth < 65536) {
				missing.emplace_back("ray dispatches of 65536 launches");
			}
			if (!missing.empty()) {
				std::string list;
				for (const auto& item : missing) {
					list += (list.empty() ? "" : ", ") + item;
				}
				_device.Reset();
				throw GpuUnsupportedError(_adapterName + " (" + GpuApiName(_desc.api) + ") lacks " + list);
			}
			_rayTracing = rayTracing;
			_maxQueriesPerFrame = (std::min)(kMaxQueriesPerFrame, rayTracing.maxRayDispatchWidth);
			_gpuUploadHeap = allocation.gpuUploadHeapSupported && !std::getenv("FASTERNGIO_NO_GPU_UPLOAD_HEAP");
			constexpr const char* kTierNames[] = { "none", "DXR 1.0", "DXR 1.1", "VK_KHR_ray_tracing_pipeline", "DXR 2.0" };
			const auto tier = static_cast<std::size_t>(rayTracing.backendTier);
			spdlog::info("GPU: {} ({}), {}, GPU upload heap {}", _adapterName, GpuApiName(_desc.api), tier < std::size(kTierNames) ? kTierNames[tier] : "unknown tier",
				_gpuUploadHeap ? "yes" : "no");
		}


		void CreatePipeline()
		{
			_shaderBinary = LoadShaderLibrary(ShaderLibraryDesc{
				.api = _desc.api,
				.mode = _desc.mode,
				.shaderDirectory = _desc.shaderDirectory,
				.cacheDirectory = _desc.shaderCacheDirectory,
				.debugQueries = DebugCandidate().has_value(),
			});

			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::All;
			constants.num32BitValues = kRootConstantCount;
			Check(_device->CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PF_None }, _layout),
				"pipeline layout");

			const rhi::ShaderBinary binary{ _shaderBinary.data(), static_cast<std::uint32_t>(_shaderBinary.size()) };
			rhi::SubobjShader shaders[] = {
				{ rhi::ShaderStage::RayGen, binary, "GrassRayGen" },
				{ rhi::ShaderStage::RayGen, binary, "CliffRayGen" },
				{ rhi::ShaderStage::Miss, binary, "GrassMiss" },
				{ rhi::ShaderStage::Miss, binary, "CliffMiss" },
				{ rhi::ShaderStage::ClosestHit, binary, "GrassClosestHit" },
				{ rhi::ShaderStage::AnyHit, binary, "CliffAnyHit" },
				{ rhi::ShaderStage::Intersection, binary, "TriangleIntersection" },
				{ rhi::ShaderStage::Intersection, binary, "HullIntersection" },
				{ rhi::ShaderStage::Intersection, binary, "CapsuleIntersection" },
			};
			// Groups 0-3 are general (raygens, misses); then the volume pass's hit groups and the cliff
			// pass's, each in primitive-kind order.
			constexpr std::uint32_t kVolumeHitGroups = 4;
			constexpr std::uint32_t kCliffHitGroups = kVolumeHitGroups + kPrimitiveKindCount;
			rhi::RayTracingShaderGroupDesc groups[] = {
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "GrassRayGen", .generalShader = "GrassRayGen" },
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "CliffRayGen", .generalShader = "CliffRayGen" },
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "GrassMiss", .generalShader = "GrassMiss" },
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "CliffMiss", .generalShader = "CliffMiss" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "TriangleHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "TriangleIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "HullHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "HullIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "CapsuleHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "CapsuleIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "CliffTriangleHitGroup", .anyHitShader = "CliffAnyHit", .intersectionShader = "TriangleIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "CliffHullHitGroup", .anyHitShader = "CliffAnyHit", .intersectionShader = "HullIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "CliffCapsuleHitGroup", .anyHitShader = "CliffAnyHit", .intersectionShader = "CapsuleIntersection" },
			};
			rhi::SubobjRayTracingPipeline pipeline{};
			pipeline.globalLayout = _layout->GetHandle();
			pipeline.shaders = { shaders, static_cast<std::uint32_t>(std::size(shaders)) };
			pipeline.shaderGroups = { groups, static_cast<std::uint32_t>(std::size(groups)) };
			// The volume payload (two words) is the larger; hit attributes carry a float3 normal.
			pipeline.shaderConfig.maxPayloadSizeInBytes = 8;
			pipeline.shaderConfig.maxAttributeSizeInBytes = 12;
			pipeline.pipelineConfig.maxTraceRecursionDepth = 1;
			const rhi::PipelineStreamItem items[] = { rhi::Make(pipeline) };
			Check(_device->CreatePipeline(items, 1, _pipeline), "ray-tracing pipeline");

			// Shader table: the two raygens, the two misses, then hit records for every non-empty subset
			// of the three kinds, each subset's records in kind order (matching the BLAS geometry
			// order); the cliff pass's records repeat the layout after the volume pass's, so its rays
			// add _hitCount to reach them.
			const auto handleSize = _rayTracing.shaderGroupHandleSize;
			std::vector<std::byte> handles(static_cast<std::size_t>(handleSize) * std::size(groups));
			Check(_device->GetRayTracingShaderGroupHandles(_pipeline->GetHandle(), 0, static_cast<std::uint32_t>(std::size(groups)), handles.data(),
					  static_cast<std::uint32_t>(handles.size())),
				"shader group handles");
			_recordStride = rhi::AlignRayTracingShaderRecordSize(handleSize, _rayTracing.shaderTableStrideAlignment);
			const auto baseAlignment = _rayTracing.shaderGroupBaseAlignment;
			_rayGenOffset = 0;
			_cliffRayGenOffset = AlignUp(_rayGenOffset + _recordStride, baseAlignment);
			_missOffset = AlignUp(_cliffRayGenOffset + _recordStride, baseAlignment);
			_hitOffset = AlignUp(_missOffset + _recordStride * 2, baseAlignment);
			std::vector<std::uint32_t> hitKinds;
			for (std::uint32_t mask = 1; mask < (1u << kPrimitiveKindCount); ++mask) {
				_subsetBase[mask] = static_cast<std::uint32_t>(hitKinds.size());
				for (std::uint32_t kind = 0; kind < kPrimitiveKindCount; ++kind) {
					if (mask & (1u << kind)) {
						hitKinds.push_back(kind);
					}
				}
			}
			_hitCount = static_cast<std::uint32_t>(hitKinds.size());
			// TraceRay's hit-group offset has four bits.
			static_assert((1u << kPrimitiveKindCount) / 2 * kPrimitiveKindCount < 16);
			const auto tableBytes = _hitOffset + _recordStride * _hitCount * 2;
			_shaderTable = CreateMapped(tableBytes, rhi::HeapType::Upload, "FasterNGIO shader table");
			const auto writeRecord = [&](std::uint64_t a_offset, std::uint32_t a_group) {
				rhi::WriteRayTracingShaderRecord(_shaderTable.mapped + a_offset, _recordStride, handles.data() + a_group * handleSize, handleSize);
			};
			writeRecord(_rayGenOffset, 0);
			writeRecord(_cliffRayGenOffset, 1);
			writeRecord(_missOffset, 2);
			writeRecord(_missOffset + _recordStride, 3);
			for (std::uint32_t i = 0; i < _hitCount; ++i) {
				writeRecord(_hitOffset + i * _recordStride, kVolumeHitGroups + hitKinds[i]);
				writeRecord(_hitOffset + (_hitCount + i) * _recordStride, kCliffHitGroups + hitKinds[i]);
			}
		}

		[[nodiscard]] MappedBuffer CreateMapped(std::uint64_t a_bytes, rhi::HeapType a_heap, const char* a_name)
		{
			MappedBuffer buffer;
			buffer.bytes = (std::max<std::uint64_t>)(a_bytes, 256);
			Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(buffer.bytes, a_heap, rhi::RF_None, a_name), buffer.resource),
				a_name);
			void* mapped = nullptr;
			buffer.resource->Map(&mapped, 0, a_heap == rhi::HeapType::Readback ? buffer.bytes : 0);
			buffer.mapped = static_cast<std::byte*>(mapped);
			return buffer;
		}

		[[nodiscard]] std::uint32_t AllocateDescriptor()
		{
			if (!_freeDescriptors.empty()) {
				const auto slot = _freeDescriptors.back();
				_freeDescriptors.pop_back();
				return slot;
			}
			if (_nextDescriptor >= kDescriptorCapacity) {
				throw std::runtime_error("GPU: descriptor heap exhausted");
			}
			return _nextDescriptor++;
		}

		void WriteRawSrv(std::uint32_t a_slot, const rhi::ResourcePtr& a_resource, std::uint64_t a_bytes)
		{
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.formatOverride = rhi::Format::R32_Typeless;
			srv.buffer.kind = rhi::BufferViewKind::Raw;
			srv.buffer.numElements = static_cast<std::uint32_t>(a_bytes / 4);
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), a_slot }, a_resource->GetHandle(), srv), "raw SRV");
		}

		void WriteStructuredSrv(std::uint32_t a_slot, const rhi::ResourcePtr& a_resource, std::uint32_t a_count, std::uint32_t a_stride)
		{
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.buffer.kind = rhi::BufferViewKind::Structured;
			srv.buffer.numElements = a_count;
			srv.buffer.structureByteStride = a_stride;
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), a_slot }, a_resource->GetHandle(), srv), "structured SRV");
		}

		// A later world replaces the current one. Its jobs have all completed (the caller waits for them
		// before posting the next world, and the inbox is in order), so once the device is idle its
		// resources and descriptors can go.
		void ReleaseWorld()
		{
			_device->WaitIdle();
			for (const auto& model : _models) {
				if (model.data) {
					_freeDescriptors.push_back(model.srv);
				}
			}
			_models.clear();
			_freeDescriptors.push_back(_tlasSrv);
			_freeDescriptors.push_back(_instanceInfoSrv);
			_instanceInfo = {};
			_tlas = {};
			_tlasStorage = {};
			_tlasInstances = {};
			_tlasGeometry = {};
			_worldReady = false;
		}

		void BuildWorldOnRenderThread(const Rejection::WorldIndex& a_world)
		{
			const auto begin = std::chrono::steady_clock::now();
			if (_worldReady) {
				ReleaseWorld();
			}
			std::uint64_t modelBytes = 0;
			const auto buildOrder = CreateModels(a_world, modelBytes);
			BuildBlases(buildOrder);
			const auto instanceCount = BuildTlas(a_world.Instances());

			// Staging copies are done; the device-local copies stay.
			for (auto& model : _models) {
				model.staging.Reset();
			}
			_worldReady = true;
			_statModels.store(buildOrder.size(), std::memory_order_relaxed);
			_statInstances.store(instanceCount, std::memory_order_relaxed);
			_statModelBytes.store(modelBytes, std::memory_order_relaxed);
			_statWorldSeconds.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(), std::memory_order_relaxed);
		}

		// Uploads every placed model with collision and creates (but does not build) its BLAS. Returns
		// the models in build order; a_modelBytes sums their buffers.
		[[nodiscard]] std::vector<std::uint32_t> CreateModels(const Rejection::WorldIndex& a_world, std::uint64_t& a_modelBytes)
		{
			const auto& models = a_world.Models();
			const auto& instances = a_world.Instances();

			// Grow each model's AABBs by the widest query radius at its smallest placed scale, so the
			// AABBs contain every capsule that could touch the model at any of its references.
			std::vector<float> minScale(models.size(), (std::numeric_limits<float>::max)());
			for (const auto& instance : instances) {
				minScale[instance.model] = (std::min)(minScale[instance.model], instance.worldFromModel.scale);
			}

			_models.clear();
			_models.resize(models.size());
			std::vector<std::uint32_t> buildOrder;
			for (std::uint32_t i = 0; i < models.size(); ++i) {
				if (models[i].collision.status != Collision::ExtractionStatus::HasCollision || minScale[i] == (std::numeric_limits<float>::max)()) {
					continue;
				}
				const auto inflation = _desc.maxQueryRadius / (std::max)(minScale[i], 1.0e-3f);
				const auto packed = PackModel(models[i].collision, inflation);
				auto& model = _models[i];
				model.dataBytes = AlignUp(packed.bytes.size(), 16);
				model.kindMask = packed.kindMask;
				if (_gpuUploadHeap) {
					auto buffer = CreateMapped(model.dataBytes, rhi::HeapType::GPUUpload, "FasterNGIO model");
					std::memcpy(buffer.mapped, packed.bytes.data(), packed.bytes.size());
					model.data = std::move(buffer.resource);
				} else {
					auto staging = CreateMapped(model.dataBytes, rhi::HeapType::Upload, "FasterNGIO model staging");
					std::memcpy(staging.mapped, packed.bytes.data(), packed.bytes.size());
					model.staging = std::move(staging.resource);
					Check(_device->CreateCommittedResource(
							  rhi::helpers::ResourceDesc::Buffer(model.dataBytes, rhi::HeapType::DeviceLocal, rhi::RF_None, "FasterNGIO model"), model.data),
						"model buffer");
				}
				a_modelBytes += model.dataBytes;
				model.srv = AllocateDescriptor();
				WriteRawSrv(model.srv, model.data, model.dataBytes);

				for (const auto& geometry : packed.geometries) {
					rhi::RayTracingGeometryDesc desc{};
					desc.type = rhi::RayTracingGeometryType::Aabbs;
					desc.flags = rhi::RTGeometry_Opaque;
					desc.aabbs.aabbBuffer = { model.data->GetHandle(), geometry.aabbOffset, static_cast<std::uint64_t>(geometry.count) * sizeof(Bounds) };
					desc.aabbs.stride = sizeof(Bounds);
					desc.aabbs.count = geometry.count;
					model.geometries.push_back(desc);
				}
				rhi::AccelerationStructureBuildInputs inputs{};
				inputs.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
				inputs.flags = rhi::RTASBuild_PreferFastTrace;
				inputs.geometries = { model.geometries.data(), static_cast<std::uint32_t>(model.geometries.size()) };
				Check(_device->GetAccelerationStructurePrebuildInfo(inputs, model.prebuild), "BLAS prebuild info");
				const auto storageBytes = AlignUp(model.prebuild.resultDataMaxSizeInBytes, kAccelerationStructureAlignment);
				Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(storageBytes, rhi::HeapType::DeviceLocal,
														   static_cast<rhi::ResourceFlags>(rhi::RF_RaytracingAccelerationStructure | rhi::RF_AllowUnorderedAccess),
														   "FasterNGIO BLAS"),
						  model.blasStorage),
					"BLAS storage");
				rhi::AccelerationStructureDesc blasDesc{};
				blasDesc.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
				blasDesc.storage = model.blasStorage->GetHandle();
				blasDesc.sizeBytes = storageBytes;
				blasDesc.flags = rhi::RTASBuild_PreferFastTrace;
				Check(_device->CreateAccelerationStructure(blasDesc, model.blas), "BLAS");
				model.blasAddress = _device->GetAccelerationStructureDeviceAddress(model.blas->GetHandle());
				buildOrder.push_back(i);
				_statBlasBytes.fetch_add(storageBytes, std::memory_order_relaxed);
			}
			return buildOrder;
		}

		// Builds the BLASes in frames bounded by a scratch budget (with the staging copies they need).
		void BuildBlases(std::span<const std::uint32_t> a_buildOrder)
		{
			std::size_t next = 0;
			while (next < a_buildOrder.size()) {
				auto work = std::make_shared<FrameWork>();
				std::uint64_t scratchBytes = 0;
				const auto first = next;
				while (next < a_buildOrder.size()) {
					const auto bytes = AlignUp(_models[a_buildOrder[next]].prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment);
					if (next > first && scratchBytes + bytes > kBlasScratchBudget) {
						break;
					}
					scratchBytes += bytes;
					++next;
				}
				EnsureScratch(_blasScratch, _blasScratchBytes, scratchBytes, "FasterNGIO BLAS scratch");
				std::uint64_t scratchOffset = 0;
				for (auto i = first; i < next; ++i) {
					auto& model = _models[a_buildOrder[i]];
					if (model.staging) {
						work->stagingCopies.push_back({ model.data->GetHandle(), { model.staging->GetHandle(), model.dataBytes } });
					}
					rhi::AccelerationStructureBuildDesc build{};
					build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
					build.destination = model.blas->GetHandle();
					build.inputs.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
					build.inputs.flags = rhi::RTASBuild_PreferFastTrace;
					build.inputs.geometries = { model.geometries.data(), static_cast<std::uint32_t>(model.geometries.size()) };
					const auto bytes = AlignUp(model.prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment);
					build.scratch = { _blasScratch->GetHandle(), scratchOffset, bytes };
					scratchOffset += bytes;
					work->blasBuilds.push_back(build);
				}
				ExecuteFrame(std::move(work), nullptr);
				// The scratch buffer is reused by the next chunk.
				_device->WaitIdle();
			}
		}

		// Builds the world TLAS, one instance per placed reference whose model has a BLAS, and its
		// SRV. Returns the instance count.
		[[nodiscard]] std::size_t BuildTlas(std::span<const Rejection::Instance> a_instances)
		{
			// Each instance's mask is its role, so the volume pass can trace roles apart; per TLAS
			// instance, the shaders also read its WorldIndex index, role and steepness.
			std::vector<rhi::PackedRayTracingInstanceDesc> packed;
			std::vector<std::uint32_t> info;
			packed.reserve(a_instances.size());
			info.reserve(a_instances.size() * 2);
			std::uint32_t roles = 0;
			for (std::uint32_t index = 0; index < a_instances.size(); ++index) {
				const auto& instance = a_instances[index];
				const auto& model = _models[instance.model];
				if (!model.blas) {
					continue;
				}
				rhi::RayTracingInstanceDesc desc{};
				std::memcpy(desc.transform, instance.worldFromModel.m, sizeof(desc.transform));
				desc.instanceID = model.srv;
				desc.instanceMask = static_cast<std::uint8_t>(instance.role);
				roles |= instance.role == Rejection::kRoleOrdinary ? 0u : static_cast<std::uint32_t>(instance.role);
				info.push_back(index);
				info.push_back(static_cast<std::uint32_t>(instance.role) | (instance.steep ? Layout::kInstanceSteep : 0u));
				desc.instanceContributionToHitGroupIndex = _subsetBase[model.kindMask];
				desc.flags = rhi::RTInstance_ForceOpaque;
				packed.push_back(rhi::PackRayTracingInstanceDesc(desc, model.blasAddress));
			}
			_tlasInstances = CreateMapped(packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc), rhi::HeapType::Upload, "FasterNGIO TLAS instances");
			std::memcpy(_tlasInstances.mapped, packed.data(), packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc));
			_tlasGeometry = {};
			_tlasGeometry.type = rhi::RayTracingGeometryType::Instances;
			_tlasGeometry.instances.instanceBuffer = { _tlasInstances.resource->GetHandle(), 0, packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc) };
			_tlasGeometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
			_tlasGeometry.instances.count = static_cast<std::uint32_t>(packed.size());
			rhi::AccelerationStructureBuildInputs inputs{};
			inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
			inputs.flags = rhi::RTASBuild_PreferFastTrace;
			inputs.geometries = { &_tlasGeometry, 1 };
			rhi::AccelerationStructurePrebuildInfo prebuild{};
			Check(_device->GetAccelerationStructurePrebuildInfo(inputs, prebuild), "TLAS prebuild info");
			const auto tlasBytes = AlignUp(prebuild.resultDataMaxSizeInBytes, kAccelerationStructureAlignment);
			Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(tlasBytes, rhi::HeapType::DeviceLocal,
													   static_cast<rhi::ResourceFlags>(rhi::RF_RaytracingAccelerationStructure | rhi::RF_AllowUnorderedAccess),
													   "FasterNGIO TLAS"),
					  _tlasStorage),
				"TLAS storage");
			rhi::AccelerationStructureDesc tlasDesc{};
			tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
			tlasDesc.storage = _tlasStorage->GetHandle();
			tlasDesc.sizeBytes = tlasBytes;
			tlasDesc.flags = rhi::RTASBuild_PreferFastTrace;
			Check(_device->CreateAccelerationStructure(tlasDesc, _tlas), "TLAS");
			EnsureScratch(_tlasScratch, _tlasScratchBytes, AlignUp(prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment), "FasterNGIO TLAS scratch");
			auto work = std::make_shared<FrameWork>();
			rhi::AccelerationStructureBuildDesc build{};
			build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
			build.destination = _tlas->GetHandle();
			build.inputs = inputs;
			build.scratch = { _tlasScratch->GetHandle(), 0, _tlasScratchBytes };
			work->tlasBuild = build;
			ExecuteFrame(std::move(work), nullptr);
			_device->WaitIdle();

			_tlasSrv = AllocateDescriptor();
			_passContext->tlasSrv = _tlasSrv;
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::AccelerationStruct;
			srv.accel.accelerationStructure = _tlas->GetHandle();
			srv.accel.sizeBytes = tlasBytes;
			// The view addresses the TLAS itself; BasicRHI treats an empty resource as a null view.
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), _tlasSrv }, _tlasStorage->GetHandle(), srv), "TLAS SRV");

			if (info.empty()) {
				info.assign(2, 0u);
			}
			_instanceInfo = CreateMapped(info.size() * sizeof(std::uint32_t), rhi::HeapType::Upload, "FasterNGIO instance info");
			std::memcpy(_instanceInfo.mapped, info.data(), info.size() * sizeof(std::uint32_t));
			_instanceInfoSrv = AllocateDescriptor();
			WriteStructuredSrv(_instanceInfoSrv, _instanceInfo.resource, static_cast<std::uint32_t>(info.size() / 2), 2 * sizeof(std::uint32_t));
			_passContext->instanceInfoSrv = _instanceInfoSrv;
			_passContext->roles = roles;
			return packed.size();
		}

		void EnsureScratch(rhi::ResourcePtr& a_buffer, std::uint64_t& a_capacity, std::uint64_t a_bytes, const char* a_name)
		{
			if (a_buffer && a_capacity >= a_bytes) {
				return;
			}
			a_buffer.Reset();
			a_capacity = AlignUp((std::max<std::uint64_t>)(a_bytes, 1ull << 20), kAccelerationStructureAlignment);
			Check(_device->CreateCommittedResource(
					  rhi::helpers::ResourceDesc::Buffer(a_capacity, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, a_name), a_buffer),
				a_name);
		}

		FrameSet& AcquireFrameSet()
		{
			for (auto& frame : _frames) {
				if (!frame->busy) {
					return *frame;
				}
			}
			auto frame = std::make_unique<FrameSet>();
			frame->candidatesSrv = AllocateDescriptor();
			frame->outputUav = AllocateDescriptor();
			if (_passContext->debugCandidate != Layout::kNoDebugCandidate) {
				Check(_device->CreateCommittedResource(
						  rhi::helpers::ResourceDesc::Buffer(Layout::kDebugBufferBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "FasterNGIO debug"), frame->debug),
					"debug buffer");
				frame->debugZero = CreateMapped(Layout::kDebugBufferBytes, rhi::HeapType::Upload, "FasterNGIO debug zero");
				std::memset(frame->debugZero.mapped, 0, Layout::kDebugBufferBytes);
				frame->debugReadback = CreateMapped(Layout::kDebugBufferBytes, rhi::HeapType::Readback, "FasterNGIO debug readback");
				frame->debugUav = AllocateDescriptor();
				rhi::UavDesc uav{};
				uav.dimension = rhi::UavDim::Buffer;
				uav.formatOverride = rhi::Format::R32_Typeless;
				uav.buffer.kind = rhi::BufferViewKind::Raw;
				uav.buffer.numElements = static_cast<std::uint32_t>(Layout::kDebugBufferBytes / 4);
				Check(_device->CreateUnorderedAccessView({ _heap->GetHandle(), frame->debugUav }, frame->debug->GetHandle(), uav), "debug UAV");
			}
			_frames.push_back(std::move(frame));
			return *_frames.back();
		}

		// Sizes a frame set for its queries (all of one kind) and concatenates the jobs' candidates.
		void FillFrameSet(FrameSet& a_frame, std::vector<std::shared_ptr<TraceJob>>& a_jobs)
		{
			std::uint64_t queries = 0;
			for (const auto& job : a_jobs) {
				queries += job->Queries().size();
			}
			a_frame.kind = a_jobs.front()->Kind();
			a_frame.resultWords = a_frame.kind == TraceKind::Cliff ? kCliffResultWords : (_passContext->roles != 0 ? kVolumeRoleWords : 1u);
			const auto words = queries * a_frame.resultWords;

			if (a_frame.candidates.bytes < queries * sizeof(Query)) {
				a_frame.candidates = CreateMapped(queries * sizeof(Query) * 3 / 2, rhi::HeapType::Upload, "FasterNGIO candidates");
				WriteStructuredSrv(a_frame.candidatesSrv, a_frame.candidates.resource, static_cast<std::uint32_t>(a_frame.candidates.bytes / sizeof(Query)), sizeof(Query));
			}
			if (a_frame.outputCapacity < words) {
				a_frame.outputCapacity = words * 3 / 2;
				const auto bytes = a_frame.outputCapacity * sizeof(std::uint32_t);
				a_frame.output.Reset();
				Check(_device->CreateCommittedResource(
						  rhi::helpers::ResourceDesc::Buffer(bytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "FasterNGIO output"),
						  a_frame.output),
					"output buffer");
				rhi::UavDesc uav{};
				uav.dimension = rhi::UavDim::Buffer;
				uav.buffer.kind = rhi::BufferViewKind::Structured;
				uav.buffer.numElements = static_cast<std::uint32_t>(a_frame.outputCapacity);
				uav.buffer.structureByteStride = sizeof(std::uint32_t);
				Check(_device->CreateUnorderedAccessView({ _heap->GetHandle(), a_frame.outputUav }, a_frame.output->GetHandle(), uav), "output UAV");
				a_frame.readback = CreateMapped(bytes, rhi::HeapType::Readback, "FasterNGIO readback");
			}

			auto* candidates = reinterpret_cast<Query*>(a_frame.candidates.mapped);
			std::uint64_t offset = 0;
			a_frame.pending.clear();
			for (auto& job : a_jobs) {
				const auto count = job->Queries().size();
				std::memcpy(candidates + offset, job->Queries().data(), count * sizeof(Query));
				job->ReleaseQueries();
				a_frame.pending.push_back({ std::move(job), offset, count });
				offset += count;
			}
			a_frame.queryCount = queries;
		}

		void ExecuteFrame(std::shared_ptr<FrameWork> a_work, FrameSet* a_frame)
		{
			a_work->frame = a_frame;
			_passContext->current = std::move(a_work);
			_host->ExecuteFrame();
			_passContext->current.reset();
			if (a_frame) {
				a_frame->busy = true;
				_inFlight.emplace(_host->LastHostFrame(), a_frame);
			}
			_statFrames.fetch_add(1, std::memory_order_relaxed);
		}

		void OnFrameCompleted(std::uint64_t a_frameNumber)
		{
			const auto it = _inFlight.find(a_frameNumber);
			if (it == _inFlight.end()) {
				return;
			}
			CompleteFrame(*it->second);
			_inFlight.erase(it);
		}

		void CompleteFrame(FrameSet& a_frame)
		{
			if (a_frame.debug) {
				const auto* words = reinterpret_cast<const std::uint32_t*>(a_frame.debugReadback.mapped);
				const auto count = (std::min)(words[0], Layout::kDebugMaxRecords);
				for (std::uint32_t i = 0; i < count; ++i) {
					const auto* record = words + (Layout::kDebugHeaderBytes + i * Layout::kDebugRecordBytes) / 4;
					const auto* f = reinterpret_cast<const float*>(record);
					spdlog::info("debug stage={} instance={} primitive={} result={} p=({:.3f},{:.3f},{:.3f},{:.3f}) q=({:.3f},{:.3f},{:.3f},{:.3f}) a=({:.2f},{:.2f},{:.2f}) b=({:.3f},{:.3f},{:.3f},{:.3f}) c=({:.2f},{:.2f},{:.2f})",
						record[0], record[1], record[2], record[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[16], f[17], f[18], f[19], f[20], f[21], f[22]);
				}
			}
			const auto* hits = reinterpret_cast<const std::uint32_t*>(a_frame.readback.mapped);
			for (auto& pending : a_frame.pending) {
				pending.job->Finish(std::span(hits + pending.offset * a_frame.resultWords, pending.count * a_frame.resultWords), a_frame.resultWords, false);
			}
			_statQueries.fetch_add(a_frame.queryCount, std::memory_order_relaxed);
			a_frame.pending.clear();
			a_frame.busy = false;
		}

		void CompleteAllInFlight()
		{
			if (_inFlight.empty()) {
				return;
			}
			_device->WaitIdle();
			for (auto& [frameNumber, frame] : _inFlight) {
				CompleteFrame(*frame);
			}
			_inFlight.clear();
		}

		void Loop()
		{
			std::deque<std::shared_ptr<TraceJob>> waiting;
			try {
				while (true) {
					const auto seen = _signal.load(std::memory_order_acquire);
					while (auto command = _inbox.TryPop()) {
						if (auto* job = std::get_if<std::shared_ptr<TraceJob>>(&*command)) {
							waiting.push_back(std::move(*job));
						} else {
							BuildWorldOnRenderThread(*std::get<std::shared_ptr<const Rejection::WorldIndex>>(*command));
						}
					}
					if (_worldReady && !waiting.empty()) {
						// One kind per dispatch: the oldest job's, gathered from the whole queue (cells post
						// volume and cliff jobs interleaved); the others keep their order.
						std::vector<std::shared_ptr<TraceJob>> jobs;
						std::uint64_t queries = 0;
						const auto kind = waiting.front()->Kind();
						for (auto it = waiting.begin(); it != waiting.end();) {
							const auto size = (*it)->Queries().size();
							if ((*it)->Kind() != kind || (!jobs.empty() && queries + size > _maxQueriesPerFrame)) {
								++it;
								continue;
							}
							queries += size;
							jobs.push_back(std::move(*it));
							it = waiting.erase(it);
						}
						auto& frame = AcquireFrameSet();
						FillFrameSet(frame, jobs);
						ExecuteFrame(std::make_shared<FrameWork>(), &frame);
						continue;
					}
					if (!_inFlight.empty()) {
						// Nothing new to trace: finish what is in flight instead of ticking empty frames.
						CompleteAllInFlight();
						continue;
					}
					if (_stopping.load(std::memory_order_acquire) && waiting.empty()) {
						return;
					}
					_signal.wait(seen, std::memory_order_acquire);
				}
			} catch (const std::exception& e) {
				spdlog::error("GPU: render thread failed: {}", e.what());
			} catch (...) {
				spdlog::error("GPU: render thread failed");
			}
			// Fail every job still owed a result so no consumer waits forever.
			for (auto& [frameNumber, frame] : _inFlight) {
				for (auto& pending : frame->pending) {
					pending.job->Finish({}, 1, true);
				}
			}
			_inFlight.clear();
			for (auto& job : waiting) {
				job->Finish({}, 1, true);
			}
			while (!_stopping.load(std::memory_order_acquire)) {
				while (auto command = _inbox.TryPop()) {
					if (auto* job = std::get_if<std::shared_ptr<TraceJob>>(&*command)) {
						(*job)->Finish({}, 1, true);
					}
				}
				const auto seen = _signal.load(std::memory_order_acquire);
				if (_stopping.load(std::memory_order_acquire)) {
					break;
				}
				_signal.wait(seen, std::memory_order_acquire);
			}
		}

		GpuRejectorDesc _desc;
		std::thread _thread;
		Concurrency::MpscQueue<Command> _inbox;
		std::atomic<std::uint64_t> _signal{ 0 };
		std::atomic<bool> _stopping{ false };
		std::atomic<std::uint64_t> _statModels{ 0 };
		std::atomic<std::uint64_t> _statInstances{ 0 };
		std::atomic<std::uint64_t> _statBlasBytes{ 0 };
		std::atomic<std::uint64_t> _statModelBytes{ 0 };
		std::atomic<std::uint64_t> _statFrames{ 0 };
		std::atomic<std::uint64_t> _statQueries{ 0 };
		std::atomic<double> _statWorldSeconds{ 0.0 };

		// Render thread only.
		rhi::Backend _backend{ rhi::Backend::D3D12 };
		rhi::DevicePtr _device;
		// Written once by the render thread before the constructor returns.
		std::string _adapterName;
		RayTracingFeatureInfo _rayTracing{};
		std::uint32_t _maxQueriesPerFrame{ 0 };
		bool _gpuUploadHeap{ false };
		rhi::DescriptorHeapPtr _heap;
		rhi::DescriptorHeapPtr _samplerHeap;
		std::uint32_t _nextDescriptor{ 0 };
		std::vector<std::uint32_t> _freeDescriptors;
		std::vector<std::byte> _shaderBinary;
		rhi::PipelineLayoutPtr _layout;
		rhi::PipelinePtr _pipeline;
		MappedBuffer _shaderTable;
		std::uint64_t _recordStride{ 0 };
		std::uint64_t _rayGenOffset{ 0 };
		std::uint64_t _cliffRayGenOffset{ 0 };
		std::uint64_t _missOffset{ 0 };
		std::uint64_t _hitOffset{ 0 };
		std::uint32_t _hitCount{ 0 };
		std::array<std::uint32_t, 1u << kPrimitiveKindCount> _subsetBase{};
		std::unique_ptr<org::PersistentGraphHost> _host;
		std::shared_ptr<org::Buffer> _heartbeat;
		std::shared_ptr<PassContext> _passContext;
		std::vector<GpuModel> _models;
		rhi::ResourcePtr _blasScratch;
		std::uint64_t _blasScratchBytes{ 0 };
		rhi::RayTracingGeometryDesc _tlasGeometry{};
		MappedBuffer _tlasInstances;
		rhi::ResourcePtr _tlasStorage;
		rhi::AccelerationStructurePtr _tlas;
		rhi::ResourcePtr _tlasScratch;
		std::uint64_t _tlasScratchBytes{ 0 };
		std::uint32_t _tlasSrv{ 0 };
		MappedBuffer _instanceInfo;
		std::uint32_t _instanceInfoSrv{ 0 };
		bool _worldReady{ false };
		std::vector<std::unique_ptr<FrameSet>> _frames;
		std::map<std::uint64_t, FrameSet*> _inFlight;
	};

	std::vector<Query> MakeQueries(std::span<const Grass::BladeCandidate> a_blades, std::span<const Rejection::QueryShape> a_shapes)
	{
		std::vector<Query> queries;
		queries.reserve(a_blades.size());
		for (const auto& blade : a_blades) {
			const auto& shape = a_shapes[blade.groupIndex];
			queries.push_back(Query{ blade.position[0], blade.position[1], blade.position[2] - shape.depth, (std::max)(shape.radius, 1.0e-3f) });
		}
		return queries;
	}

	Rejection::VolumeHits ReadVolumeHits(std::span<const std::uint32_t> a_words)
	{
		Rejection::VolumeHits hits;
		hits.ordinary = (a_words[0] & Layout::kRoleOrdinary) != 0;
		hits.cliff = (a_words[0] & Layout::kRoleCliff) != 0;
		hits.partIgnored = (a_words[0] & Layout::kRolePartIgnored) != 0 ? a_words[1] : Rejection::kNoInstance;
		return hits;
	}

	Rejection::CliffRays ReadCliffRays(std::span<const std::uint32_t> a_words)
	{
		Rejection::CliffRays rays;
		rays.upClosest = a_words[Layout::kCliffWordUpClosest];
		rays.cliffT = std::bit_cast<float>(a_words[Layout::kCliffWordCliffT]);
		// The shader reports the normal facing down the ray; NGIO negates it to face up the cliff.
		rays.cliffNormal = {
			-std::bit_cast<float>(a_words[Layout::kCliffWordNormal]),
			-std::bit_cast<float>(a_words[Layout::kCliffWordNormal + 1]),
			-std::bit_cast<float>(a_words[Layout::kCliffWordNormal + 2]),
		};
		if (rays.upClosest == Rejection::kNoInstance) {
			rays.cliffT = -1.0f;
		}
		for (std::size_t i = 0; i < rays.neighbours.size(); ++i) {
			rays.neighbours[i] = Rejection::CliffRay{
				.closest = a_words[Layout::kCliffWordNeighbours + i * 2],
				.highestT = std::bit_cast<float>(a_words[Layout::kCliffWordNeighbours + i * 2 + 1]),
			};
		}
		return rays;
	}

	TraceJob::TraceJob(std::vector<Query> a_queries, TraceKind a_kind) :
		_queries(std::move(a_queries)), _kind(a_kind)
	{}

	TraceJob::~TraceJob()
	{
		auto* node = _subscribers.load(std::memory_order_acquire);
		if (node == ClosedList()) {
			return;
		}
		while (node) {
			delete std::exchange(node, node->next);
		}
	}

	bool TraceJob::Subscribe(std::function<void()> a_callback)
	{
		auto* node = new Subscriber{ std::move(a_callback) };
		auto* head = _subscribers.load(std::memory_order_acquire);
		do {
			if (head == ClosedList()) {
				delete node;
				return false;
			}
			node->next = head;
		} while (!_subscribers.compare_exchange_weak(head, node, std::memory_order_acq_rel, std::memory_order_acquire));
		return true;
	}

	void TraceJob::Finish(std::span<const std::uint32_t> a_hits, std::uint32_t a_resultWords, bool a_failed) noexcept
	{
		_hits.assign(a_hits.begin(), a_hits.end());
		_resultWords = a_resultWords;
		_failed.store(a_failed, std::memory_order_release);
		_complete.store(true, std::memory_order_release);
		auto* node = _subscribers.exchange(ClosedList(), std::memory_order_acq_rel);
		while (node) {
			if (node->callback) {
				node->callback();
			}
			delete std::exchange(node, node->next);
		}
	}

	void TraceJob::ReleaseQueries() noexcept
	{
		std::vector<Query>().swap(_queries);
	}

	GpuRejector::GpuRejector(GpuRejectorDesc a_desc) :
		_impl(std::make_unique<Impl>(std::move(a_desc)))
	{}

	GpuRejector::~GpuRejector() = default;

	void GpuRejector::PostWorld(std::shared_ptr<const Rejection::WorldIndex> a_world)
	{
		_impl->PostWorld(std::move(a_world));
	}

	void GpuRejector::Post(std::shared_ptr<TraceJob> a_job)
	{
		_impl->Post(std::move(a_job));
	}

	GpuRejectorStats GpuRejector::Stats() const
	{
		return _impl->Stats();
	}
}
