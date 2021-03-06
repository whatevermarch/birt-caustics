#include "Renderer.h"

#include <iostream>
#include <sstream>

#ifdef _DEBUG
const bool VALIDATION_ENABLED = true;
#else
const bool VALIDATION_ENABLED = false;
#endif

#define APP_NAME "BIRT Caustics v0.1"

#define RESOURCES_PATH "..\\res\\"

#ifdef USE_TEST_SCENE
// For CornellBox
#define SCENE_PATH RESOURCES_PATH "CornellBox\\glTF\\test\\"
#define SCENE_FILENAME "test_x4.gltf"
#else
// For Sponza
#define SCENE_PATH RESOURCES_PATH "Cauldron-Media\\Sponza\\glTF\\"
#define SCENE_FILENAME "Sponza.gltf" 
#endif

class App : public FrameworkWindows
{
public:

    //  constructors
    App(LPCSTR name) : FrameworkWindows(name) {}

    //  overrided mandatory methods
    void OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight, bool* pbFullScreen) override;
    void OnCreate(HWND hWnd) override;
    void OnDestroy() override;
    void OnRender() override;
    bool OnEvent(MSG msg) override;
    void OnResize(uint32_t Width, uint32_t Height) override;
    void SetFullScreen(bool fullscreen) override;

protected:

    //  device
    Device device;

    //  display
    SwapChain swapChain;
    DisplayModes currentDisplayMode = DISPLAYMODE_SDR;
    std::vector<DisplayModes> availableDisplayModes;
    std::vector<const char*> availableDisplayModeNames;

    //  model loader (GLTF)
    SceneLoader* sceneLoader = nullptr;

    //  renderer
    Renderer* renderer = nullptr;
    Renderer::State renderer_state;
    float sun_pitch = XM_PI / 4.f;

    //  main camera
    Camera camera;

    //  time (ms.)
    double deltaTime;
    double lastFrameTime;

    //  caustics profiling
    //  workaround: target only one unit
    std::vector<float> profTimes;
    std::vector<float> accumProfTimes;
    unsigned int accumCount = 0;
    double accumInSec = 0;
};


void App::OnParseCommandLine(LPSTR lpCmdLine, uint32_t* pWidth, uint32_t* pHeight, bool* pbFullScreen)
{
    //  set window extent
    /**pWidth = 1280;
    *pHeight = 720;*/
    *pbFullScreen = false;
}

void App::OnCreate(HWND hWnd)
{
    //  check if cauldron-media repo exists
    //  this repo contains default multimedia and scene files needed for the first phase of the development.
    /*DWORD dwAttrib = GetFileAttributes(CAULDRON_MEDIA_PATH);
    if ((dwAttrib == INVALID_FILE_ATTRIBUTES) || ((dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) == 0)
    {
        MessageBox(NULL, "Scene files not found!\n\nPlease check the readme on how to get the media files.", "Cauldron Panic!", MB_ICONERROR);
        exit(0);
    }*/

    //  create device
    this->device.OnCreate("My App", "My Engine", VALIDATION_ENABLED, VALIDATION_ENABLED, hWnd);
    this->device.CreatePipelineCache();

    //  init shader compiler
    InitDirectXCompiler();
    CreateShaderCache();

    //  create swap chain
    uint32_t numBackBuffers = 2;
    this->swapChain.OnCreate(&this->device, numBackBuffers, hWnd);

    //  init renderer
    this->renderer = new Renderer();
    this->renderer->OnCreate(&this->device, &this->swapChain);

    //  setup initial state of renderer
    this->renderer_state.sunDir = PolarToVector(XM_PI / 2.f, this->sun_pitch);  // 45 degree

    //  setup initial camera settings
    {
#ifdef USE_TEST_SCENE
        const XMVECTOR eyePos = XMVectorSet(0.f, 0.752f, 2.8f, 0.f);
        const XMVECTOR lookDir = XMVectorSet(0.f, 0.35f, 0.f, 0.f);
#else
        const XMVECTOR eyePos = XMVectorSet(-4.4918f, 3.2016f, -1.3915f, 0.f);
        const XMVECTOR lookDir = XMVectorSet(-9.0900f, 0.8032f, 0.2913f, 0.f);
#endif
        this->camera.LookAt(eyePos, lookDir);
    }

    //  load scene (metadata)
    this->sceneLoader = new SceneLoader();
    if (!this->sceneLoader->Load(SCENE_PATH, SCENE_FILENAME))
    {
        MessageBox(NULL, "The selected model couldn't be found, please check the file path", "Cauldron Panic!", MB_ICONERROR);
        exit(0);
    }
#ifndef USE_TEST_SCENE
    //  tweak scene data
    {
        //  Sponza has no light cached in the data, so we add one
        tfNode n;
        //n.m_tranform.LookAt(this->renderer_state.sunDir * 20.5f, XMVectorSet(0, 0, 0, 0));

        const XMVECTOR lightPos = XMVectorSet(-6.8f, 2.1f, -0.35f, 0.f);
        const XMVECTOR shineDir = XMVectorSet(-8.1f, -0.522f, -0.35f, 0.f);
        n.m_tranform.LookAt(lightPos, shineDir);

        tfLight l;
        l.m_type = tfLight::LIGHT_SPOTLIGHT;
        l.m_intensity = 10.f;
        l.m_color = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
        l.m_range = 15;
        l.m_outerConeAngle = XM_PI / 4.0f;
        l.m_innerConeAngle = (XM_PI / 4.0f) * 0.9f;

        this->sceneLoader->AddLight(n, l);
    }
#endif
    //  init GUI subsystem
    ImGUI_Init((void*)hWnd);
}

