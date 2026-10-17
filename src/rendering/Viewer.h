#pragma once
#include <GLFW/glfw3.h>
#include <gl3w/gl3w.h>
#include <glm/glm.hpp>
#include <memory>
#include <array>
#include <string>

#include "Framebuffer.h"
#include "camera/Camera.h"
#include "scene/MIDIScene.h"
#include "ScreenQuad.h"

#include "../helpers/Recorder.h"

#include "State.h"
#include "Renderer.h"

#define DEBUG_SPEED (1.0f)

struct ImVec2;

struct SystemAction {
	enum Type {
		NONE, FIX_SIZE, FREE_SIZE, FULLSCREEN, QUIT, RESIZE
	};

	Type type;
	glm::ivec4 data;

	SystemAction(Type action);
};


class Viewer {

public:

	Viewer(const Configuration& config);

	~Viewer();
	
	bool loadFile(const std::string & midiFilePath);

	bool connectDevice(const std::string & deviceName);

	void setState(const State & state);
	
	/// Draw function
	SystemAction draw(const double currentTime);
	
	/// Clean function
	void clean();

	/// Handle screen resizing
	void resize(int width, int height);

	/// Handle window content density.
	void rescale(float scale);

	void resizeAndRescale(int width, int height, float scale);

	/// Handle keyboard inputs
	void keyPressed(int key, int action);

	/// Directly start recording.
	bool startDirectRecording(const Export& exporting, const glm::vec2 & size);

	void setGUIScale(float scale);

	float guiScale() const { return _guiScale; }

	/// The settings panel is being rescaled by dragging its corner.
	bool isResizingPanel() const { return _resizingPanel; }

	void updateConfiguration(Configuration& config);

private:
	

	struct Layer {
		
		enum Type : unsigned int {
			BGCOLOR = 0, BGTEXTURE, BLUR, ANNOTATIONS, KEYBOARD, PARTICLES, NOTES, FLASHES, PEDAL, WAVE, COUNT
		};

		Type type = BGCOLOR;
		std::string name = "None";
		void (Viewer::*draw)(const glm::vec2 &) = nullptr;
		bool * toggle = nullptr;

	};

	void blurPrepass();

	void drawBackgroundImage(const glm::vec2 & invSize);

	void drawBlur(const glm::vec2 & invSize);

	void drawParticles(const glm::vec2 & invSize);

	void drawScore(const glm::vec2 & invSize);

	void drawKeyboard(const glm::vec2 & invSize);

	void drawNotes(const glm::vec2 & invSize);

	void drawFlashes(const glm::vec2 & invSize);

	void drawPedals(const glm::vec2 & invSize);
	
	void drawWaves(const glm::vec2 & invSize);

	SystemAction drawGUI(const double currentTime);

	void drawScene(bool transparentBG);

	void showTopButtons(double currentTime);

	void showStatusLine();

	void showSessionTab();

	void showPlaybackTab();

	void showSceneTab();

	void showEffectsTab();

	SystemAction showDisplayTab();

	/// Begin a tab of the settings panel, marking it when active.
	bool beginTab(const char* label);

	/// Small title introducing a group of controls.
	void sectionTitle(const char* label);

	/// Draw a title in spaced capitals between two diamonds, centered in a row.
	void ornamentTitle(const char* label, const ImVec2& rowMin, const ImVec2& rowMax);

	/// Button filling one column of the two-columns grid.
	bool gridButton(const char* label);

	/// Corner grip of the settings panel, dragging it rescales the whole interface.
	void panelResizeGrip();

	/// Collapsible section for a layer, with a checkbox to show/hide the layer.
	bool layerSection(const char* label, bool& enabled);

	void showNoteOptions();

	void showParticleOptions();

	void showKeyboardOptions();

	void showFlashOptions();

	void showPedalOptions();
	
	void showWaveOptions();

	void showBlurOptions();

	void showScoreOptions();

	void showBackgroundOptions();

	void showBottomButtons();

	void showLayers();

	void showDevices();

	void showVisibility();

	void showSets();

	void showSetEditor();

	void showParticlesEditor();

	bool drawPedalImageSettings(GLuint tex, const glm::vec2& size, bool labelsAfter, bool flipUV, PathCollection& path, unsigned int index, glm::vec3& color);

	void refreshPedalTextures(State::PedalsState& pedals);

	void showPedalsEditor();

	void applyBackgroundColor();

	void applyAllSettings();
	
	void reset();

	/// Seek the file playback timeline by deltaSeconds (YouTube-style). File mode only.
	void seek(double deltaSeconds);

	void startRecording();

	void updateSizes();

	bool channelColorEdit(const char * name, const char * displayName, ColorArray & colors);
	
	void updateMinMaxKeys();

	void synchronizeColors(const ColorArray & colors);

	void ImGuiPushItemWidth(int w);

	void ImGuiSameLine(int w = 0);

	// Light vertical breathing room between GUI groups (a calm alternative to a
	// full-width separator, for a more minimalist rhythm).
	void guiSeparatorSpacing();

	State _state;
	std::array<Layer, Layer::COUNT> _layers;
	State _backupState;

	// Kept in double precision so that the timeline stays accurate even after
	// the app has been running for many hours (a float loses ~8ms of resolution
	// once the value reaches tens of thousands of seconds).
	double _timer = 0.0;
	double _timerStart = 0.0;
	bool _shouldPlay = false;
	bool _showGUI = true;
	bool _showDebug = false;
	bool _verbose = false;

	Renderer _renderer;
	Recorder _recorder;
	Camera _camera;
	
	std::shared_ptr<Framebuffer> _particlesFramebuffer;
	std::shared_ptr<Framebuffer> _blurFramebuffer0;
	std::shared_ptr<Framebuffer> _blurFramebuffer1;
	std::shared_ptr<Framebuffer> _renderFramebuffer;
	std::shared_ptr<Framebuffer> _finalFramebuffer;

	std::shared_ptr<MIDIScene> _scene;
	ScreenQuad _blurringScreen;
	ScreenQuad _passthrough;
	ScreenQuad _backgroundTexture;
	ScreenQuad _fxaa;

	glm::ivec2 _windowSize;
	glm::ivec2 _backbufferSize;
	float _guiScale = 1.0f;
	unsigned int _shouldQuit = 0;
	int _selectedPort = 0;
	bool _panelScrolls = false; ///< The settings panel had a scrollbar on the last frame.
	bool _resizingPanel = false; ///< The corner grip of the settings panel is being dragged.
	float _resizeGrabOffset = 0.0f; ///< Distance between the cursor and the panel edge when the drag started.
	float _pendingGuiScale = 0.0f; ///< Scale requested by the grip, applied at the beginning of the next frame.
	std::string _selectedTab = "Session"; ///< Tab of the settings panel selected on the last frame.
	bool _showLayers = false;
	bool _showSetListEditor = false;
	bool _showParticleEditor = false;
	bool _showPedalsEditor = false;
	bool _exitAfterRecording = false;
	bool _fullscreen = false;
	bool _liveplay = false;
	std::string _midiOutputName; ///< Remembered MIDI output port, auto-applied on file load.
	bool _useTransparency = false;
	const bool _supportTransparency;
};
