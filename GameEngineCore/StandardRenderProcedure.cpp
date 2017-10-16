#include "Engine.h"
#include "WorldRenderPass.h"
#include "PostRenderPass.h"
#include "Renderer.h"
#include "RenderPassRegistry.h"
#include "DirectionalLightActor.h"
#include "AtmosphereActor.h"
#include "ToneMappingActor.h"
#include "FrustumCulling.h"
#include "RenderProcedure.h"
#include "StandardViewUniforms.h"
#include "LightingData.h"

namespace GameEngine
{
	class LightUniforms
	{
	public:
		VectorMath::Vec3 lightDir; float pad0 = 0.0f;
		VectorMath::Vec3 lightColor; 
		float ambient = 0.2f;
		int shadowMapId = -1;
		int numCascades = 0;
		int padding2 = 0, pad3 = 0;
		VectorMath::Matrix4 lightMatrix[MaxShadowCascades];
		float zPlanes[MaxShadowCascades];
		LightUniforms()
		{
			lightDir.SetZero();
			lightColor.SetZero();
			for (int i = 0; i < MaxShadowCascades; i++)
				zPlanes[i] = 0.0f;
		}
	};

	class StandardRenderProcedure : public IRenderProcedure
	{
	private:
		RendererSharedResource * sharedRes = nullptr;
		ViewResource * viewRes = nullptr;

		RefPtr<WorldRenderPass> shadowRenderPass;
		RefPtr<WorldRenderPass> forwardRenderPass;
		RefPtr<PostRenderPass> atmospherePass;
		RefPtr<PostRenderPass> toneMappingFromAtmospherePass;
		RefPtr<PostRenderPass> toneMappingFromLitColorPass;

		RenderOutput * forwardBaseOutput = nullptr;
		RenderOutput * transparentAtmosphereOutput = nullptr;

		StandardViewUniforms viewUniform;

		RefPtr<WorldPassRenderTask> forwardBaseInstance, transparentPassInstance;

		DeviceMemory renderPassUniformMemory;
		SharedModuleInstances sharedModules;
		ModuleInstance forwardBasePassParams;
		CoreLib::List<ModuleInstance> shadowViewInstances;

		DrawableSink sink;

		List<Drawable*> reorderBuffer, drawableBuffer;
		LightingEnvironment lighting;
		AtmosphereParameters lastAtmosphereParams;
		ToneMappingParameters lastToneMappingParams;

		bool useAtmosphere = false;
		bool toneMapping = false;
		bool useEnvMap = false;
	public:
		StandardRenderProcedure(bool pToneMapping, bool pUseEnvMap)
		{
			toneMapping = pToneMapping;
			useEnvMap = pUseEnvMap;
		}
		~StandardRenderProcedure()
		{
			if (forwardBaseOutput)
				viewRes->DestroyRenderOutput(forwardBaseOutput);
			if (transparentAtmosphereOutput)
				viewRes->DestroyRenderOutput(transparentAtmosphereOutput);

		}
		virtual RenderTarget* GetOutput() override
		{
			if (toneMapping)
			{
				return viewRes->LoadSharedRenderTarget("toneColor", StorageFormat::RGBA_8).Ptr();
			}
			else
			{
				if (useAtmosphere)
					return viewRes->LoadSharedRenderTarget("litAtmosphereColor", StorageFormat::RGBA_F16).Ptr();
				else
					return viewRes->LoadSharedRenderTarget("litColor", StorageFormat::RGBA_F16).Ptr();
			}
		}
		virtual void UpdateSharedResourceBinding() override
		{
			for (int i = 0; i < DynamicBufferLengthMultiplier; i++)
			{
				auto descSet = forwardBasePassParams.GetDescriptorSet(i);
				descSet->BeginUpdate();
				descSet->Update(1, sharedRes->textureSampler.Ptr());
				descSet->EndUpdate();
			}
			lighting.UpdateSharedResourceBinding();
		}
		virtual void Init(Renderer * renderer, ViewResource * pViewRes) override
		{
			viewRes = pViewRes;
			sharedRes = renderer->GetSharedResource();
			shadowRenderPass = CreateShadowRenderPass();
			shadowRenderPass->Init(renderer);
			
			forwardRenderPass = CreateForwardBaseRenderPass();
			forwardRenderPass->Init(renderer);

			forwardBaseOutput = viewRes->CreateRenderOutput(
				forwardRenderPass->GetRenderTargetLayout(),
				viewRes->LoadSharedRenderTarget("litColor", StorageFormat::RGBA_F16),
				viewRes->LoadSharedRenderTarget("depthBuffer", DepthBufferFormat)
			);
			transparentAtmosphereOutput = viewRes->CreateRenderOutput(
				forwardRenderPass->GetRenderTargetLayout(),
				viewRes->LoadSharedRenderTarget("litAtmosphereColor", StorageFormat::RGBA_F16),
				viewRes->LoadSharedRenderTarget("depthBuffer", DepthBufferFormat)
			);
			
			atmospherePass = CreateAtmospherePostRenderPass(viewRes);
			atmospherePass->SetSource(MakeArray(
				PostPassSource("litColor", StorageFormat::RGBA_F16),
				PostPassSource("depthBuffer", DepthBufferFormat),
				PostPassSource("litAtmosphereColor", StorageFormat::RGBA_F16)
			).GetArrayView());
			atmospherePass->Init(renderer);

			toneMappingFromAtmospherePass = CreateToneMappingPostRenderPass(viewRes);
			toneMappingFromAtmospherePass->SetSource(MakeArray(
				PostPassSource("litAtmosphereColor", StorageFormat::RGBA_F16),
				PostPassSource("toneColor", StorageFormat::RGBA_8)
			).GetArrayView());
			toneMappingFromAtmospherePass->Init(renderer);


			if (toneMapping)
			{
				toneMappingFromLitColorPass = CreateToneMappingPostRenderPass(viewRes);
				toneMappingFromLitColorPass->SetSource(MakeArray(
					PostPassSource("litColor", StorageFormat::RGBA_F16),
					PostPassSource("toneColor", StorageFormat::RGBA_8)
				).GetArrayView());
				toneMappingFromLitColorPass->Init(renderer);
			}
			// initialize forwardBasePassModule and lightingModule
			renderPassUniformMemory.Init(sharedRes->hardwareRenderer.Ptr(), BufferUsage::UniformBuffer, true, 22, sharedRes->hardwareRenderer->UniformBufferAlignment());
			sharedRes->CreateModuleInstance(forwardBasePassParams, spEnvFindModule(sharedRes->sharedSpireEnvironment, "ForwardBasePassParams"), &renderPassUniformMemory);
			lighting.Init(*sharedRes, &renderPassUniformMemory, useEnvMap);
			UpdateSharedResourceBinding();
			sharedModules.View = &forwardBasePassParams;
			shadowViewInstances.Reserve(1024);
		}