void App::OnDestroy()
{
    //  destroy GUI subsystem
    ImGUI_Shutdown();

    //  deactivate full screen mode before exiting the app
    this->SetFullScreen(false);

    //  destroy renderer
    this->renderer->unloadScene();
    this->renderer->OnDestroyWindowSizeDependentResources();
    this->renderer->OnDestroy();
    delete this->renderer;

    //  destroy scene loader
    delete this->sceneLoader;

    //  destroy swap chain
    this->swapChain.OnDestroyWindowSizeDependentResources();
    this->swapChain.OnDestroy();

    //  destroy shader compiler
    DestroyShaderCache(&this->device);

    //  destroy device
    this->device.DestroyPipelineCache();
    this->device.OnDestroy();
}

void App::OnRender()
{
    //  retrieve timing info
    double timeNow = MillisecondsNow();
    this->deltaTime = timeNow - this->lastFrameTime;
    this->lastFrameTime = timeNow;

    //  initialize new GUI frame
    ImGUI_UpdateIO();
    ImGui::NewFrame();

    static int loadingStage = 0;
    static double cumulativeFrameTime = 0.;
    static float fps = 0.f;
    static bool isSceneLoaded = false;
    if (loadingStage == 0)
    {
        ImGui::GetStyle().FrameBorderSize = 1.0f;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(240, 300), ImGuiCond_Once);

        bool opened = true;
        ImGui::Begin("Info", &opened);

        //  load scene if it has not been loaded yet
        if (!isSceneLoaded)
        {
            //  upload mesh data to device
            loadingStage = this->renderer->loadScene(this->sceneLoader, 0);

            ImGui::End();
            ImGui::EndFrame();

            //  reset fps timer
            cumulativeFrameTime = 0.;
            fps = 0.f;

            isSceneLoaded = true;

            return;
        }

        //  sample frame rate
        cumulativeFrameTime += this->deltaTime;
        if (cumulativeFrameTime > 500.)
        {
            fps = (float)(1.0 / this->deltaTime);
            cumulativeFrameTime = 0.;
        }

        // run the animation
        this->renderer_state.deltaTime = this->deltaTime;

        if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Resolution\t: %i x %i", this->m_Width, this->m_Height);
            ImGui::Text("Frame rate\t: %.3f", fps);
        }

        if (ImGui::CollapsingHeader("Scene Config", ImGuiTreeNodeFlags_DefaultOpen))
        {
            //  sun orientation
            {
                float meter = 3.f * (this->sun_pitch - XM_PI / 2.f) / XM_PI;
                ImGui::SliderFloat("Sun Orientation", &meter, -1.f, 1.f);

                //  update light direction
                this->sun_pitch = XM_PI * (2.f * meter + 3.f) / 6.f;
                this->renderer_state.sunDir = PolarToVector(XM_PI / 2.f, this->sun_pitch);

                //  update light transformation matrix
                //  ToDo : do only for directional light settings
                /*int light_node_idx = this->sceneLoader->m_lightInstances[0].m_nodeIndex;
                this->sceneLoader->m_nodes[light_node_idx].m_tranform.LookAt(
                    this->renderer_state.sunDir * 20.5f, XMVectorSet(0, 0, 0, 0));
                this->sceneLoader->m_animatedMats[light_node_idx] =
                    this->sceneLoader->m_nodes[light_node_idx].m_tranform.GetWorldMat();*/
            }
        }

        if (ImGui::CollapsingHeader("Debug", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("D/I Contribution", &this->renderer_state.DIWeight, 0.f, 1.f);
        }

        if (ImGui::CollapsingHeader("Profiler", ImGuiTreeNodeFlags_DefaultOpen))
        {
            //  workaround: only target caustics
            const int causticsTimeIndex = 3;

            const std::vector<TimeStamp>& timeStamps = this->renderer->getTimeStamps();
            if (timeStamps.size() > 0)
            {
                this->accumProfTimes.resize(timeStamps.size());
                this->profTimes.resize(timeStamps.size());

                for (uint32_t i = 0; i < timeStamps.size(); i++)
                    this->accumProfTimes[i] += timeStamps[i].m_microseconds;

                this->accumInSec += this->deltaTime;
                this->accumCount += 1;

                if (this->accumInSec >= 1000.0)
                {
                    for (uint32_t i = 0; i < timeStamps.size(); i++)
                    {
                        this->profTimes[i] = this->accumProfTimes[i] / this->accumCount;
                        this->accumProfTimes[i] = 0;
                    }

                    this->accumInSec = 0;
                    this->accumCount = 0;
                }

                //ImGui::Text("%-22s: %7.1f", timeStamps[causticsTimeIndex].m_label.c_str(), this->profTime);
                for (uint32_t i = 0; i < timeStamps.size(); i++)
                {
                    ImGui::Text("%-22s: %7.1f", timeStamps[i].m_label.c_str(), this->profTimes[i]);
                }
            }
        }

        ImGui::End();

        //  manage key inputs
        ImGuiIO& io = ImGui::GetIO();
        {
            float newYaw = this->camera.GetYaw();
            float newPitch = this->camera.GetPitch();

            //If the mouse was not used by the GUI then it's for the camera
            //
            if (io.WantCaptureMouse)
            {
                io.MouseDelta.x = 0;
                io.MouseDelta.y = 0;
                io.MouseWheel = 0;
            }
            else if ((io.KeyCtrl == false) && (io.MouseDown[1] == true))
            {
                newYaw -= io.MouseDelta.x / 100.f;
                newPitch += io.MouseDelta.y / 100.f;
            }

            //  WASD move
            //  P.S. multiplying deltaTime(ms.) is no diff. from multiplying its internal speed
            this->camera.UpdateCameraWASD(newYaw, newPitch, io.KeysDown, /*io.DeltaTime*/ this->deltaTime * 2.0e-3);
        }
    }
    else
    {
        ImGui::OpenPopup("Loading");
        if (ImGui::BeginPopupModal("Loading", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            float progress = (float)loadingStage / 12.0f;
            ImGui::ProgressBar(progress, ImVec2(0.f, 0.f), NULL);
            ImGui::EndPopup();
        }

        //  keep uploading mesh data until loading stage returns to 0 again (means finished)
        loadingStage = this->renderer->loadScene(this->sceneLoader, loadingStage);
    }

    //  propagate initial transformation matrix for the enire scene heirarchy
    this->sceneLoader->TransformScene(0, XMMatrixIdentity());

    //  command renderer to do its thing
    this->renderer->OnRender(&this->swapChain, &this->camera, &this->renderer_state);

    //  upon completed, present rendered frame to the front buffer
    this->swapChain.Present();

    //  update previous MVP matrices
    this->camera.UpdatePreviousMatrices();
}

