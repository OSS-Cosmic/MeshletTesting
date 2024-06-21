/*
 * Copyright (c) 2017-2024 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit Test for testing transformations using a solar system.
// Tests the basic mat4 transformations, such as scaling, rotation, and
// translation.

#include "offsetAllocator.h"

#include "tinyimageformat_query.h"
#define MAX_PLANETS                                                            \
  20 // Does not affect test, just for allocating space in uniform block. Must
     // match with shader.

// Define these only in *one* .cc file.

// Interfaces
#include "Common_3/Application/Interfaces/IApp.h"
#include "Common_3/Application/Interfaces/ICameraController.h"
#include "Common_3/Application/Interfaces/IFont.h"
#include "Common_3/Application/Interfaces/IInput.h"
#include "Common_3/Application/Interfaces/IProfiler.h"
#include "Common_3/Application/Interfaces/IScreenshot.h"
#include "Common_3/Application/Interfaces/IUI.h"
#include "Common_3/Game/Interfaces/IScripting.h"
#include "Common_3/Utilities/Interfaces/IFileSystem.h"
#include "Common_3/Utilities/Interfaces/ILog.h"
#include "Common_3/Utilities/Interfaces/ITime.h"

#include "Common_3/Utilities/RingBuffer.h"

// Renderer
#include "Common_3/Graphics/Interfaces/IGraphics.h"
#include "Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"

#include "Common_3/Tools/ThirdParty/OpenSource/meshoptimizer/src/meshoptimizer.h"

// Math
#include "Common_3/Utilities/Math/MathTypes.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_gltf.h"

#include "Common_3/Utilities/Interfaces/IMemory.h"


/// Demo structures
struct PlanetInfoStruct {
  mat4 mTranslationMat;
  mat4 mScaleMat;
  mat4 mSharedMat; // Matrix to pass down to children
  vec4 mColor;
  uint mParentIndex;
  float mYOrbitSpeed; // Rotation speed around parent
  float mZOrbitSpeed;
  float mRotationSpeed; // Rotation speed around self
  float mMorphingSpeed; // Speed of morphing betwee cube and sphere
};

struct UniformBlock {
  CameraMatrix mProjectView;
  mat4 mToWorldMat[MAX_PLANETS];
  vec4 mColor[MAX_PLANETS];
  float mGeometryWeight[MAX_PLANETS][4];

  // Point Light Information
  vec3 mLightPosition;
  vec3 mLightColor;
};

struct UniformBlockSky {
  CameraMatrix mProjectView;
};

// But we only need Two sets of resources (one in flight and one being used on
// CPU)
const uint32_t gDataBufferCount = 2;
const uint gTimeOffset = 600000; // For visually better starting locations
const float gRotSelfScale = 0.0004f;
const float gRotOrbitYScale = 0.001f;
const float gRotOrbitZScale = 0.00001f;

Renderer *pRenderer = NULL;

Queue *pGraphicsQueue = NULL;
GpuCmdRing gGraphicsCmdRing = {};

SwapChain *pSwapChain = NULL;
RenderTarget *pDepthBuffer = NULL;
Semaphore *pImageAcquiredSemaphore = NULL;

Shader *pOpaqueShader = NULL;
RootSignature *pRootSignature = NULL;
Sampler *pSampler0 = NULL;

uint32_t gFrameIndex = 0;
ProfileToken gGpuProfileToken = PROFILE_INVALID_TOKEN;

ICameraController *pCameraController = NULL;

UIComponent *pGuiWindow = NULL;

struct MeshletSlot {
    OffsetAllocator::Allocation m_vertexAlloc;
    OffsetAllocator::Allocation m_indexAlloc;
    size_t m_numVerts;
    size_t m_numIndecies;
};
MeshletSlot* meshletSlots = NULL; 

uint32_t gFontID = 0;

QueryPool *pPipelineStatsQueryPool[gDataBufferCount] = {};
FontDrawDesc gFrameTimeDraw;

struct MeshletEntry {
  uint32_t materialID;
  uint32_t mNumVerts;
  uint32_t mNumIndecies;
  OffsetAllocator::Allocation vertexAlloc;
  OffsetAllocator::Allocation indexAlloc;
};


#define OPAQUE_POSITION_ELEMENT_SIZE sizeof(float3)
#define OPAQUE_INDEX_ELEMENT_SIZE sizeof(uint32_t)
#define OPAQUE_NUM_VERTS 6000000
#define OPAQUE_NUM_INDICES 6000000

OffsetAllocator::Allocator* opaqueIndexAlloc;
OffsetAllocator::Allocator* opaqueVertexAlloc;
Buffer* opaqueIndexBuffer;
Buffer* opaquePositionBuffer;

static unsigned char gPipelineStatsCharArray[2048] = {};
static bstring gPipelineStats = bfromarr(gPipelineStatsCharArray);

class MeshletViewer : public IApp {
public:
  bstring mSceneGLTF;

  MeshletViewer() {
    for (int i = 0; i < argc; i += 1) {
      if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
        mSceneGLTF = bdynfromcstr(argv[i + 1]);
      }
    }
  }

  bool Init() {
    // FILE PATHS
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_BINARIES,
                            "CompiledShaders");
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_TEXTURES, "Textures");
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");
    fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_SCREENSHOTS,
                            "Screenshots");
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SCRIPTS, "Scripts");
    fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_DEBUG, "Debug");

    // window and renderer setup
    RendererDesc settings;
    memset(&settings, 0, sizeof(settings));
    settings.mD3D11Supported = true;
    settings.mGLESSupported = true;
    initRenderer(GetName(), &settings, &pRenderer);
    // check for init success
    if (!pRenderer)
      return false;

    {
      opaqueIndexAlloc = (OffsetAllocator::Allocator*)tf_calloc(1, sizeof(OffsetAllocator::Allocator));
      opaqueVertexAlloc = (OffsetAllocator::Allocator*)tf_calloc(1, sizeof(OffsetAllocator::Allocator));
      tf_placement_new<OffsetAllocator::Allocator>(opaqueIndexAlloc, OPAQUE_NUM_VERTS );
      tf_placement_new<OffsetAllocator::Allocator>(opaqueVertexAlloc, OPAQUE_NUM_INDICES);
      {
        BufferLoadDesc loadDesc = {};
        loadDesc.ppBuffer = &opaqueIndexBuffer;
        loadDesc.mDesc.mDescriptors =
            DESCRIPTOR_TYPE_INDEX_BUFFER | DESCRIPTOR_TYPE_BUFFER_RAW;
        loadDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        loadDesc.mDesc.mStructStride = OPAQUE_INDEX_ELEMENT_SIZE;
        loadDesc.mDesc.mElementCount = OPAQUE_NUM_INDICES;
        loadDesc.mDesc.mSize = OPAQUE_NUM_INDICES * OPAQUE_INDEX_ELEMENT_SIZE;
        loadDesc.mDesc.pName = "Opaque Index Buffer";
        addResource(&loadDesc, nullptr);
      }
      {
        BufferLoadDesc loadDesc = {};
        loadDesc.ppBuffer = &opaquePositionBuffer;
        loadDesc.mDesc.mDescriptors =
            DESCRIPTOR_TYPE_VERTEX_BUFFER | DESCRIPTOR_TYPE_BUFFER_RAW;
        loadDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        loadDesc.mDesc.mStructStride = OPAQUE_POSITION_ELEMENT_SIZE;
        loadDesc.mDesc.mElementCount = OPAQUE_NUM_VERTS;
        loadDesc.mDesc.mSize = OPAQUE_NUM_VERTS * OPAQUE_POSITION_ELEMENT_SIZE;
        loadDesc.mDesc.pName = "Opaque Position Buffer";
        addResource(&loadDesc, NULL);
      }
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    if (!loader.LoadASCIIFromFile(&model, &err, &warn,
                                  (char *)mSceneGLTF.data)) {
      printf("failed to load GLTF: %s", (char *)mSceneGLTF.data);
      if (!warn.empty()) {
        printf("Warn: %s\n", warn.c_str());
      }

      if (!err.empty()) {
        printf("Err: %s\n", err.c_str());
      }
      return false;
    }
    const size_t max_vertices = 64;
    const size_t max_triangles = 124;
    const float cone_weight = 0.0f;

    uint32_t* meshletVerts = NULL;
    uint8_t* meshletTries = NULL;
    meshopt_Meshlet* mesoptsMeshlets = NULL;

    for (auto& meshes : model.meshes) {
        for (auto& prim : meshes.primitives) {
            tinygltf::Accessor indexAccess = model.accessors[prim.indices];
            auto& indexBufferView = model.bufferViews[indexAccess.bufferView];
            auto& indexBuffer = model.buffers[indexBufferView.buffer];

            ASSERT(indexAccess.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
            const size_t numberIndecies = indexBufferView.byteLength / indexBufferView.byteStride;

            tinygltf::Accessor* positionAccessor = NULL;

            for (auto& attrib : prim.attributes) {
                tinygltf::Accessor accessor = model.accessors[attrib.second];
                auto& bufferView = model.bufferViews[accessor.bufferView];
                auto& buffer = model.buffers[bufferView.buffer];
                if (attrib.first.compare("POSITION") == 0) {
                    ASSERT(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);
                    ASSERT(accessor.type == TINYGLTF_TYPE_VEC3);
                    positionAccessor = &accessor;
                }
            }

            auto& positionBufferView = model.bufferViews[positionAccessor->bufferView];
            auto& positionBuffer = model.buffers[positionBufferView.buffer];

            const size_t numberElements = positionBufferView.byteLength / positionBufferView.byteStride;

            const size_t max_meshlets = meshopt_buildMeshletsBound(numberIndecies, 64, 124);
            arrsetlen(mesoptsMeshlets, max_meshlets);
            arrsetlen(meshletVerts, max_meshlets * max_vertices);
            arrsetlen(meshletTries, max_meshlets * max_triangles * 3);
            size_t meshlet_count = meshopt_buildMeshlets(
                mesoptsMeshlets,
                meshletVerts,
                meshletTries,
                (uint32_t*)indexBuffer.data.data(),
                numberIndecies,
                (float*)positionBuffer.data.data(),
                numberElements,
                positionBufferView.byteStride,
                max_vertices,
                max_triangles,
                cone_weight);
            for (size_t i = 0; i < meshlet_count; i++) {
                MeshletSlot meshlet = { 0 };

                OffsetAllocator::Allocation vertexAlloc = opaqueVertexAlloc->allocate(mesoptsMeshlets->vertex_count);
                OffsetAllocator::Allocation indexAlloc = opaqueIndexAlloc->allocate(mesoptsMeshlets->triangle_count * 3);
                BufferUpdateDesc positionUpdateDesc = { opaquePositionBuffer,
                                                        vertexAlloc.offset * OPAQUE_POSITION_ELEMENT_SIZE,
                                                        mesoptsMeshlets->vertex_count * OPAQUE_POSITION_ELEMENT_SIZE };
                BufferUpdateDesc indexUpdateDesc = { opaqueIndexBuffer,
                                                     indexAlloc.offset * OPAQUE_INDEX_ELEMENT_SIZE,
                                                     (mesoptsMeshlets->triangle_count * 3) * OPAQUE_INDEX_ELEMENT_SIZE };

                beginUpdateResource(&positionUpdateDesc);
                for (size_t j = 0; j < mesoptsMeshlets[i].vertex_count; j++) {
                    memcpy(
                        ((uint8_t*)positionUpdateDesc.pMappedData) + (j * OPAQUE_POSITION_ELEMENT_SIZE),
                        (positionBuffer.data.data() + positionBufferView.byteOffset) +
                            (meshletVerts[j + mesoptsMeshlets[i].vertex_offset] * positionBufferView.byteStride),
                        min(positionBufferView.byteStride, OPAQUE_POSITION_ELEMENT_SIZE));
                }
                endUpdateResource(&positionUpdateDesc);

                beginUpdateResource(&indexUpdateDesc);
                for (size_t j = 0; j < mesoptsMeshlets[i].triangle_count; j++) {
                    *((uint32_t*)(((uint8_t*)indexUpdateDesc.pMappedData) + (((j * 3) + 0) * OPAQUE_INDEX_ELEMENT_SIZE))) =
                        meshletTries[((j + mesoptsMeshlets[i].triangle_offset) * 3) + 0];
                    *((uint32_t*)(((uint8_t*)indexUpdateDesc.pMappedData) + (((j * 3) + 1) * OPAQUE_INDEX_ELEMENT_SIZE))) =
                        meshletTries[((j + mesoptsMeshlets[i].triangle_offset) * 3) + 1];
                    *((uint32_t*)(((uint8_t*)indexUpdateDesc.pMappedData) + (((j * 3) + 2) * OPAQUE_INDEX_ELEMENT_SIZE))) =
                        meshletTries[((j + mesoptsMeshlets[i].triangle_offset) * 3) + 2];
                }
                endUpdateResource(&indexUpdateDesc);
                meshlet.m_indexAlloc = indexAlloc;
                meshlet.m_vertexAlloc = vertexAlloc;
                meshlet.m_numVerts = mesoptsMeshlets[i].vertex_count;
                meshlet.m_numIndecies = mesoptsMeshlets[i].triangle_count * 3;
                arrpush(meshletSlots, meshlet);
            }
        }
    }

    arrfree(meshletVerts);
    arrfree(meshletTries);
    arrfree(mesoptsMeshlets);

    if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
        QueryPoolDesc poolDesc = {};
        poolDesc.mQueryCount = 3; // The count is 3 due to quest & multi-view use
                                  // otherwise 2 is enough as we use 2 queries.
        poolDesc.mType = QUERY_TYPE_PIPELINE_STATISTICS;
        for (uint32_t i = 0; i < gDataBufferCount; ++i) {
            addQueryPool(pRenderer, &poolDesc, &pPipelineStatsQueryPool[i]);
        }
    }

    QueueDesc queueDesc = {};
    queueDesc.mType = QUEUE_TYPE_GRAPHICS;
    queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
    addQueue(pRenderer, &queueDesc, &pGraphicsQueue);

    GpuCmdRingDesc cmdRingDesc = {};
    cmdRingDesc.pQueue = pGraphicsQueue;
    cmdRingDesc.mPoolCount = gDataBufferCount;
    cmdRingDesc.mCmdPerPoolCount = 1;
    cmdRingDesc.mAddSyncPrimitives = true;
    addGpuCmdRing(pRenderer, &cmdRingDesc, &gGraphicsCmdRing);

    addSemaphore(pRenderer, &pImageAcquiredSemaphore);

    initResourceLoaderInterface(pRenderer);

    // Loads Skybox Textures
    //for (int i = 0; i < 6; ++i) {
    //    TextureLoadDesc textureDesc = {};
    //    textureDesc.pFileName = pSkyBoxImageFileNames[i];
    //    textureDesc.ppTexture = &pSkyBoxTextures[i];
    //    // Textures representing color should be stored in SRGB or HDR format
    //    textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
    //    addResource(&textureDesc, NULL);
    //}

    // Dynamic sampler that is bound at runtime
    SamplerDesc samplerDesc = { FILTER_LINEAR,
                                FILTER_LINEAR,
                                MIPMAP_MODE_NEAREST,
                                ADDRESS_MODE_CLAMP_TO_EDGE,
                                ADDRESS_MODE_CLAMP_TO_EDGE,
                                ADDRESS_MODE_CLAMP_TO_EDGE };
    addSampler(pRenderer, &samplerDesc, &pSampler0);

    //uint64_t skyBoxDataSize = 4 * 6 * 6 * sizeof(float);
    //BufferLoadDesc skyboxVbDesc = {};
    //skyboxVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
    //skyboxVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    //skyboxVbDesc.mDesc.mSize = skyBoxDataSize;
    //skyboxVbDesc.pData = gSkyBoxPoints;
    //skyboxVbDesc.ppBuffer = &pSkyBoxVertexBuffer;
    //addResource(&skyboxVbDesc, NULL);

    // BufferLoadDesc ubDesc = {};
    // ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    // ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    // ubDesc.pData = NULL;
    // for (uint32_t i = 0; i < gDataBufferCount; ++i) {
    //   ubDesc.mDesc.pName = "ProjViewUniformBuffer";
    //   ubDesc.mDesc.mSize = sizeof(UniformBlock);
    //   ubDesc.ppBuffer = &pProjViewUniformBuffer[i];
    //   addResource(&ubDesc, NULL);
    //   ubDesc.mDesc.pName = "SkyboxUniformBuffer";
    //   ubDesc.mDesc.mSize = sizeof(UniformBlockSky);
    //   ubDesc.ppBuffer = &pSkyboxUniformBuffer[i];
    //   addResource(&ubDesc, NULL);
    // }

    // Load fonts
    FontDesc font = {};
    font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
    fntDefineFonts(&font, 1, &gFontID);

    FontSystemDesc fontRenderDesc = {};
    fontRenderDesc.pRenderer = pRenderer;
    if (!initFontSystem(&fontRenderDesc))
        return false; // report?

    // Initialize Forge User Interface Rendering
    UserInterfaceDesc uiRenderDesc = {};
    uiRenderDesc.pRenderer = pRenderer;
    initUserInterface(&uiRenderDesc);

    // Initialize micro profiler and its UI.
    ProfilerDesc profiler = {};
    profiler.pRenderer = pRenderer;
    profiler.mWidthUI = mSettings.mWidth;
    profiler.mHeightUI = mSettings.mHeight;
    initProfiler(&profiler);

    // Gpu profiler can only be added after initProfile.
    gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

    /************************************************************************/
    // GUI
    /************************************************************************/
    UIComponentDesc guiDesc = {};
    guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
    uiCreateComponent(GetName(), &guiDesc, &pGuiWindow);

    //SliderUintWidget vertexLayoutWidget;
    //vertexLayoutWidget.mMin = 0;
    //vertexLayoutWidget.mMax = 1;
    //vertexLayoutWidget.mStep = 1;
    //vertexLayoutWidget.pData = &gSphereLayoutType;
    //UIWidget* pVLw = uiCreateComponentWidget(pGuiWindow, "Vertex Layout", &vertexLayoutWidget, WIDGET_TYPE_SLIDER_UINT);
    //uiSetWidgetOnEditedCallback(pVLw, nullptr, reloadRequest);

    if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
        static float4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
        DynamicTextWidget statsWidget;
        statsWidget.pText = &gPipelineStats;
        statsWidget.pColor = &color;
        uiCreateComponentWidget(pGuiWindow, "Pipeline Stats", &statsWidget, WIDGET_TYPE_DYNAMIC_TEXT);
    }

    waitForAllResourceLoads();

    CameraMotionParameters cmp{ 160.0f, 600.0f, 200.0f };
    vec3 camPos{ 48.0f, 48.0f, 20.0f };
    vec3 lookAt{ vec3(0) };

    pCameraController = initFpsCameraController(camPos, lookAt);

    pCameraController->setMotionParameters(cmp);

    InputSystemDesc inputDesc = {};
    inputDesc.pRenderer = pRenderer;
    inputDesc.pWindow = pWindow;
    inputDesc.pJoystickTexture = "circlepad.tex";
    if (!initInputSystem(&inputDesc))
        return false;

    // App Actions
    InputActionDesc actionDesc = { DefaultInputActions::DUMP_PROFILE_DATA,
                                   [](InputActionContext* ctx) {
                                       dumpProfileData(((Renderer*)ctx->pUserData)->pName);
                                       return true;
                                   },
                                   pRenderer };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::TOGGLE_FULLSCREEN,
                   [](InputActionContext* ctx) {
                       WindowDesc* winDesc = ((IApp*)ctx->pUserData)->pWindow;
                       if (winDesc->fullScreen)
                           winDesc->borderlessWindow
                               ? setBorderless(winDesc, getRectWidth(&winDesc->clientRect), getRectHeight(&winDesc->clientRect))
                               : setWindowed(winDesc, getRectWidth(&winDesc->clientRect), getRectHeight(&winDesc->clientRect));
                       else
                           setFullscreen(winDesc);
                       return true;
                   },
                   this };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::EXIT, [](InputActionContext* ctx) {
                      requestShutdown();
                      return true;
                  } };
    addInputAction(&actionDesc);
    InputActionCallback onAnyInput = [](InputActionContext* ctx) {
        if (ctx->mActionId > UISystemInputActions::UI_ACTION_START_ID_) {
            uiOnInput(ctx->mActionId, ctx->mBool, ctx->pPosition, &ctx->mFloat2);
        }

        return true;
    };

    typedef bool (*CameraInputHandler)(InputActionContext* ctx, DefaultInputActions::DefaultInputAction action);
    static CameraInputHandler onCameraInput = [](InputActionContext* ctx, DefaultInputActions::DefaultInputAction action) {
        if (*(ctx->pCaptured)) {
            float2 delta = uiIsFocused() ? float2(0.f, 0.f) : ctx->mFloat2;
            switch (action) {
            case DefaultInputActions::ROTATE_CAMERA:
                pCameraController->onRotate(delta);
                break;
            case DefaultInputActions::TRANSLATE_CAMERA:
                pCameraController->onMove(delta);
                break;
            case DefaultInputActions::TRANSLATE_CAMERA_VERTICAL:
                pCameraController->onMoveY(delta[0]);
                break;
            default:
                break;
            }
        }
        return true;
    };
    actionDesc = { DefaultInputActions::CAPTURE_INPUT,
                   [](InputActionContext* ctx) {
                       setEnableCaptureInput(!uiIsFocused() && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);
                       return true;
                   },
                   NULL };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::ROTATE_CAMERA,
                   [](InputActionContext* ctx) {
                       return onCameraInput(ctx, DefaultInputActions::ROTATE_CAMERA);
                   },
                   NULL };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::TRANSLATE_CAMERA,
                   [](InputActionContext* ctx) {
                       return onCameraInput(ctx, DefaultInputActions::TRANSLATE_CAMERA);
                   },
                   NULL };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::TRANSLATE_CAMERA_VERTICAL,
                   [](InputActionContext* ctx) {
                       return onCameraInput(ctx, DefaultInputActions::TRANSLATE_CAMERA_VERTICAL);
                   },
                   NULL };
    addInputAction(&actionDesc);
    actionDesc = { DefaultInputActions::RESET_CAMERA, [](InputActionContext* ctx) {
                      if (!uiWantTextInput())
                          pCameraController->resetView();
                      return true;
                  } };
    addInputAction(&actionDesc);
    GlobalInputActionDesc globalInputActionDesc = { GlobalInputActionDesc::ANY_BUTTON_ACTION, onAnyInput, this };
    setGlobalInputAction(&globalInputActionDesc);

    gFrameIndex = 0;

    return true;
  }

  void Exit() {
      exitInputSystem();

      exitCameraController(pCameraController);

      exitUserInterface();

      exitFontSystem();

      // Exit profile
      exitProfiler();

      for (uint32_t i = 0; i < gDataBufferCount; ++i) {
          if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
              removeQueryPool(pRenderer, pPipelineStatsQueryPool[i]);
          }
      }

      removeSampler(pRenderer, pSampler0);

      removeGpuCmdRing(pRenderer, &gGraphicsCmdRing);
      removeSemaphore(pRenderer, pImageAcquiredSemaphore);

      exitResourceLoaderInterface(pRenderer);

      removeQueue(pRenderer, pGraphicsQueue);

      exitRenderer(pRenderer);
      pRenderer = NULL;
  }

  bool Load(ReloadDesc* pReloadDesc) {
      if (pReloadDesc->mType & RELOAD_TYPE_SHADER) {
          addShaders();
          addRootSignatures();
          addDescriptorSets();
      }

      if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET)) {
          if (!addSwapChain())
              return false;

          if (!addDepthBuffer())
              return false;
      }

      if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET)) {
          addPipelines();
      }

      UserInterfaceLoadDesc uiLoad = {};
      uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
      uiLoad.mHeight = mSettings.mHeight;
      uiLoad.mWidth = mSettings.mWidth;
      uiLoad.mLoadType = pReloadDesc->mType;
      loadUserInterface(&uiLoad);

      FontSystemLoadDesc fontLoad = {};
      fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
      fontLoad.mHeight = mSettings.mHeight;
      fontLoad.mWidth = mSettings.mWidth;
      fontLoad.mLoadType = pReloadDesc->mType;
      loadFontSystem(&fontLoad);

      initScreenshotInterface(pRenderer, pGraphicsQueue);

      return true;
  }

  void Unload(ReloadDesc* pReloadDesc) {
      waitQueueIdle(pGraphicsQueue);

      unloadFontSystem(pReloadDesc->mType);
      unloadUserInterface(pReloadDesc->mType);

      if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET)) {
          //removeResource(pSphereVertexBuffer);
          //removeResource(pSphereIndexBuffer);
      }

      if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET)) {
          removeSwapChain(pRenderer, pSwapChain);
          removeRenderTarget(pRenderer, pDepthBuffer);
      }

      if (pReloadDesc->mType & RELOAD_TYPE_SHADER) {
          removeDescriptorSets();
          removeRootSignatures();
          removeShaders();
      }

      exitScreenshotInterface();
  }

  void Update(float deltaTime) {
      updateInputSystem(deltaTime, mSettings.mWidth, mSettings.mHeight);

      pCameraController->update(deltaTime);
      /************************************************************************/
      // Scene Update
      /************************************************************************/
      static float currentTime = 0.0f;
      currentTime += deltaTime * 1000.0f;

      // update camera with time
      mat4 viewMat = pCameraController->getViewMatrix();

      const float aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
      const float horizontal_fov = PI / 2.0f;
      CameraMatrix projMat = CameraMatrix::perspectiveReverseZ(horizontal_fov, aspectInverse, 0.1f, 1000.0f);
      //gUniformData.mProjectView = projMat * viewMat;

      // point light parameters
      //gUniformData.mLightPosition = vec3(0, 0, 0);
      //gUniformData.mLightColor = vec3(0.9f, 0.9f, 0.7f); // Pale Yellow

     // // update planet transformations
     // for (unsigned int i = 0; i < gNumPlanets; i++) {
     //     mat4 rotSelf, rotOrbitY, rotOrbitZ, trans, scale, parentMat;
     //     rotSelf = rotOrbitY = rotOrbitZ = parentMat = mat4::identity();
     //     if (gPlanetInfoData[i].mRotationSpeed > 0.0f)
     //         rotSelf = mat4::rotationY(gRotSelfScale * (currentTime + gTimeOffset) / gPlanetInfoData[i].mRotationSpeed);
     //     if (gPlanetInfoData[i].mYOrbitSpeed > 0.0f)
     //         rotOrbitY = mat4::rotationY(gRotOrbitYScale * (currentTime + gTimeOffset) / gPlanetInfoData[i].mYOrbitSpeed);
     //     if (gPlanetInfoData[i].mZOrbitSpeed > 0.0f)
     //         rotOrbitZ = mat4::rotationZ(gRotOrbitZScale * (currentTime + gTimeOffset) / gPlanetInfoData[i].mZOrbitSpeed);
     //     if (gPlanetInfoData[i].mParentIndex > 0)
     //         parentMat = gPlanetInfoData[gPlanetInfoData[i].mParentIndex].mSharedMat;

     //     trans = gPlanetInfoData[i].mTranslationMat;
     //     scale = gPlanetInfoData[i].mScaleMat;

     //     scale[0][0] /= 2;
     //     scale[1][1] /= 2;
     //     scale[2][2] /= 2;

     //     gPlanetInfoData[i].mSharedMat = parentMat * rotOrbitY * trans;
     //     gUniformData.mToWorldMat[i] = parentMat * rotOrbitY * rotOrbitZ * trans * rotSelf * scale;
     //     gUniformData.mColor[i] = gPlanetInfoData[i].mColor;

     //     float step;
     //     float phase = modf(currentTime * gPlanetInfoData[i].mMorphingSpeed / 2000.f, &step);
     //     if (phase > 0.5f)
     //         phase = 2 - phase * 2;
     //     else
     //         phase = phase * 2;

     //     gUniformData.mGeometryWeight[i][0] = phase;
     // }

      viewMat.setTranslation(vec3(0));
      //gUniformDataSky = {};
      //gUniformDataSky.mProjectView = projMat * viewMat;
  }

  void Draw() {
      if (pSwapChain->mEnableVsync != mSettings.mVSyncEnabled) {
          waitQueueIdle(pGraphicsQueue);
          ::toggleVSync(pRenderer, &pSwapChain);
      }

      uint32_t swapchainImageIndex;
      acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

      RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
      GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);

      // Stall if CPU is running "gDataBufferCount" frames ahead of GPU
      FenceStatus fenceStatus;
      getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
      if (fenceStatus == FENCE_STATUS_INCOMPLETE)
          waitForFences(pRenderer, 1, &elem.pFence);

      // Update uniform buffers
      //BufferUpdateDesc viewProjCbv = { pProjViewUniformBuffer[gFrameIndex] };
      //beginUpdateResource(&viewProjCbv);
      //memcpy(viewProjCbv.pMappedData, &gUniformData, sizeof(gUniformData));
      //endUpdateResource(&viewProjCbv);

      //BufferUpdateDesc skyboxViewProjCbv = { pSkyboxUniformBuffer[gFrameIndex] };
      //beginUpdateResource(&skyboxViewProjCbv);
      //memcpy(skyboxViewProjCbv.pMappedData, &gUniformDataSky, sizeof(gUniformDataSky));
      //endUpdateResource(&skyboxViewProjCbv);

      // Reset cmd pool for this frame
      resetCmdPool(pRenderer, elem.pCmdPool);

      if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
          QueryData data3D = {};
          QueryData data2D = {};
          getQueryData(pRenderer, pPipelineStatsQueryPool[gFrameIndex], 0, &data3D);
          getQueryData(pRenderer, pPipelineStatsQueryPool[gFrameIndex], 1, &data2D);
          bformat(
              &gPipelineStats,
              "\n"
              "Pipeline Stats 3D:\n"
              "    VS invocations:      %u\n"
              "    PS invocations:      %u\n"
              "    Clipper invocations: %u\n"
              "    IA primitives:       %u\n"
              "    Clipper primitives:  %u\n"
              "\n"
              "Pipeline Stats 2D UI:\n"
              "    VS invocations:      %u\n"
              "    PS invocations:      %u\n"
              "    Clipper invocations: %u\n"
              "    IA primitives:       %u\n"
              "    Clipper primitives:  %u\n",
              data3D.mPipelineStats.mVSInvocations,
              data3D.mPipelineStats.mPSInvocations,
              data3D.mPipelineStats.mCInvocations,
              data3D.mPipelineStats.mIAPrimitives,
              data3D.mPipelineStats.mCPrimitives,
              data2D.mPipelineStats.mVSInvocations,
              data2D.mPipelineStats.mPSInvocations,
              data2D.mPipelineStats.mCInvocations,
              data2D.mPipelineStats.mIAPrimitives,
              data2D.mPipelineStats.mCPrimitives);
      }

      Cmd* cmd = elem.pCmds[0];
      beginCmd(cmd);

      cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);
      if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
          cmdResetQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], 0, 2);
          QueryDesc queryDesc = { 0 };
          cmdBeginQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], &queryDesc);
      }

      RenderTargetBarrier barriers[] = {
          { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
      };
      cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

      cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw Skybox/Planets");

      // simply record the screen cleaning command
      BindRenderTargetsDesc bindRenderTargets = {};
      bindRenderTargets.mRenderTargetCount = 1;
      bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
      bindRenderTargets.mDepthStencil = { pDepthBuffer, LOAD_ACTION_CLEAR };
      cmdBindRenderTargets(cmd, &bindRenderTargets);
      cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
      cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

     // cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw Planets");

     // cmdBindPipeline(cmd, pSpherePipeline);
     // cmdBindDescriptorSet(cmd, gFrameIndex * 2 + 1, pDescriptorSetUniforms);
     // cmdBindVertexBuffer(cmd, 1, &pSphereVertexBuffer, &gSphereVertexLayout.mBindings[0].mStride, nullptr);
     // cmdBindIndexBuffer(cmd, pSphereIndexBuffer, INDEX_TYPE_UINT16, 0);

     // cmdDrawIndexedInstanced(cmd, gSphereIndexCount, 0, gNumPlanets, 0, 0);
     // cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
     // cmdEndGpuTimestampQuery(cmd, gGpuProfileToken); // Draw Skybox/Planets
     // cmdBindRenderTargets(cmd, NULL);

      if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
          QueryDesc queryDesc = { 0 };
          cmdEndQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], &queryDesc);

          queryDesc = { 1 };
          cmdBeginQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], &queryDesc);
      }

      cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");

      bindRenderTargets = {};
      bindRenderTargets.mRenderTargetCount = 1;
      bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_LOAD };
      bindRenderTargets.mDepthStencil = { NULL, LOAD_ACTION_DONTCARE };
      cmdBindRenderTargets(cmd, &bindRenderTargets);

      gFrameTimeDraw.mFontColor = 0xff00ffff;
      gFrameTimeDraw.mFontSize = 18.0f;
      gFrameTimeDraw.mFontID = gFontID;
      float2 txtSizePx = cmdDrawCpuProfile(cmd, float2(8.f, 15.f), &gFrameTimeDraw);
      cmdDrawGpuProfile(cmd, float2(8.f, txtSizePx.y + 75.f), gGpuProfileToken, &gFrameTimeDraw);

      cmdDrawUserInterface(cmd);

      cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
      cmdBindRenderTargets(cmd, NULL);

      barriers[0] = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
      cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

      cmdEndGpuFrameProfile(cmd, gGpuProfileToken);

      if (pRenderer->pGpu->mSettings.mPipelineStatsQueries) {
          QueryDesc queryDesc = { 1 };
          cmdEndQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], &queryDesc);
          cmdResolveQuery(cmd, pPipelineStatsQueryPool[gFrameIndex], 0, 2);
      }

      endCmd(cmd);

      FlushResourceUpdateDesc flushUpdateDesc = {};
      flushUpdateDesc.mNodeIndex = 0;
      flushResourceUpdates(&flushUpdateDesc);
      Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

      QueueSubmitDesc submitDesc = {};
      submitDesc.mCmdCount = 1;
      submitDesc.mSignalSemaphoreCount = 1;
      submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
      submitDesc.ppCmds = &cmd;
      submitDesc.ppSignalSemaphores = &elem.pSemaphore;
      submitDesc.ppWaitSemaphores = waitSemaphores;
      submitDesc.pSignalFence = elem.pFence;
      queueSubmit(pGraphicsQueue, &submitDesc);

      QueuePresentDesc presentDesc = {};
      presentDesc.mIndex = swapchainImageIndex;
      presentDesc.mWaitSemaphoreCount = 1;
      presentDesc.pSwapChain = pSwapChain;
      presentDesc.ppWaitSemaphores = &elem.pSemaphore;
      presentDesc.mSubmitDone = true;

      queuePresent(pGraphicsQueue, &presentDesc);
      flipProfiler();

      gFrameIndex = (gFrameIndex + 1) % gDataBufferCount;
  }

  const char* GetName() {
      return "01_Transformations";
  }

  bool addSwapChain() {
      SwapChainDesc swapChainDesc = {};
      swapChainDesc.mWindowHandle = pWindow->handle;
      swapChainDesc.mPresentQueueCount = 1;
      swapChainDesc.ppPresentQueues = &pGraphicsQueue;
      swapChainDesc.mWidth = mSettings.mWidth;
      swapChainDesc.mHeight = mSettings.mHeight;
      swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
      swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
      swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
      swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
      swapChainDesc.mFlags = SWAP_CHAIN_CREATION_FLAG_ENABLE_FOVEATED_RENDERING_VR;
      ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

      return pSwapChain != NULL;
  }

  bool addDepthBuffer() {
      // Add depth buffer
      RenderTargetDesc depthRT = {};
      depthRT.mArraySize = 1;
      depthRT.mClearValue.depth = 0.0f;
      depthRT.mClearValue.stencil = 0;
      depthRT.mDepth = 1;
      depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
      depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
      depthRT.mHeight = mSettings.mHeight;
      depthRT.mSampleCount = SAMPLE_COUNT_1;
      depthRT.mSampleQuality = 0;
      depthRT.mWidth = mSettings.mWidth;
      depthRT.mFlags = TEXTURE_CREATION_FLAG_ON_TILE | TEXTURE_CREATION_FLAG_VR_MULTIVIEW;
      addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

      return pDepthBuffer != NULL;
  }

  void addDescriptorSets() {
  }

  void removeDescriptorSets() {
  }

  void addRootSignatures() {
      Shader* shaders[2];
      uint32_t shadersCount = 0;
      //shaders[shadersCount++] = pSphereShader;
      //shaders[shadersCount++] = pSkyBoxDrawShader;

      RootSignatureDesc rootDesc = {};
      rootDesc.mShaderCount = shadersCount;
      rootDesc.ppShaders = shaders;
      addRootSignature(pRenderer, &rootDesc, &pRootSignature);
  }

  void removeRootSignatures() {
      removeRootSignature(pRenderer, pRootSignature);
  }

  void addShaders() {
      //ShaderLoadDesc skyShader = {};
      //skyShader.mStages[0].pFileName = "skybox.vert";
      //skyShader.mStages[1].pFileName = "skybox.frag";

      ShaderLoadDesc basicShader = {};
      basicShader.mStages[0].pFileName = "basic.vert";
      basicShader.mStages[1].pFileName = "basic.frag";

      //addShader(pRenderer, &skyShader, &pSkyBoxDrawShader);
      addShader(pRenderer, &basicShader, &pOpaqueShader);
  }

  void removeShaders() {
      removeShader(pRenderer, pOpaqueShader);
  }

  void addPipelines() {
      RasterizerStateDesc rasterizerStateDesc = {};
      rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

      RasterizerStateDesc sphereRasterizerStateDesc = {};
      sphereRasterizerStateDesc.mCullMode = CULL_MODE_FRONT;

      DepthStateDesc depthStateDesc = {};
      depthStateDesc.mDepthTest = true;
      depthStateDesc.mDepthWrite = true;
      depthStateDesc.mDepthFunc = CMP_GEQUAL;
  }
};
DEFINE_APPLICATION_MAIN(MeshletViewer)