		ArrayView<Drawable*> GetDrawable(DrawableSink * objSink, bool transparent, bool shadowCasterOnly, CullFrustum cf, bool append)
		{
			if (!append)
				drawableBuffer.Clear();
			for (auto obj : objSink->GetDrawables(transparent))
			{
				if (shadowCasterOnly && !obj->CastShadow)
					continue;
				if (cf.IsBoxInFrustum(obj->Bounds))
					drawableBuffer.Add(obj);
			}
			return drawableBuffer.GetArrayView();
		}

		List<Texture*> textures;
		
		virtual void Run(FrameRenderTask & task, const RenderProcedureParameters & params) override
		{
			int w = 0, h = 0;

			forwardRenderPass->ResetInstancePool();
			forwardBaseOutput->GetSize(w, h);
			forwardBaseInstance = forwardRenderPass->CreateInstance(forwardBaseOutput, true);
			float aspect = w / (float)h;
			shadowRenderPass->ResetInstancePool();
			
			GetDrawablesParameter getDrawableParam;
			viewUniform.CameraPos = params.view.Position;
			viewUniform.ViewTransform = params.view.Transform;
			getDrawableParam.CameraDir = params.view.GetDirection();
			getDrawableParam.IsEditorMode = params.isEditorMode;
			Matrix4 mainProjMatrix;
			Matrix4::CreatePerspectiveMatrixFromViewAngle(mainProjMatrix,
				params.view.FOV, w / (float)h,
				params.view.ZNear, params.view.ZFar, ClipSpaceType::ZeroToOne);
			Matrix4::Multiply(viewUniform.ViewProjectionTransform, mainProjMatrix, viewUniform.ViewTransform);
			
			viewUniform.ViewTransform.Inverse(viewUniform.InvViewTransform);
			viewUniform.ViewProjectionTransform.Inverse(viewUniform.InvViewProjTransform);
			viewUniform.Time = Engine::Instance()->GetTime();

			getDrawableParam.CameraPos = viewUniform.CameraPos;
			getDrawableParam.rendererService = params.rendererService;
			getDrawableParam.sink = &sink;
			
			useAtmosphere = false;
			sink.Clear();
						
			CoreLib::Graphics::BBox levelBounds;
			// initialize bounds to a small extent to prevent error
			levelBounds.Min = Vec3::Create(-10.0f);
			levelBounds.Max = Vec3::Create(10.0f);
			for (auto & actor : params.level->Actors)
			{
				levelBounds.Union(actor.Value->Bounds);
				actor.Value->GetDrawables(getDrawableParam);
				auto actorType = actor.Value->GetEngineType();
				if (actorType == EngineActorType::Atmosphere)
				{
					useAtmosphere = true;
					auto atmosphere = dynamic_cast<AtmosphereActor*>(actor.Value.Ptr());
					auto newParams = atmosphere->GetParameters();
					if (!(lastAtmosphereParams == newParams))
					{
						atmosphere->SunDir = atmosphere->SunDir.GetValue().Normalize();
						newParams = atmosphere->GetParameters();
						atmospherePass->SetParameters(&newParams, sizeof(newParams));
						lastAtmosphereParams = newParams;
					}
				}
				else if (toneMapping && actorType == EngineActorType::ToneMapping)
				{
					auto toneMappingActor = dynamic_cast<ToneMappingActor*>(actor.Value.Ptr());
					if (!(lastToneMappingParams == toneMappingActor->Parameters))
					{
						toneMappingFromAtmospherePass->SetParameters(&toneMappingActor->Parameters, sizeof(toneMappingActor->Parameters));
						toneMappingFromLitColorPass->SetParameters(&toneMappingActor->Parameters, sizeof(toneMappingActor->Parameters));
						lastToneMappingParams = toneMappingActor->Parameters;
					}
				}
			}
			
			lighting.GatherInfo(task, &sink, params, w, h, viewUniform, shadowRenderPass.Ptr());

			forwardBasePassParams.SetUniformData(&viewUniform, (int)sizeof(viewUniform));
			auto cameraCullFrustum = CullFrustum(params.view.GetFrustum(aspect));
			
			forwardBaseOutput->GetFrameBuffer()->GetRenderAttachments().GetTextures(textures);
			task.AddImageTransferTask(textures.GetArrayView(), CoreLib::ArrayView<Texture*>());

			forwardRenderPass->Bind();
			sharedRes->pipelineManager.PushModuleInstance(&forwardBasePassParams);
			sharedRes->pipelineManager.PushModuleInstance(&lighting.moduleInstance);
			forwardBaseInstance->SetDrawContent(sharedRes->pipelineManager, reorderBuffer, GetDrawable(&sink, false, false, cameraCullFrustum, false));
			sharedRes->pipelineManager.PopModuleInstance();
			sharedRes->pipelineManager.PopModuleInstance();
			task.AddTask(forwardBaseInstance);

			task.AddImageTransferTask(CoreLib::ArrayView<Texture*>(), textures.GetArrayView());

			if (useAtmosphere)
			{
				task.AddTask(atmospherePass->CreateInstance(sharedModules));
			}
			task.sharedModuleInstances = sharedModules;


			// transparency pass
			reorderBuffer.Clear();
			for (auto drawable : GetDrawable(&sink, true, false, cameraCullFrustum, false))
			{
				reorderBuffer.Add(drawable);
			}
			if (reorderBuffer.Count())
			{
				reorderBuffer.Sort([=](Drawable* d1, Drawable* d2) { return d1->Bounds.Distance(params.view.Position) > d2->Bounds.Distance(params.view.Position); });
				if (useAtmosphere)
				{
					transparentPassInstance = forwardRenderPass->CreateInstance(transparentAtmosphereOutput, false);
					transparentAtmosphereOutput->GetFrameBuffer()->GetRenderAttachments().GetTextures(textures);
				}
				else
				{
					transparentPassInstance = forwardRenderPass->CreateInstance(forwardBaseOutput, false);
					forwardBaseOutput->GetFrameBuffer()->GetRenderAttachments().GetTextures(textures);
				}

				forwardRenderPass->Bind();
				sharedRes->pipelineManager.PushModuleInstance(&forwardBasePassParams);
				sharedRes->pipelineManager.PushModuleInstance(&lighting.moduleInstance);

				transparentPassInstance->SetFixedOrderDrawContent(sharedRes->pipelineManager, reorderBuffer.GetArrayView());
				sharedRes->pipelineManager.PopModuleInstance();
				sharedRes->pipelineManager.PopModuleInstance();
				task.AddImageTransferTask(textures.GetArrayView(), CoreLib::ArrayView<Texture*>());
				task.AddTask(transparentPassInstance);
				task.AddImageTransferTask(CoreLib::ArrayView<Texture*>(), textures.GetArrayView());
			}

			if (toneMapping)
			{
				if (useAtmosphere)
				{
					task.AddTask(toneMappingFromAtmospherePass->CreateInstance(sharedModules));
				}
				else
				{
					task.AddTask(toneMappingFromLitColorPass->CreateInstance(sharedModules));
				}
			}
		}
	};

	IRenderProcedure * CreateStandardRenderProcedure(bool toneMapping, bool useEnvMap)
	{
		return new StandardRenderProcedure(toneMapping, useEnvMap);
	}
}