bool App::OnEvent(MSG msg)
{
    return ImGUI_WndProcHandler(msg.hwnd, msg.message, msg.wParam, msg.lParam) ? true : false;
}

void App::OnResize(uint32_t Width, uint32_t Height)
{
    //  flush gpu command queues first
    this->device.GPUFlush();

    //  destroy current window context first
    if (this->m_Width > 0 && this->m_Height > 0)
    {
        //  destroy window context bound to the renderer also
        //  this needs to be done BEFORE the swap chain.
        if (this->renderer != nullptr)
            this->renderer->OnDestroyWindowSizeDependentResources();

        this->swapChain.OnDestroyWindowSizeDependentResources();
    }

    //  change window extent
    this->m_Width = Width;
    this->m_Height = Height;

    //  initialize new window context
    if (this->m_Width > 0 && this->m_Height > 0)
    {
        this->swapChain.OnCreateWindowSizeDependentResources(this->m_Width, this->m_Height, false, this->currentDisplayMode);
        
        //  initialize window context for the renderer also
        //  this needs to be done AFTER the swap chain.
        if (this->renderer != nullptr)
            this->renderer->OnCreateWindowSizeDependentResources(&this->swapChain, this->m_Width, this->m_Height);
    }

    //  setup new camera settings
    this->camera.SetFov(XM_PI / 4, this->m_Width, this->m_Height, 0.1f, 1000.0f);
}

void App::SetFullScreen(bool fullscreen)
{
    //  flush gpu command queues first
    this->device.GPUFlush();

    //  force using SDR mode on window mode
    if ((fullscreen == false) && (this->currentDisplayMode != DISPLAYMODE_SDR))
        this->currentDisplayMode = DISPLAYMODE_SDR;

    //  set to full screen mode if requested
    this->swapChain.SetFullScreen(fullscreen);
}


//--------------------------------------------------------------------------------------
//
// WinMain
//
//--------------------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
{
    // create new instance
    return RunFramework(hInstance, lpCmdLine, nCmdShow, new App(APP_NAME));
}
