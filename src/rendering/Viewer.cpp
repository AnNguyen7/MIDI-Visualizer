#include "../helpers/ProgramUtilities.h"
#include "../helpers/ResourcesManager.h"
#include "../helpers/ImGuiStyle.h"
#include "../helpers/System.h"
#include "../resources/strings.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <iostream>
#include <sr_gui.h>
#include <stdio.h>
#include <vector>

#include "Viewer.h"
#include "scene/MIDIScene.h"
#include "scene/MIDISceneFile.h"
#include "scene/MIDISceneLive.h"

#include <algorithm>
#include <fstream>

SystemAction::SystemAction(SystemAction::Type act) {
	type = act;
	data = glm::ivec4(0);
}

Viewer::Viewer(const Configuration& config) :
	_supportTransparency(!config.preventTransparency) {

	_showGUI = true;
	_showDebug = false;

	_fullscreen = config.fullscreen;
	_windowSize = config.windowSize;
	_useTransparency = config.useTransparency && _supportTransparency;
	_midiOutputName = config.lastMidiOutput;

	// GL options
	glEnable(GL_CULL_FACE);
	glFrontFace(GL_CCW);
	glCullFace(GL_BACK);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
	glBlendEquation(GL_FUNC_ADD);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	_camera.screen(config.windowSize[0], config.windowSize[1], 1.0f);
	_backbufferSize = glm::vec2(config.windowSize);

	// Setup framebuffers, size does not really matter as we expect a resize event just after.
	const glm::ivec2 renderSize = _camera.renderSize();
	_particlesFramebuffer = std::shared_ptr<Framebuffer>(new Framebuffer(renderSize[0], renderSize[1],
		GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE));
	_blurFramebuffer0 = std::shared_ptr<Framebuffer>(new Framebuffer(renderSize[0], renderSize[1],
		GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE));
	_blurFramebuffer1 = std::shared_ptr<Framebuffer>(new Framebuffer(renderSize[0], renderSize[1],
		GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE));
	_renderFramebuffer = std::shared_ptr<Framebuffer>(new Framebuffer(renderSize[0], renderSize[1],
	GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE));
	_finalFramebuffer = std::shared_ptr<Framebuffer>(new Framebuffer(renderSize[0], renderSize[1],
		GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, GL_CLAMP_TO_EDGE));

	_backgroundTexture.init("backgroundtexture_frag", "backgroundtexture_vert");
	_blurringScreen.init(_particlesFramebuffer->textureId(), "particlesblur_frag");
	_fxaa.init("fxaa_frag");
	_passthrough.init("screenquad_frag");

	// Create the layers.
	//_layers[Layer::BGCOLOR].type = Layer::BGCOLOR;
	//_layers[Layer::BGCOLOR].name = "Background color";
	//_layers[Layer::BGCOLOR].toggle = &_state.showBackground;

	_layers[Layer::BGTEXTURE].type = Layer::BGTEXTURE;
	_layers[Layer::BGTEXTURE].name = "Background image";
	_layers[Layer::BGTEXTURE].draw = &Viewer::drawBackgroundImage;

	_layers[Layer::BLUR].type = Layer::BLUR;
	_layers[Layer::BLUR].name = "Blur effect";
	_layers[Layer::BLUR].draw = &Viewer::drawBlur;

	_layers[Layer::ANNOTATIONS].type = Layer::ANNOTATIONS;
	_layers[Layer::ANNOTATIONS].name = "Score";
	_layers[Layer::ANNOTATIONS].draw = &Viewer::drawScore;

	_layers[Layer::KEYBOARD].type = Layer::KEYBOARD;
	_layers[Layer::KEYBOARD].name = "Keyboard";
	_layers[Layer::KEYBOARD].draw = &Viewer::drawKeyboard;

	_layers[Layer::PARTICLES].type = Layer::PARTICLES;
	_layers[Layer::PARTICLES].name = "Particles";
	_layers[Layer::PARTICLES].draw = &Viewer::drawParticles;

	_layers[Layer::NOTES].type = Layer::NOTES;
	_layers[Layer::NOTES].name = "Notes";
	_layers[Layer::NOTES].draw = &Viewer::drawNotes;

	_layers[Layer::FLASHES].type = Layer::FLASHES;
	_layers[Layer::FLASHES].name = "Flashes";
	_layers[Layer::FLASHES].draw = &Viewer::drawFlashes;

	_layers[Layer::PEDAL].type = Layer::PEDAL;
	_layers[Layer::PEDAL].name = "Pedal";
	_layers[Layer::PEDAL].draw = &Viewer::drawPedals;

	_layers[Layer::WAVE].type = Layer::WAVE;
	_layers[Layer::WAVE].name = "Waves";
	_layers[Layer::WAVE].draw = &Viewer::drawWaves;

	// Register state.
	_layers[Layer::BGTEXTURE].toggle = &_state.background.image;
	_layers[Layer::BLUR].toggle = &_state.showBlur;
	_layers[Layer::ANNOTATIONS].toggle = &_state.showScore;
	_layers[Layer::KEYBOARD].toggle = &_state.showKeyboard;
	_layers[Layer::PARTICLES].toggle = &_state.showParticles;
	_layers[Layer::NOTES].toggle = &_state.showNotes;
	_layers[Layer::FLASHES].toggle = &_state.showFlashes;
	_layers[Layer::PEDAL].toggle = &_state.showPedal;
	_layers[Layer::WAVE].toggle = &_state.showWave;

	// Check setup errors.
	checkGLError();

	_renderer.renderSetup();
	_scene.reset(new MIDIScene());
}

Viewer::~Viewer() {}

bool Viewer::loadFile(const std::string& midiFilePath) {
	std::shared_ptr<MIDISceneFile> scene(nullptr);

	try {
		scene = std::make_shared<MIDISceneFile>(midiFilePath, _state.setOptions, _state.filter);
	} catch(...){
		// Failed to load.
		return false;
	}
	// Player.
	_timer = -_state.prerollTime;
	_shouldPlay = false;
	_liveplay = false;

	// Init objects.
	_scene = scene;
	// Apply the visual transpose (display only) and rebuild note geometry so a
	// persisted transpose shows immediately on load.
	scene->setTranspose(_state.transpose);
	scene->updateVisibleNotes(_state.filter);
	// Auto-route file playback to the remembered MIDI output (e.g. Pianoteq via
	// IAC Driver Bus 1) so the user doesn't have to re-pick it every session. If
	// the port is currently unavailable this just logs and leaves output closed;
	// we keep the remembered name so the next load retries.
	if(!_midiOutputName.empty()){
		scene->setMidiOutputByName(_midiOutputName);
	}
	applyAllSettings();
	return true;
}

bool Viewer::connectDevice(const std::string& deviceName) {
	std::shared_ptr<MIDIScene> scene(nullptr);
	_selectedPort = -1;
	
	const auto & devices = MIDISceneLive::availablePorts(true);
	for(int i = 0; i < devices.size(); ++i){
		if(devices[i] == deviceName){
			_selectedPort = i;
		}
	}

	if(_selectedPort == -1){
		if(deviceName != VIRTUAL_DEVICE_NAME){
			std::cerr << "[MIDI] Unable to connect to device named " << deviceName << "." << std::endl;
			return false;
		}
	}

	try {
		_scene = std::make_shared<MIDISceneLive>(_selectedPort, _verbose);
	} catch(const std::exception & e){
		// The device can vanish between the listing and the opening.
		std::cerr << "[MIDI] Unable to connect to device named " << deviceName << ": " << e.what() << std::endl;
		return false;
	}
	// Apply the visual transpose (display only) so a persisted value applies to
	// notes played from here on.
	_scene->setTranspose(_state.transpose);
	_timer = 0.0f;
	// Don't start immediately
	// _shouldPlay = true;
	_state.reverseScroll = true;
	_state.scrollSpeed = 1.0f;
	_liveplay = true;
	applyAllSettings();

	return true;
}

SystemAction Viewer::draw(double currentTime) {

	if(_recorder.isRecording()){
		_timer = _recorder.currentTime();

		drawScene(_recorder.isTransparent());

		_recorder.record(_finalFramebuffer);
		_recorder.drawProgress();

		// Determine which system action to take.
		SystemAction action = SystemAction::NONE;
		// Look at the frame ID.
		if(_recorder.currentFrame() < 2){
			action = SystemAction::FIX_SIZE;
		} else if(_recorder.currentFrame() >= _recorder.framesCount()){
			action = _exitAfterRecording ? SystemAction::QUIT : SystemAction::FREE_SIZE;
			_timer = 0.0f;
			_timerStart = 0.0f;
			_shouldPlay = false;
			resize(_backbufferSize[0], _backbufferSize[1]);
		}
		// Make sure the backbuffer is updated, this is nicer.
		glViewport(0, 0, GLsizei(_backbufferSize[0]), GLsizei(_backbufferSize[1]));
		_passthrough.draw(_finalFramebuffer->textureId(), _timer);
		return action;
	}

	// -- Default mode --

	// Compute the time elapsed since last frame, or keep the same value if
	// playback is disabled.
	_timer = _shouldPlay ? (currentTime - _timerStart) : _timer;

	// Render scene and blit, with GUI on top if needed.
	drawScene(_useTransparency);

	glViewport(0, 0, GLsizei(_backbufferSize[0]), GLsizei(_backbufferSize[1]));
	_passthrough.draw(_finalFramebuffer->textureId(), _timer);

	SystemAction action = SystemAction::NONE;
	if(_showGUI){
		action = drawGUI(currentTime);
	}

	return action;
}

void Viewer::drawScene(bool transparentBG){

	// Only drive the external synth during real-time playback (not when paused or exporting).
	if(auto fs = std::dynamic_pointer_cast<MIDISceneFile>(_scene)) fs->setOutputActive(_shouldPlay && !_recorder.isRecording());
	// The particles of a note last for the lifetime, from the note until they have vanished.
	_scene->setParticlesDuration(_state.particles.lifetime);
	// Update active notes listing.
	_scene->updatesActiveNotes(_state.scrollSpeed * _timer, _state.scrollSpeed, _state.filter);
	// Let renderer update GPU data if needed.
	_renderer.upload(_scene);

	const glm::vec2 invSizeFb = 1.0f / glm::vec2(_renderFramebuffer->_width, _renderFramebuffer->_height);

	// Blur rendering.
	if (_state.showBlur) {
		blurPrepass();
	}

	// Set viewport
	_renderFramebuffer->bind();
	glViewport(0, 0, _renderFramebuffer->_width, _renderFramebuffer->_height);

	// Final pass (directly on screen).
	// Background color.
	if(transparentBG){
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	} else {
		glClearColor(_state.background.color[0], _state.background.color[1], _state.background.color[2], 1.0f);
	}

	glClear(GL_COLOR_BUFFER_BIT);

	// Draw the layers in order.
	for (int i = 0; i < _state.layersMap.size(); ++i) {
		const int layerId = _state.layersMap[i];
		if (layerId >= _layers.size()) {
			continue;
		}
		if (_layers[layerId].draw && *(_layers[layerId].toggle)) {
			(this->*_layers[layerId].draw)(invSizeFb);
		}
	}

	_renderFramebuffer->unbind();

	// Apply fxaa.
	if(_state.applyAA){
		_finalFramebuffer->bind();
		_fxaa.draw(_renderFramebuffer->textureId(), 0.0, invSizeFb);
		_finalFramebuffer->unbind();
	} else {
		// Else just do a blit.
		_renderFramebuffer->bind(GL_READ_FRAMEBUFFER);
		_finalFramebuffer->bind(GL_DRAW_FRAMEBUFFER);
		glBlitFramebuffer(0, 0, _renderFramebuffer->_width, _renderFramebuffer->_height, 0, 0, _finalFramebuffer->_width, _finalFramebuffer->_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		_finalFramebuffer->unbind();
	}

}

void Viewer::blurPrepass() {
	const glm::vec2 invSizeB = 1.0f / glm::vec2(_particlesFramebuffer->_width, _particlesFramebuffer->_height);
	// Bind particles buffer.
	_particlesFramebuffer->bind();
	// Set viewport.
	glViewport(0, 0, _particlesFramebuffer->_width, _particlesFramebuffer->_height);
	// Draw blurred particles from previous frames.
	_passthrough.draw(_blurFramebuffer1->textureId(), _timer);
	if (_state.showParticles) {
		// Draw the new particles.
		_renderer.drawParticles(_scene, _timer, invSizeB, _state.particles, Quality::availables.at(_state.quality).particlesResolution, true);
	}
	if (_state.showBlurNotes) {
		// Draw the notes.
		_renderer.drawNotes(_scene, _timer * _state.scrollSpeed, invSizeB, _state.notes, _state.reverseScroll, true);
	}

	_particlesFramebuffer->unbind();

	// Perform blur on result from particles pass.
	// We re-use the 'time' uniform to denote if this is the vertical or horizontal pass.
	// Horizontal pass
	const glm::vec2 invBlurSize0 = 1.0f / glm::vec2(_particlesFramebuffer->_width, _particlesFramebuffer->_height);
	glViewport(0, 0, _blurFramebuffer0->_width, _blurFramebuffer0->_height);
	_blurFramebuffer0->bind();
	_blurringScreen.draw(_particlesFramebuffer->textureId(), 0.0f, invBlurSize0);
	_blurFramebuffer0->unbind();
	// Vertical pass
	const glm::vec2 invBlurSize1 = 1.0f / glm::vec2(_blurFramebuffer0->_width, _blurFramebuffer0->_height);
	glViewport(0, 0, _blurFramebuffer1->_width, _blurFramebuffer1->_height);
	_blurFramebuffer1->bind();
	_blurringScreen.draw(_blurFramebuffer0->textureId(), 1.0f, invBlurSize1);
	_blurFramebuffer1->unbind();

}

void Viewer::drawBackgroundImage(const glm::vec2 &) {
	// Use background.tex and background.imageAlpha
	// Early exit if no texture or transparent.
	if(_state.background.tex == 0 || _state.background.imageAlpha < 1.0f/255.0f) {
		return;
	}
	glEnable(GL_BLEND);
	ShaderProgram& bgProg = _backgroundTexture.program();
	bgProg.use();
	bgProg.uniform("textureAlpha", _state.background.imageAlpha);
	bgProg.uniform("behindKeyboard", _state.background.imageBehindKeyboard);
	bgProg.uniform("keyboardHeight", _state.keyboard.size);
	bgProg.uniform("scroll", float(_timer) * _state.background.scrollSpeed);
	_backgroundTexture.draw(_state.background.tex, _timer);

	glDisable(GL_BLEND);
}

void Viewer::drawBlur(const glm::vec2 &) {
	glEnable(GL_BLEND);
	_passthrough.draw(_blurFramebuffer1->textureId(), _timer);
	glDisable(GL_BLEND);
}

void Viewer::drawParticles(const glm::vec2 & invSize) {
	_renderer.drawParticles(_scene, _timer, invSize, _state.particles, Quality::availables.at(_state.quality).finalResolution, false);
}

void Viewer::drawScore(const glm::vec2 & invSize) {
	glEnable(GL_BLEND);
	// The score is resolution dependent, because of the text and line settings specified in pixels.
	const auto& currentQuality = Quality::availables.at(_state.quality);
	_renderer.drawScore(_scene, _timer * _state.scrollSpeed, invSize, _state.score, _state.scale, currentQuality.finalResolution, _state.keyboard.size, _state.horizontalScroll, _state.reverseScroll );
	glDisable(GL_BLEND);
}

void Viewer::drawKeyboard(const glm::vec2 & invSize) {
	const ColorArray & majColors = _state.keyboard.customKeyColors ? _state.keyboard.majorColor : _state.notes.majorColors;
	const ColorArray & minColors = _state.keyboard.customKeyColors ? _state.keyboard.minorColor : _state.notes.minorColors;
	_renderer.drawKeyboard(_scene, _timer, invSize, _state.keyboard.edgeColor, _state.keyboard.backColor, majColors, minColors, _state.keyboard.highlightKeys);
}

void Viewer::drawNotes(const glm::vec2 & invSize) {
	glEnable(GL_BLEND);
	_renderer.drawNotes(_scene, _timer * _state.scrollSpeed, invSize, _state.notes, _state.reverseScroll, false);
	glDisable(GL_BLEND);
}

void Viewer::drawFlashes(const glm::vec2 & invSize) {
	_renderer.drawFlashes(_scene, _timer, invSize, _state.flashes);
}

void Viewer::drawPedals(const glm::vec2 & invSize){
	// Extra shift above the waves.
	_renderer.drawPedals(_scene, _timer, invSize, _state.pedals, _state.keyboard.size + (_state.showWave ? 0.01f : 0.0f), _state.horizontalScroll);
}

void Viewer::drawWaves(const glm::vec2 & invSize){
	_renderer.drawWaves(_scene, _timer, invSize, _state.waves, _state.keyboard.size);
}

SystemAction Viewer::drawGUI(const double currentTime) {

	SystemAction action = SystemAction::NONE;

	// Apply the scale requested by the corner grip before laying anything out.
	if(_pendingGuiScale > 0.0f){
		setGUIScale(_pendingGuiScale);
		_pendingGuiScale = 0.0f;
	}

	// Fixed width so that the panel does not jump between tabs, and capped to the
	// window height (it scrolls beyond that) so that it never runs off screen.
	const ImVec2 & displaySize = ImGui::GetIO().DisplaySize;
	const float panelMargin = 10.0f;
	// Leave room for the scrollbar when there is one, the grid would be clipped otherwise.
	const float scrollbarWidth = _panelScrolls ? ImGui::GetStyle().ScrollbarSize : 0.0f;
	const float panelWidth = (std::min)(_guiScale * float(PANEL_WIDTH) + scrollbarWidth, displaySize.x);
	const float panelMaxHeight = (std::max)(displaySize.y - 2.0f * panelMargin, 100.0f);
	ImGui::SetNextWindowPos(ImVec2(panelMargin, panelMargin), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(panelWidth, 0.0f), ImVec2(panelWidth, panelMaxHeight));

	if (ImGui::Begin("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
		ImGui::paperBackground();
		_panelScrolls = ImGui::GetScrollMaxY() > 0.0f;

		showTopButtons(currentTime);
		showStatusLine();
		ImGui::Spacing();

		if(ImGui::BeginTabBar("##SettingsTabs")){
			if(beginTab("Session")){
				showSessionTab();
				ImGui::EndTabItem();
			}
			if(beginTab("Playback")){
				showPlaybackTab();
				ImGui::EndTabItem();
			}
			if(beginTab("Scene")){
				showSceneTab();
				ImGui::EndTabItem();
			}
			if(beginTab("Effects")){
				showEffectsTab();
				ImGui::EndTabItem();
			}
			if(beginTab("Display")){
				const SystemAction displayAction = showDisplayTab();
				if(displayAction.type != SystemAction::NONE){
					action = displayAction;
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		if (_showDebug) {
			ImGui::Separator();
			ImGui::Text("Debug: ");
			ImGuiSameLine();
			ImGui::TextDisabled("(press D to hide)");
			ImGui::Text("%.1f FPS / %.1f ms", ImGui::GetIO().Framerate, ImGui::GetIO().DeltaTime * 1000.0f);
			ImGui::Text("Render size: %dx%d, screen size: %dx%d", _renderFramebuffer->_width, _renderFramebuffer->_height, _camera.screenSize()[0], _camera.screenSize()[1]);
			if (ImGui::Button("Print MIDI content to console")) {
				_scene->print();
			}
			ImGui::Checkbox("Verbose log", &_verbose);
			if(ImGui::Button("Assign random colors")){
				_state.lockParticleColor = true;

				const ColorArray debugColors = {
					glm::vec3{1.0f, 0.0f, 0.0f},
					glm::vec3{1.0f, 0.5f, 0.0f},
					glm::vec3{1.0f, 1.0f, 0.0f},
					glm::vec3{0.5f, 1.0f, 0.0f},
					glm::vec3{0.0f, 1.0f, 0.0f},
					glm::vec3{0.0f, 1.0f, 0.5f},
					glm::vec3{0.0f, 1.0f, 1.0f},
					glm::vec3{0.0f, 0.5f, 1.0f},
					glm::vec3{0.0f, 0.0f, 1.0f},
					glm::vec3{0.5f, 0.0f, 1.0f},
					glm::vec3{1.0f, 0.0f, 1.0f},
					glm::vec3{1.0f, 0.0f, 0.5f}
				};
				synchronizeColors(debugColors);
			}
		}

		panelResizeGrip();
	}
	ImGui::End();

	if(_showLayers){
		showLayers();
	}

	if(_showSetListEditor){
		showSetEditor();
	}

	if(_showParticleEditor){
		showParticlesEditor();
	}

	if(_showPedalsEditor){
		showPedalsEditor();
	}

	if(_shouldQuit != 0){
		// We should only open the popup once.
		if(_shouldQuit == 1){
			ImGui::OpenPopup("Quit");
			_shouldQuit = 2;
		}

		if(ImGui::BeginPopupModal("Quit", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)){
			ImGui::paperBackground();
			ImGui::Text("Are you sure you want to quit?");
			const ImVec2 buttonSize(_guiScale * (COLUMN_SIZE - 20.0f), 0.0f);
			if(ImGui::Button("No", buttonSize)){
				_shouldQuit = 0;
			}
			ImGuiSameLine(COLUMN_SIZE);
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::dangerColor());
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::dangerColor());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::onDarkTextColor());
			if(ImGui::Button("Yes", buttonSize)){
				ImGui::PopStyleColor(3);
				ImGui::EndPopup();
				return SystemAction::QUIT;
			}
			ImGui::PopStyleColor(3);
			ImGui::EndPopup();
		}
	}
	return action;
}

void Viewer::synchronizeColors(const ColorArray & colors){
	// Keep the colors in sync if needed.
	if (!_state.lockParticleColor) {
		return;
	}

	for(size_t cid = 0; cid < SETS_COUNT; ++cid){
		_state.notes.majorColors[cid] = _state.particles.colors[cid] = _state.notes.minorColors[cid] = _state.flashes.colors[cid] = colors[cid];
	}

	// If we have only one channel, synchronize one-shot effects.
	// Disable this because it's not symmetric.
	//if(!_state.perChannelColors){
	//	_state.pedals.color = _state.waves.color = _state.notes.majorColors[0];
	//}
}

bool Viewer::beginTab(const char* label){
	// The selected tab is a dark block, its label needs a light color.
	const bool wasSelected = _selectedTab == label;
	if(wasSelected){
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::onDarkTextColor());
		ImGui::PushStyleColor(ImGuiCol_TabHovered, ImGui::darkHoverColor());
	}
	const bool selected = ImGui::BeginTabItem(label);
	if(wasSelected){
		ImGui::PopStyleColor(2);
	}
	if(!selected){
		return false;
	}
	_selectedTab = label;
	ImGui::Spacing();
	return true;
}

void Viewer::ornamentTitle(const char* label, const ImVec2& rowMin, const ImVec2& rowMax){
	// Engraved title: spaced capitals between two diamonds, with thin rules out to the edges of the row.
	ImFont* font = ImGui::smallFont();
	const float fontSize = font->FontSize * ImGui::GetIO().FontGlobalScale;
	const float tracking = 0.16f * fontSize;
	const float diamond = 0.24f * fontSize;
	const float gap = 0.55f * fontSize;

	// The label stops at the ID separator.
	std::string title;
	for(const char* c = label; *c != '\0' && !(c[0] == '#' && c[1] == '#'); ++c){
		title.push_back(char(std::toupper((unsigned char)(*c))));
	}

	float titleWidth = 0.0f;
	for(size_t cid = 0; cid < title.size(); ++cid){
		titleWidth += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, &title[cid], &title[cid] + 1).x;
	}
	titleWidth += tracking * float((std::max)(int(title.size()) - 1, 0));

	const ImU32 inkColor = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 ruleColor = ImGui::GetColorU32(ImGuiCol_Separator);
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	const float centerY = std::floor(0.5f * (rowMin.y + rowMax.y)) + 0.5f;
	const float titleMin = std::floor(0.5f * (rowMin.x + rowMax.x) - 0.5f * titleWidth);
	const float titleMax = titleMin + titleWidth;

	ImVec2 cursor(titleMin, centerY - 0.5f * fontSize);
	for(size_t cid = 0; cid < title.size(); ++cid){
		drawList->AddText(font, fontSize, cursor, inkColor, &title[cid], &title[cid] + 1);
		cursor.x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, &title[cid], &title[cid] + 1).x + tracking;
	}

	const float diamondLeft = titleMin - gap - diamond;
	const float diamondRight = titleMax + gap + diamond;
	for(const float x : {diamondLeft, diamondRight}){
		drawList->AddQuadFilled(ImVec2(x - diamond, centerY), ImVec2(x, centerY - diamond), ImVec2(x + diamond, centerY), ImVec2(x, centerY + diamond), inkColor);
	}

	const float ruleLeftEnd = diamondLeft - diamond - gap;
	const float ruleRightStart = diamondRight + diamond + gap;
	if(ruleLeftEnd > rowMin.x){
		drawList->AddLine(ImVec2(rowMin.x, centerY), ImVec2(ruleLeftEnd, centerY), ruleColor, 1.0f);
	}
	if(ruleRightStart < rowMax.x){
		drawList->AddLine(ImVec2(ruleRightStart, centerY), ImVec2(rowMax.x, centerY), ruleColor, 1.0f);
	}
}

void Viewer::panelResizeGrip(){
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const ImGuiIO & io = ImGui::GetIO();
	const ImGuiStyle & style = ImGui::GetStyle();

	// Bottom right corner, next to the scrollbar when there is one.
	const float scrollbarWidth = _panelScrolls ? style.ScrollbarSize : 0.0f;
	const ImVec2 winMin = window->Pos;
	const ImVec2 winMax(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
	const float gripSize = (std::max)(18.0f * _guiScale, 14.0f);
	const ImVec2 gripMax(winMax.x - scrollbarWidth, winMax.y);
	const ImRect gripRect(ImVec2(gripMax.x - gripSize, gripMax.y - gripSize), gripMax);
	const ImGuiID gripId = window->GetID("##panelResize");

	// The grip sits in the padding of the window: register it without going through the layout
	// (it would grow the panel), and lift the content clipping so that all of it reacts.
	ImGui::PushClipRect(winMin, winMax, false);
	bool hovered = false;
	bool held = false;
	if(ImGui::ItemAdd(gripRect, gripId)){
		ImGui::ButtonBehavior(gripRect, gripId, &hovered, &held);
	}

	if(held){
		if(!_resizingPanel){
			_resizingPanel = true;
			_resizeGrabOffset = winMax.x - io.MousePos.x;
		}
		// The panel width is PANEL_WIDTH (plus the scrollbar) times the scale: find the scale
		// putting the right edge back under the cursor, the top left corner does not move.
		const float baseWidth = float(PANEL_WIDTH) + scrollbarWidth / _guiScale;
		const float wantedWidth = io.MousePos.x + _resizeGrabOffset - winMin.x;
		const float maxScale = (std::max)(0.5f, (std::min)(3.0f, io.DisplaySize.x / baseWidth));
		const float scale = std::round(glm::clamp(wantedWidth / baseWidth, 0.5f, maxScale) * 100.0f) / 100.0f;
		if(std::abs(scale - _guiScale) > 0.004f){
			_pendingGuiScale = scale;
		}
	} else {
		_resizingPanel = false;
	}

	if(hovered || held){
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
		if(!held){
			ImGui::SetTooltip("Drag to resize the panel");
		}
	}

	// Three diagonal lines, inside the rim.
	ImVec4 gripColor = style.Colors[ImGuiCol_Text];
	gripColor.w = (hovered || held) ? 0.9f : 0.45f;
	const ImU32 gripColorU32 = ImGui::GetColorU32(gripColor);
	const float unit = _guiScale;
	const float inset = 7.5f * unit;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	for(int lid = 1; lid <= 3; ++lid){
		const float reach = (1.5f + 3.5f * float(lid)) * unit;
		drawList->AddLine(ImVec2(gripMax.x - inset - reach, gripMax.y - inset), ImVec2(gripMax.x - inset, gripMax.y - inset - reach), gripColorU32, (std::max)(1.0f, 1.2f * unit));
	}
	ImGui::PopClipRect();
}

void Viewer::sectionTitle(const char* label){
	ImGui::Spacing();
	const ImVec2 rowMin = ImGui::GetCursorScreenPos();
	const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight());
	ImGui::Dummy(rowSize);
	ornamentTitle(label, rowMin, ImVec2(rowMin.x + rowSize.x, rowMin.y + rowSize.y));
}

bool Viewer::gridButton(const char* label){
	// Buttons filling one column of the grid, so that rows of actions line up.
	return ImGui::Button(label, ImVec2(_guiScale * float(COLUMN_SIZE - 24), 0.0f));
}

bool Viewer::layerSection(const char* label, bool& enabled){
	ImGui::PushID(label);
	ImGui::Checkbox("##enabled", &enabled);
	ImGui::helpTooltip("Show or hide this layer");
	ImGui::SameLine();

	// The rest of the row is the title, clicking it folds or unfolds the section.
	ImGuiStorage* storage = ImGui::GetStateStorage();
	const ImGuiID openId = ImGui::GetID("##open");
	bool open = storage->GetBool(openId, false);

	const ImVec2 rowMin = ImGui::GetCursorScreenPos();
	const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
	const ImVec2 rowMax(rowMin.x + rowSize.x, rowMin.y + rowSize.y);
	if(ImGui::InvisibleButton("##title", rowSize)){
		open = !open;
		storage->SetBool(openId, open);
	}
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	if(ImGui::IsItemHovered()){
		drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
	}
	// Fold mark on the left.
	const float arrow = 0.28f * ImGui::GetFontSize();
	const ImVec2 arrowCenter(std::floor(rowMin.x + 2.0f * arrow) + 0.5f, std::floor(0.5f * (rowMin.y + rowMax.y)) + 0.5f);
	const ImU32 inkColor = ImGui::GetColorU32(ImGuiCol_Text);
	if(open){
		drawList->AddTriangleFilled(ImVec2(arrowCenter.x - arrow, arrowCenter.y - 0.7f * arrow), ImVec2(arrowCenter.x + arrow, arrowCenter.y - 0.7f * arrow), ImVec2(arrowCenter.x, arrowCenter.y + 0.9f * arrow), inkColor);
	} else {
		drawList->AddTriangleFilled(ImVec2(arrowCenter.x - 0.7f * arrow, arrowCenter.y - arrow), ImVec2(arrowCenter.x + 0.9f * arrow, arrowCenter.y), ImVec2(arrowCenter.x - 0.7f * arrow, arrowCenter.y + arrow), inkColor);
	}
	ornamentTitle(label, ImVec2(rowMin.x + 5.0f * arrow, rowMin.y), ImVec2(rowMax.x - 2.0f * arrow, rowMax.y));

	ImGui::PopID();
	return open;
}

void Viewer::showStatusLine(){
	const double time = _timer * _state.scrollSpeed;
	const int nCount = _scene->notesCount();

	ImGui::PushFont(ImGui::smallFont());
	if(auto liveScene = std::dynamic_pointer_cast<MIDISceneLive>(_scene)){
		ImGui::TextDisabled("Live on %s, %.1fs, %d notes", liveScene->deviceName().c_str(), time, nCount);
	} else if(std::dynamic_pointer_cast<MIDISceneFile>(_scene)){
		const double duration = _scene->duration();
		const int speed = int(std::round(double(nCount)/(std::max)(0.001, duration)));
		ImGui::TextDisabled("%.1fs of %.1fs, %d notes, %d per second", time, duration, nCount, speed);
	} else {
		ImGui::TextDisabled("Load a MIDI file or connect a device to start.");
	}
	ImGui::PopFont();
}

void Viewer::showSessionTab(){
	if(gridButton("Load MIDI file...")) {
		// Read arguments.
		char* outPath = nullptr;
		int res = sr_gui_ask_load_file("Load MIDI file", "", "mid", &outPath);
		System::forceLocale();
		if(res == SR_GUI_VALIDATED && outPath){
			loadFile(std::string(outPath));
		}
		free(outPath);
	}
	ImGuiSameLine(COLUMN_SIZE);
	if(_liveplay){
		if (gridButton("End session")) {
			_scene = std::make_shared<MIDIScene>();
			_liveplay = false;
			_shouldPlay = false;
			_timer = 0.0;
			applyAllSettings();
		}
		ImGui::helpTooltip("Disconnect from the device and clear the recorded notes");
	} else {
		if (gridButton("Connect to device...")) {
			ImGui::OpenPopup("Devices");
		}
		showDevices();
	}

	const bool existingScene = (std::dynamic_pointer_cast<MIDISceneFile>(_scene) != nullptr)
							|| (std::dynamic_pointer_cast<MIDISceneLive>(_scene) != nullptr);
	if(existingScene){
		if(gridButton("Export MIDI file...")) {
			char* savePath = nullptr;
			int res = sr_gui_ask_save_file("Save MIDI file", "", "mid", &savePath);
			System::forceLocale();
			if(res == SR_GUI_VALIDATED && savePath){
				std::ofstream outFile = System::openOutputFile(std::string(savePath), true);
				_scene->save(outFile);
				outFile.close();
			}
			free(savePath);
		}
		ImGui::helpTooltip("Export a new MIDI file of the current session");
	}
	if(_liveplay){
		if(existingScene){
			ImGuiSameLine(COLUMN_SIZE);
		}
		if (gridButton("Reset stuck notes")) {
			if(auto ls = std::dynamic_pointer_cast<MIDISceneLive>(_scene)) ls->resetStuckNotes();
		}
		ImGui::helpTooltip("Force-close any note stuck on screen (a note whose\nnote-off was lost), without reconnecting the device.\nShortcut: K");
	}

	// MIDI output: send playback events to a virtual port so an external
	// sound engine (e.g. Pianoteq via the macOS IAC Driver) can play in sync.
	if(auto fileScene = std::dynamic_pointer_cast<MIDISceneFile>(_scene)){
		sectionTitle("MIDI output");
		const std::string currentName = fileScene->midiOutputPortName();
		const char * previewLabel = currentName.empty() ? "Disabled" : currentName.c_str();
		const ImGuiStyle & style = ImGui::GetStyle();
		const float refreshWidth = ImGui::CalcTextSize("Refresh").x + 2.0f * style.FramePadding.x;
		ImGui::PushItemWidth(-(refreshWidth + style.ItemSpacing.x));
		if(ImGui::BeginCombo("##midi_out_combo", previewLabel)){
			// Use cached list - the Refresh button forces a re-scan.
			const auto & ports = MIDISceneFile::availableOutputPorts(false);
			if(ImGui::Selectable("Disabled", currentName.empty())){
				fileScene->setMidiOutputByName("");
				_midiOutputName.clear();
			}
			for(const auto & name : ports){
				const bool sel = (name == currentName);
				if(ImGui::Selectable(name.c_str(), sel)){
					fileScene->setMidiOutputByName(name);
					_midiOutputName = name;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		ImGuiSameLine();
		if(ImGui::Button("Refresh##midi_out")){
			MIDISceneFile::availableOutputPorts(true);
		}
		ImGui::helpTooltip("Send note on/off events to an external app. On macOS, enable IAC Driver in Audio MIDI Setup, then select it here.");
	}

	sectionTitle("Settings and video");
	showBottomButtons();
}

void Viewer::showPlaybackTab(){
	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::Combo("Min key", &_state.minKey, midiKeysStrings, 128)){
		updateMinMaxKeys();
	}
	ImGui::helpTooltip(s_min_key_dsc);

	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::Combo("Max key", &_state.maxKey, midiKeysStrings, 128)){
		updateMinMaxKeys();
	}
	ImGui::helpTooltip(s_max_key_dsc);

	if (ImGui::InputFloat("Preroll", &_state.prerollTime, 0.1f, 1.0f, "%.1fs")) {
		reset();
	}
	ImGui::helpTooltip(s_preroll_dsc);

	if(_liveplay){
		ImGui::BeginDisabled();
	}
	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::InputFloat("Speed", &_state.scrollSpeed, 0.1f, 0.5f, "%.1fx")){
		_state.scrollSpeed = (std::max)(0.1f, (std::min)(5.0f, _state.scrollSpeed));
	}
	if(_liveplay){
		ImGui::EndDisabled();
		if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
			ImGui::SetTooltip("Not available in liveplay");
		}
	} else {
		ImGui::helpTooltip(s_scroll_speed_dsc);
	}

	// Visual transpose in semitones (display only). Set this to the same
	// value as Pianoteq's transpose so the on-screen notes match the sound.
	if(ImGui::InputInt("Transpose", &_state.transpose)){
		_state.transpose = (std::max)(-24, (std::min)(24, _state.transpose));
		_scene->setTranspose(_state.transpose);
		_scene->updateVisibleNotes(_state.filter);
	}
	ImGui::helpTooltip(s_transpose_dsc);
	ImGui::PopItemWidth();

	if(_liveplay){
		ImGui::BeginDisabled();
	}
	ImGuiSameLine(COLUMN_SIZE);
	if (gridButton("Tracks and channels...")) {
		ImGui::OpenPopup("Visibility options");
	}
	ImGui::helpTooltip("Define which tracks and channels are visible");
	showVisibility();
	if(_liveplay){
		ImGui::EndDisabled();
		if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
			ImGui::SetTooltip("Not available in liveplay");
		}
	}

	if(ImGui::Checkbox("Horizontal scroll", &_state.horizontalScroll)){
		_renderer.setOrientation(_state.horizontalScroll);
	}
	ImGui::helpTooltip(s_scroll_horizontal_dsc);

	if(_liveplay){
		ImGui::BeginDisabled();
	}
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::Checkbox("Reverse scroll", &_state.reverseScroll);
	if(_liveplay){
		ImGui::EndDisabled();
		if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
			ImGui::SetTooltip("Not available in liveplay");
		}
	} else {
		ImGui::helpTooltip(s_scroll_reverse_dsc);
	}
}

void Viewer::showSceneTab(){
	if(ImGui::Checkbox("Per-set colors", &_state.perSetColors)){
		if(!_state.perSetColors){
			_state.synchronizeSets();
		}
	}
	ImGui::helpTooltip(s_colors_per_set_dsc);

	if(_state.perSetColors){
		ImGuiSameLine(COLUMN_SIZE);
		if(gridButton("Define color sets...")){
			ImGui::OpenPopup("Note sets options");
		}
		ImGui::helpTooltip("Define how notes should be assigned to color sets");
		showSets();
	}

	if (ImGui::Checkbox("Same colors for all effects", &_state.lockParticleColor)) {
		// If we enable the lock, make sure the colors are synched.
		synchronizeColors(_state.notes.majorColors);
	}
	ImGui::helpTooltip(s_lock_colors_dsc);

	ImGui::Spacing();

	if(layerSection("Notes##HEADER", _state.showNotes)){
		ImGui::BeginDisabled(!_state.showNotes);
		showNoteOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Keyboard##HEADER", _state.showKeyboard)){
		ImGui::BeginDisabled(!_state.showKeyboard);
		showKeyboardOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Background image##HEADER", _state.background.image)){
		showBackgroundOptions();
	}
}

void Viewer::showEffectsTab(){
	if(layerSection("Flashes##HEADER", _state.showFlashes)){
		ImGui::BeginDisabled(!_state.showFlashes);
		showFlashOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Particles##HEADER", _state.showParticles)){
		ImGui::BeginDisabled(!_state.showParticles);
		showParticleOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Pedal##HEADER", _state.showPedal)){
		ImGui::BeginDisabled(!_state.showPedal);
		showPedalOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Wave##HEADER", _state.showWave)){
		ImGui::BeginDisabled(!_state.showWave);
		showWaveOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Score##HEADER", _state.showScore)){
		ImGui::BeginDisabled(!_state.showScore);
		showScoreOptions();
		ImGui::EndDisabled();
	}
	if(layerSection("Blur##HEADER", _state.showBlur)){
		ImGui::BeginDisabled(!_state.showBlur);
		showBlurOptions();
		ImGui::EndDisabled();
	}

	ImGui::Spacing();
	if (gridButton("Reorder layers...")) {
		_showLayers = true;
	}
	ImGui::helpTooltip("Define which effects are visible and their ordering");
}

SystemAction Viewer::showDisplayTab(){
	SystemAction action = SystemAction::NONE;

	ImGuiPushItemWidth(ITEM_WIDTH);
	if (ImGui::Combo("Quality", (int *)(&_state.quality), "Half\0Low\0Medium\0High\0Double\0\0")) {
		updateSizes();
	}
	ImGui::helpTooltip(s_quality_dsc);
	ImGui::PopItemWidth();

	// Add FXAA.
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::Checkbox("Smoothing", &_state.applyAA);
	ImGui::helpTooltip(s_smooth_dsc);

	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::InputFloat("GUI size", &_guiScale, 0.25f, 1.0f, "%.2fx")){
		_guiScale = glm::clamp(_guiScale, 0.25f, 4.0f);
		setGUIScale(_guiScale);
	}
	ImGui::helpTooltip("Scale of the interface texts and buttons on screen");
	ImGui::PopItemWidth();
	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::Button("Reset GUI size")){
		setGUIScale(1.0f);
	}
	ImGui::helpTooltip("Reset the scale of the interface to 1x");

	if(ImGui::Checkbox("Fullscreen", &_fullscreen)){
		action = SystemAction::FULLSCREEN;
	}
	ImGui::helpTooltip("Toggle fullscreen window");

	if(_supportTransparency) {
		ImGuiSameLine(COLUMN_SIZE);
		if(ImGui::Checkbox("Transparent", &_useTransparency)){
			_useTransparency = _useTransparency && _supportTransparency;
		}
		ImGui::helpTooltip("Toggle background window transparency");
	}

	if(!_fullscreen){
		ImGuiPushItemWidth(ITEM_WIDTH);
		ImGui::InputInt2("Window size", &_windowSize[0]);
		ImGui::PopItemWidth();
		ImGuiSameLine(COLUMN_SIZE);
		if(ImGui::Button("Resize window")){
			action = SystemAction::RESIZE;
			action.data[0] = _windowSize[0];
			action.data[1] = _windowSize[1];
		}
		ImGui::helpTooltip("Apply the specified size to the window");
	}

	return action;
}

void Viewer::showTopButtons(double currentTime){
	const ImVec2 transportSize(_guiScale * 86.0f, 0.0f);

	if (ImGui::Button(_shouldPlay ? "Pause" : "Play", transportSize)) {
		_shouldPlay = !_shouldPlay;
		_timerStart = currentTime - _timer;
		if(!_shouldPlay){
			if(auto fs = std::dynamic_pointer_cast<MIDISceneFile>(_scene)) fs->silenceAllNotes();
		}
	}
	ImGui::helpTooltip("Shortcut: P");
	ImGuiSameLine();
	if (ImGui::Button("Restart", transportSize)) {
		reset();
	}
	ImGui::helpTooltip("Shortcut: R");
	ImGuiSameLine();
	if (ImGui::Button("Hide", transportSize)) {
		_showGUI = false;
	}
	ImGui::helpTooltip("Press I to show the panel again");

	// About mark, aligned on the right edge.
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("(?)").x);
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		const std::string versionString = std::string("MIDIVisualizer v") + std::to_string(MIDIVIZ_VERSION_MAJOR) + "." + std::to_string(MIDIVIZ_VERSION_MINOR);
		ImGui::TextUnformatted(versionString.c_str());
		ImGui::TextUnformatted("Created by S. Rodriguez (kosua20)");
		ImGui::TextUnformatted("github.com/kosua20/MIDIVisualizer");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
	if(ImGui::IsItemClicked()){
		sr_gui_open_in_browser("https://github.com/kosua20/MIDIVisualizer");
	}
}

void Viewer::showNoteOptions() {

	if(channelColorEdit("Notes", "Notes", _state.notes.majorColors)){
		synchronizeColors(_state.notes.majorColors);
	}
	ImGui::helpTooltip(s_color_major_dsc);
	ImGuiSameLine();

	if(channelColorEdit("Minors", "Minors", _state.notes.minorColors)){
		synchronizeColors(_state.notes.minorColors);
	}
	ImGui::helpTooltip(s_color_minor_dsc);
	ImGuiSameLine(COLUMN_SIZE);

	if(ImGui::Button("Use background images...")){
		ImGui::OpenPopup("Note background textures");
	}
	ImGui::helpTooltip("Define background textures for major and minor notes");

	if(ImGui::BeginPopup("Note background textures")){
		ImGui::paperBackground();
		struct BackgroundTextureParams {
			const char* name;
			std::vector<std::string>& paths;
			float& scale;
			float& alpha;
			bool& scroll;
			GLuint& tex;
		};

		BackgroundTextureParams paramGroups[2] = {
			{"Major notes", _state.notes.majorImagePath, _state.notes.majorTexScale, _state.notes.majorTexAlpha, _state.notes.majorTexScroll, _state.notes.majorTex},
			{"Minor notes", _state.notes.minorImagePath, _state.notes.minorTexScale, _state.notes.minorTexAlpha, _state.notes.minorTexScroll, _state.notes.minorTex},
		};

		const char* paramDescs[2][4] = {
			{"Load an image for major notes", s_notes_major_img_scale_dsc, s_notes_major_img_intensity_dsc, s_notes_major_img_scroll_dsc},
			{"Load an image for minor notes", s_notes_minor_img_scale_dsc, s_notes_minor_img_intensity_dsc, s_notes_minor_img_scroll_dsc},
		};

		for(unsigned int i = 0; i < 2; ++i){
			BackgroundTextureParams& group = paramGroups[i];
			ImGui::PushID(group.name);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s", group.name);
			ImGuiSameLine(115);
			if(ImGui::Button("Load...")){
				 // Read arguments.
				 char *outPath = nullptr;
				 int res = sr_gui_ask_load_file("Select image", "", "jpg,jpeg,png", &outPath);
				 System::forceLocale();
				 if(res == SR_GUI_VALIDATED && outPath){
					 group.paths = { std::string(outPath) };
					 glDeleteTextures(1, &group.tex);
					 group.tex = loadTexture(group.paths[0], 4, false);
				 }
				 free(outPath);
			 }
			ImGui::helpTooltip(paramDescs[i][0]);
			ImGuiSameLine();
			if(ImGui::Button("Clear")) {
				 group.paths.clear();
				 glDeleteTextures(1, &group.tex);
				 group.tex = 0;
			}
			ImGui::helpTooltip("Remove the current image");
			ImGuiSameLine();

			ImGuiPushItemWidth(ITEM_WIDTH);
			if(ImGui::SliderFloat("Scale", &group.scale, 0.1f, 15.0f, "%.2fx")){
				group.scale = glm::max(group.scale, 0.001f);
			}
			ImGui::helpTooltip(paramDescs[i][1]);
			ImGuiSameLine();

			if(ImGui::SliderPercent("Intensity", &group.alpha, 0.0f, 1.0f)){
				group.alpha = glm::clamp(group.alpha, 0.0f, 1.0f);
			}
			ImGui::helpTooltip(paramDescs[i][2]);
			ImGuiSameLine();

			ImGui::Checkbox("Scroll", &group.scroll);
			ImGui::helpTooltip(paramDescs[i][3]);
			ImGui::PopItemWidth();
			ImGui::PopID();
		}

		ImGui::EndPopup();
	}


	ImGuiPushItemWidth(ITEM_WIDTH);
	bool smw0 = ImGui::InputFloat("Scale", &_state.scale, 0.01f, 0.1f, "%.2fx");
	ImGui::helpTooltip(s_time_scale_dsc);
	ImGuiSameLine(COLUMN_SIZE);

	smw0 = ImGui::SliderPercent("Minor width", &_state.background.minorsWidth, 0.1f, 1.0f) || smw0;
	ImGui::helpTooltip(s_minor_size_dsc);
	ImGui::PopItemWidth();

	if (smw0) {
		_state.scale = (std::max)(_state.scale, 0.01f);
		_state.background.minorsWidth = glm::clamp(_state.background.minorsWidth, 0.1f, 1.0f);
		// TODO: (MV) just apply when needed?
		_renderer.setScaleAndMinorWidth(_state.scale, _state.background.minorsWidth);
	}

	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::SliderFloat("Radius", &_state.notes.cornerRadius, 0.0f, 1.0f)){
		_state.notes.cornerRadius = glm::clamp(_state.notes.cornerRadius, 0.0f, 1.0f);
	}
	ImGui::helpTooltip(s_notes_corner_radius_dsc);

	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::SliderFloat("Fadeout", &_state.notes.fadeOut, 0.0f, 1.0f)){
		_state.notes.fadeOut = glm::clamp(_state.notes.fadeOut, 0.0f, 1.0f);
		_renderer.setKeyboardSizeAndFadeout(_state.keyboard.size, _state.notes.fadeOut);
	}
	ImGui::helpTooltip(s_fadeout_notes_dsc);

	if(ImGui::SliderPercent("Edge", &_state.notes.edgeWidth, 0.0f, 1.0f)){
		_state.notes.edgeWidth = glm::clamp(_state.notes.edgeWidth, 0.0f, 1.0f);
	}
	ImGui::helpTooltip(s_notes_edge_width_dsc);
	ImGuiSameLine(COLUMN_SIZE);

	if(ImGui::SliderPercent("Intensity", &_state.notes.edgeBrightness, 0.0f, 4.0f)){
		_state.notes.edgeBrightness = glm::max(_state.notes.edgeBrightness, 0.0f);
	}
	ImGui::helpTooltip(s_notes_edge_intensity_dsc);

	ImGui::PopItemWidth();
}

void Viewer::showFlashOptions() {
	if(channelColorEdit("Color##Flashes", "Color", _state.flashes.colors)){
		synchronizeColors(_state.flashes.colors);
	}
	ImGui::helpTooltip(s_color_flashes_dsc);

	ImGuiSameLine(COLUMN_SIZE);
	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderFloat("Scale##flash", &_state.flashes.size, 0.1f, 3.0f, "%.2fx");
	ImGui::helpTooltip(s_flashes_size_dsc);
	ImGui::PopItemWidth();

	// Additional halo control
	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::SliderFloat("Halo min", &_state.flashes.haloInnerRadius, 0.0f, 1.0f)){
		_state.flashes.haloInnerRadius = glm::clamp(_state.flashes.haloInnerRadius, 0.0f, _state.flashes.haloOuterRadius);
	}
	ImGui::helpTooltip(s_flashes_halo_inner_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::SliderFloat("Halo max", &_state.flashes.haloOuterRadius, 0.0f, 1.0f)){
		_state.flashes.haloOuterRadius = glm::clamp(_state.flashes.haloOuterRadius, _state.flashes.haloInnerRadius, 1.0f);
	}
	ImGui::helpTooltip(s_flashes_halo_outer_dsc);

	if(ImGui::SliderPercent("Intensity##Halo", &_state.flashes.haloIntensity, 0.0f, 2.0f)){
		_state.flashes.haloIntensity = std::max(0.f, _state.flashes.haloIntensity);
	}
	ImGui::helpTooltip(s_flashes_halo_intensity_dsc);

	// Texture atlas (specify rows/cols count)
	if (ImGui::Button("Load image...##Flashes")){
		// Read arguments.
		char *outPath = nullptr;
		int res = sr_gui_ask_load_file("Select image", "", "jpg,jpeg,png", &outPath);
		System::forceLocale();
		if(res == SR_GUI_VALIDATED && outPath){
			_state.flashes.imagePath = { std::string(outPath) };
			if(_state.flashes.tex != ResourcesManager::getTextureFor("flash")){
				glDeleteTextures(1, &_state.flashes.tex);
			}
			_state.flashes.tex = loadTexture(_state.flashes.imagePath[0], 1, false);
		}
		free(outPath);
	}
	ImGui::helpTooltip("Load an atlas image for the animated flash");

	ImGuiSameLine(COLUMN_SIZE);
	if (ImGui::Button("Clear image##Flashes")) {
		_state.flashes.imagePath.clear();
		if(_state.flashes.tex != ResourcesManager::getTextureFor("flash")){
			glDeleteTextures(1, &_state.flashes.tex);
		}
		_state.flashes.tex = ResourcesManager::getTextureFor("flash");
		_state.flashes.texColCount = 2;
		_state.flashes.texRowCount = 4;
	}
	ImGui::helpTooltip("Restore the default flash image atlas");

	// Don't expose tiling on default image.
	if(!_state.flashes.imagePath.empty()){
		if(ImGui::InputInt("Columns", &_state.flashes.texColCount)){
			_state.flashes.texColCount = std::max(1, _state.flashes.texColCount);
		}
		ImGui::helpTooltip(s_flashes_img_columns_dsc);
		ImGuiSameLine(COLUMN_SIZE);
		if(ImGui::InputInt("Rows", &_state.flashes.texRowCount)){
			_state.flashes.texRowCount = std::max(1, _state.flashes.texRowCount);
		}
		ImGui::helpTooltip(s_flashes_img_rows_dsc);
	}

	ImGui::PopItemWidth();
}

void Viewer::showParticleOptions(){
	ImGui::PushID("ParticlesSettings");

	if(channelColorEdit("Color##Particles", "Color", _state.particles.colors)){
		synchronizeColors(_state.particles.colors);
	}
	ImGui::helpTooltip(s_color_particles_dsc);

	ImGuiSameLine(COLUMN_SIZE);

	ImGuiPushItemWidth(ITEM_WIDTH);
	if (ImGui::InputFloat("Size##particles", &_state.particles.scale, 0.1f, 1.0f, "%.1fpx")) {
		_state.particles.scale = glm::clamp(_state.particles.scale, 0.1f, 10.0f);
	}
	ImGui::helpTooltip(s_particles_size_dsc);

	if (ImGui::SliderInt("Count", &_state.particles.count, 1, 512)) {
		_state.particles.count = glm::clamp(_state.particles.count, 1, 512);
	}
	ImGui::helpTooltip(s_particles_count_dsc);
	ImGui::PopItemWidth();

	ImGuiSameLine(COLUMN_SIZE);

	if(ImGui::Button("Configure images...##Particles")) {
		_showParticleEditor = true;
		_backupState = _state;
	}
	ImGui::helpTooltip("Define images to assign randomly to each particle");

	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::SliderFloat("Float height", &_state.particles.floatHeight, 0.1f, 3.0f, "%.2fx")){
		_state.particles.floatHeight = glm::clamp(_state.particles.floatHeight, 0.1f, 3.0f);
	}
	ImGui::helpTooltip(s_particles_float_height_dsc);

	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::SliderFloat("Lifetime", &_state.particles.lifetime, 0.2f, 10.0f, "%.1fs")){
		_state.particles.lifetime = glm::clamp(_state.particles.lifetime, 0.2f, 10.0f);
	}
	ImGui::helpTooltip(s_particles_lifetime_dsc);

	ImGui::SliderPercent("Sway", &_state.particles.sway, 0.0f, 2.0f);
	ImGui::helpTooltip(s_particles_turbulences_dsc);
	ImGui::PopItemWidth();

	ImGui::PopID();
}

void Viewer::showKeyboardOptions(){
	ImGuiPushItemWidth(25);
	ImGui::ColorEdit3("Edge color##Keys", &_state.keyboard.edgeColor[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_keyboard_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::ColorEdit3("Fill color##Keys", &_state.keyboard.backColor[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_keyboard_bg_dsc);
	ImGui::PopItemWidth();

	ImGuiPushItemWidth(ITEM_WIDTH);
	if(ImGui::SliderPercent("Height##Keys", &_state.keyboard.size, 0.0f, 1.0f)){
		_state.keyboard.size = glm::clamp(_state.keyboard.size, 0.0f, 1.0f);
		_renderer.setKeyboardSizeAndFadeout(_state.keyboard.size, _state.notes.fadeOut);
	}
	ImGui::helpTooltip(s_keyboard_size_dsc);
	ImGuiSameLine(COLUMN_SIZE);

	if(ImGui::SliderPercent("Minor height##Keys", &_state.keyboard.minorHeight, 0.0f, 1.0f)){
		_state.keyboard.minorHeight = glm::clamp(_state.keyboard.minorHeight, 0.0f, 1.0f);
		// TODO: (MV) just apply when needed?
		_renderer.setMinorEdgesAndHeight(_state.keyboard.minorEdges, _state.keyboard.minorHeight);
	}
	ImGui::helpTooltip(s_keyboard_minor_height_dsc);
	ImGui::PopItemWidth();

	ImGuiPushItemWidth(25);
	if (ImGui::Checkbox("Minor edges##Keys", &_state.keyboard.minorEdges)){
		// TODO: (MV) just apply when needed?
		_renderer.setMinorEdgesAndHeight(_state.keyboard.minorEdges, _state.keyboard.minorHeight);
	}
	ImGui::helpTooltip(s_keyboard_minor_edges_dsc);
	ImGui::PopItemWidth();
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::Checkbox("Highlight keys", &_state.keyboard.highlightKeys);
	ImGui::helpTooltip(s_keyboard_highlight_dsc);

	if (_state.keyboard.highlightKeys) {
		ImGui::Checkbox("Custom colors", &_state.keyboard.customKeyColors);
		ImGui::helpTooltip(s_keyboard_custom_colors_dsc);

		if (_state.keyboard.customKeyColors) {
			ImGuiSameLine(COLUMN_SIZE);
			ImGuiPushItemWidth(25);
			if(ImGui::ColorEdit3("Major##KeysHighlight", &_state.keyboard.majorColor[0][0], ImGuiColorEditFlags_NoInputs)){
				// Ensure synchronization of the override array.
				for(size_t cid = 1; cid < _state.keyboard.majorColor.size(); ++cid){
					_state.keyboard.majorColor[cid] = _state.keyboard.majorColor[0];
				}
			}
			ImGui::helpTooltip(s_color_keyboard_major_dsc);

			ImGuiSameLine(COLUMN_SIZE+95);
			if(ImGui::ColorEdit3("Minor##KeysHighlight", &_state.keyboard.minorColor[0][0], ImGuiColorEditFlags_NoInputs)){
				// Ensure synchronization of the override array.
				for(size_t cid = 1; cid < _state.keyboard.minorColor.size(); ++cid){
					_state.keyboard.minorColor[cid] = _state.keyboard.minorColor[0];
				}
			}
			ImGui::helpTooltip(s_color_keyboard_minor_dsc);
			ImGui::PopItemWidth();
		}
	}
}

void Viewer::showPedalOptions(){
	ImGuiPushItemWidth(25);
	if(ImGui::ColorEdit3("Colors##Pedals", &_state.pedals.centerColor[0], ImGuiColorEditFlags_NoInputs)){
		// Synchronize other colors
		_state.pedals.topColor = _state.pedals.centerColor;
		_state.pedals.leftColor = _state.pedals.centerColor;
		_state.pedals.rightColor = _state.pedals.centerColor;
	}
	ImGui::helpTooltip(s_color_pedal_dsc);
	ImGui::PopItemWidth();

	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::Combo("Location", (int*)&_state.pedals.location, "Top left\0Bottom left\0Top right\0Bottom right\0");
	ImGui::helpTooltip(s_pedal_location_dsc);

	if(ImGui::SliderPercent("Opacity##Pedals", &_state.pedals.opacity, 0.0f, 1.0f)){
		_state.pedals.opacity = glm::clamp(_state.pedals.opacity, 0.0f, 1.0f);
	}
	ImGui::helpTooltip(s_pedal_opacity_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::SliderPercent("Size##Pedals", &_state.pedals.size, 0.05f, 0.5f)){
		_state.pedals.size = glm::clamp(_state.pedals.size, 0.05f, 0.5f);
	}
	ImGui::helpTooltip(s_pedal_size_dsc);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Merge pedals", &_state.pedals.merge);
	ImGui::helpTooltip(s_pedal_merge_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	if(ImGui::Button("Configure images...##Pedals")) {
		_showPedalsEditor = true;
		_backupState = _state;
	}
	ImGui::helpTooltip("Define images to use for each pedal and their layout");

}

void Viewer::showWaveOptions(){
	ImGuiPushItemWidth(25);
	ImGui::ColorEdit3("Color##Waves", &_state.waves.color[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_wave_dsc);
	ImGui::PopItemWidth();

	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderFloat("Amplitude##Waves", &_state.waves.amplitude, 0.0f, 5.0f, "%.2fx");
	ImGui::helpTooltip(s_wave_amplitude_dsc);

	ImGui::SliderFloat("Spread##Waves", &_state.waves.spread, 0.0f, 5.0f, "%.2fx");
	ImGui::helpTooltip(s_wave_size_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderFloat("Frequency##Waves", &_state.waves.frequency, 0.0f, 5.0f, "%.2fx");
	ImGui::helpTooltip(s_wave_frequency_dsc);

	if(ImGui::SliderPercent("Opacity##Waves", &_state.waves.opacity, 0.0f, 1.0f)){
		_state.waves.opacity = glm::clamp(_state.waves.opacity, 0.0f, 1.0f);
	}
	ImGui::helpTooltip(s_wave_opacity_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderFloat("Speed##Waves", &_state.waves.speed, 0.0f, 5.0f, "%.2fx");
	ImGui::helpTooltip(s_wave_speed_dsc);
	
	ImGui::SliderPercent("Noise##Waves", &_state.waves.noiseIntensity, 0.0f, 2.0f);
	ImGui::helpTooltip(s_wave_noise_intensity_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderPercent("Extent##Waves", &_state.waves.noiseSize, 0.0f, 1.0f);
	ImGui::helpTooltip(s_wave_noise_extent_dsc);
	ImGui::PopItemWidth();

}

void Viewer::showBlurOptions(){
	ImGui::Checkbox("Blur the notes", &_state.showBlurNotes);
	ImGui::helpTooltip(s_show_blur_notes_dsc);
	ImGuiSameLine(COLUMN_SIZE);

	ImGuiPushItemWidth(ITEM_WIDTH);
	if (ImGui::SliderFloat("Fading", &_state.attenuation, 0.0f, 1.0f)) {
		_state.attenuation = glm::clamp(_state.attenuation, 0.0f, 1.0f);
		_blurringScreen.program().use();
		_blurringScreen.program().uniform("attenuationFactor", _state.attenuation);
		glUseProgram(0);
	}
	ImGui::helpTooltip(s_blur_attenuation_dsc);
	ImGui::PopItemWidth();
}

void Viewer::showScoreOptions(){
	ImGuiPushItemWidth(25);
	ImGui::ColorEdit3("Vertical lines##Background", &_state.score.vLinesColor[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_lines_vertical_dsc);
	ImGuiSameLine();

	ImGui::ColorEdit3("Horizontal lines##Background", &_state.score.hLinesColor[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_lines_horizontal_dsc);
	ImGuiSameLine();

	ImGui::ColorEdit3("Labels##Background", &_state.score.digitsColor[0], ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_numbers_dsc);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Horizontal lines", &_state.score.hLines);
	ImGui::helpTooltip(s_show_horiz_lines_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderFloat("Thickness##Horizontal", &_state.score.hLinesWidth, 0.0f, 15.0f, "%.0fpx");
	ImGui::helpTooltip(s_score_lines_horizontal_width_dsc);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Vertical lines", &_state.score.vLines);
	ImGui::helpTooltip(s_show_vert_lines_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderFloat("Thickness##Vertical", &_state.score.vLinesWidth, 0.0f, 15.0f, "%.0fpx");
	ImGui::helpTooltip(s_score_lines_vertical_width_dsc);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Digits", &_state.score.digits);
	ImGui::helpTooltip(s_show_numbers_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderPercent("Scale##Digits", &_state.score.digitsScale, 0.0f, 0.5f);
	ImGui::helpTooltip(s_score_digits_size_dsc);
	ImGui::PopItemWidth();

	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderPercent("Offset X##Digits", &_state.score.digitsOffset[0], -1.f, 1.0f);
	ImGui::helpTooltip(s_score_digits_offset_x_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderPercent("Offset Y##Digits", &_state.score.digitsOffset[1], -1.f, 1.0f);
	ImGui::helpTooltip(s_score_digits_offset_y_dsc);
	ImGui::PopItemWidth();

}

void Viewer::showBackgroundOptions(){
	ImGuiPushItemWidth(25);
	const glm::vec3 oldColor(_state.background.color);
	ImGui::ColorEdit3("Color##Background", &_state.background.color[0],
		ImGuiColorEditFlags_NoInputs);
	ImGui::helpTooltip(s_color_bg_dsc);
	if(oldColor != _state.background.color){
		applyBackgroundColor();
	}
	ImGui::PopItemWidth();
	ImGuiSameLine(COLUMN_SIZE);

	ImGuiPushItemWidth(ITEM_WIDTH);
	if (ImGui::SliderPercent("Opacity##Background", &_state.background.imageAlpha, 0.0f, 1.0f)) {
		_state.background.imageAlpha = glm::clamp(_state.background.imageAlpha, 0.0f, 1.0f);
	}
	ImGui::helpTooltip(s_bg_img_opacity_dsc);

	ImGui::PopItemWidth();
	if (ImGui::Button("Load image...##Background")){
		// Read arguments.
		char *outPath = nullptr;
		int res = sr_gui_ask_load_file("Select image", "", "jpg,jpeg,png", &outPath);
		System::forceLocale();
		if(res == SR_GUI_VALIDATED && outPath){
			_state.background.imagePath = { std::string(outPath) };
			glDeleteTextures(1, &_state.background.tex);
			_state.background.tex = loadTexture(_state.background.imagePath[0], 4, false);
			if(_state.background.tex != 0){
				_state.background.image = true;
				// Ensure minimal visibility.
				if (_state.background.imageAlpha < 0.1f) {
					_state.background.imageAlpha = 0.1f;
				}
			}
		}
		free(outPath);
	}
	ImGui::helpTooltip("Define an image for the background");
	ImGuiSameLine(COLUMN_SIZE);

	if (ImGui::Button("Clear image##Background")) {
		_state.background.image = false;
		_state.background.imagePath.clear();
		glDeleteTextures(1, &_state.background.tex);
		_state.background.tex = 0;
	}
	ImGui::helpTooltip("Remove the background image");

	ImGuiPushItemWidth(ITEM_WIDTH);
	ImGui::SliderFloat("Scroll X##Background", &_state.background.scrollSpeed[0], -0.25f, 0.25f);
	ImGui::helpTooltip(s_bg_img_scroll_x_dsc);
	ImGuiSameLine(COLUMN_SIZE);
	ImGui::SliderFloat("Scroll Y##Background", &_state.background.scrollSpeed[1], -0.25f, 0.25f);
	ImGui::helpTooltip(s_bg_img_scroll_y_dsc);
	ImGui::PopItemWidth();

	ImGui::Checkbox("Image extends under keyboard", &_state.background.imageBehindKeyboard);
	ImGui::helpTooltip(s_bg_img_behind_keyboard_dsc);

}

void Viewer::showBottomButtons(){
	if (gridButton("Save settings...")) {
		// Read arguments.
		char* savePath = nullptr;
		int res = sr_gui_ask_save_file("Create config file", "", "ini", &savePath);
		System::forceLocale();
		if(res == SR_GUI_VALIDATED && savePath){
			_state.save(std::string(savePath));
		}
		free(savePath);
	}
	ImGui::helpTooltip("Save the current settings for all effects");
	ImGuiSameLine(COLUMN_SIZE);

	if (gridButton("Load settings...")) {
		// Read arguments.
		char* outPath = nullptr;
		int res = sr_gui_ask_load_file("Select config file", "", "ini", &outPath);
		System::forceLocale();
		if(res == SR_GUI_VALIDATED && outPath){
			if(_state.load(std::string(outPath))){
				setState(_state);
			}
		}
		free(outPath);
	}
	ImGui::helpTooltip("Load effects settings from a configuration file");

	if(gridButton("Export video...")){
		ImGui::OpenPopup("Export");
	}
	ImGui::helpTooltip("Export video recording");

	if(_recorder.drawGUI(_guiScale)){
		startRecording();
	}
	ImGuiSameLine(COLUMN_SIZE);

	if (gridButton("Reset settings")) {
		_state.reset();
		setState(_state);
	}
	ImGui::helpTooltip("Restore the default effects settings");
}

void Viewer::showLayers() {
	const ImVec2 & screenSize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
	
	if (ImGui::Begin("Layers", &_showLayers)) {
		ImGui::paperBackground();
		ImGui::TextDisabled("You can drag and drop layers to reorder them.");
		for (int i = int(_state.layersMap.size()) - 1; i >= 0; --i) {
			const int layerId = _state.layersMap[i];
			if (layerId >= _layers.size()) {
				continue;
			}
			auto & layer = _layers[layerId];
			if (layer.type == Layer::BGCOLOR) {
				continue;
			}
			ImGui::Separator();
			ImGui::PushID(layerId);

			ImGui::Checkbox("##LayerCheckbox", layer.toggle);
			ImGuiSameLine();
			ImGui::Selectable(layer.name.c_str());

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				ImGui::Text("%s", layer.name.c_str());
				ImGui::SetDragDropPayload("REORDER_LAYER", &i, sizeof(int));
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("REORDER_LAYER"))
				{
					int iPayload = *(const int*)payload->Data;
					int newId = _state.layersMap[iPayload];
					// Instead of just swapping, we shift all intermediate indices.
					const int ddlt = (iPayload <= i ? 1 : -1);
					for (int lid = iPayload; lid != i; lid += ddlt) {
						_state.layersMap[lid] = _state.layersMap[lid + ddlt];
					}
					_state.layersMap[i] = newId;
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::PopID();
		}
		ImGui::Separator();
	}
	ImGui::End();
}

void Viewer::showDevices(){
	if(ImGui::BeginPopupModal("Devices", nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::paperBackground();

		bool starting = false;
		const ImVec2 buttonSize(_guiScale * (EXPORT_COLUMN_SIZE-20.0f), 0.0f);

		ImGui::Text("Select a device to listen to or");

		ImGuiSameLine();

		if(ImGui::SmallButton("start virtual device")){
			try {
				_scene = std::make_shared<MIDISceneLive>(-1, _verbose);
				starting = true;
			} catch(const std::exception & e){
				std::cerr << "[MIDI] Unable to start virtual device: " << e.what() << std::endl;
			}
		}
		ImGui::helpTooltip("Act as a virtual device (via JACK)\nother MIDI elements can connect to");
		ImGui::Separator();

		const auto & devices = MIDISceneLive::availablePorts();
		for(int i = 0; i < devices.size(); ++i){
			ImGui::RadioButton(devices[i].c_str(), &_selectedPort, i);
		}

		if(devices.empty()){
			ImGui::TextDisabled("No device found.");
		}

		ImGui::Separator();

		if(ImGui::Button("Cancel", buttonSize)){
			ImGui::CloseCurrentPopup();
		}

		if(!devices.empty()){
			ImGuiSameLine(EXPORT_COLUMN_SIZE);
			// The selection can be stale (failed auto-connect, device unplugged since):
			// a negative index would silently start a virtual device instead.
			const bool validPort = _selectedPort >= 0 && _selectedPort < int(devices.size());
			if(!validPort){
				ImGui::BeginDisabled();
			}
			if(ImGui::Button("Start", buttonSize)){
				try {
					_scene = std::make_shared<MIDISceneLive>(_selectedPort, _verbose);
					starting = true;
				} catch(const std::exception & e){
					// The device can vanish between the listing and the opening.
					std::cerr << "[MIDI] Unable to connect to device: " << e.what() << std::endl;
				}
			}
			if(!validPort){
				ImGui::EndDisabled();
			}
		}

		if(starting){
			_scene->setTranspose(_state.transpose);
			_timer = 0.0f;
			_shouldPlay = true;
			_state.reverseScroll = true;
			_state.scrollSpeed = 1.0f;
			_liveplay = true;
			applyAllSettings();

			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void Viewer::showVisibility(){

	if(ImGui::BeginPopup("Visibility options")){
		ImGui::paperBackground();
		bool shouldUpdate = false;

		ImGui::Text("Tracks");
		ImGui::Separator();
		ImGuiPushItemWidth(35);
		const size_t trackCount = _state.filter.tracks.size();
		const std::string tenPrefix = trackCount < 10 ? "" : "0";
		for(size_t cid = 0; cid < trackCount; ++cid){
			const std::string nameC = (cid < 10 ? tenPrefix : "") + std::to_string(cid);
			bool val = _state.filter.tracks[cid];
			shouldUpdate = ImGui::Checkbox(nameC.c_str(), &val) || shouldUpdate;
			if(shouldUpdate){
				_state.filter.tracks[cid] = val;
			}
			if((cid % 4 != 3) && (cid != (trackCount-1))){
				ImGuiSameLine();
			}
		}
		ImGui::PopItemWidth();

		// Do 4x4 columns of checkboxes
		ImGui::Text("Channels");
		ImGui::Separator();
		ImGuiPushItemWidth(35);
		for(size_t cid = 0; cid < _state.filter.channels.size(); ++cid){
			const std::string nameC = std::string(cid < 10 ? "0" : "") + std::to_string(cid);
			shouldUpdate = ImGui::Checkbox(nameC.c_str(), &_state.filter.channels[cid]) || shouldUpdate;
			if(cid % 4 != 3){
				ImGuiSameLine();
			}
		}
		ImGui::PopItemWidth();

		if(shouldUpdate){
			_scene->updateVisibleNotes(_state.filter);
		}
		ImGui::EndPopup();
	}

}

void Viewer::showSets(){
	if(ImGui::BeginPopup("Note sets options")){
		ImGui::paperBackground();
		ImGui::Text("Decide how notes should be grouped in multiple sets");
		ImGui::Text("(to which you can assign different key/effects colors).");
		ImGui::Text("This can be based on the MIDI channel, the track, the key,");
		ImGui::Text("using the chromatic sequence, separating notes that are");
		ImGui::Text("lower or higher than a given key, or defining custom lists.");

		bool shouldUpdate = false;
		shouldUpdate = ImGui::RadioButton("Channel", (int*)(&_state.setOptions.mode), int(SetMode::CHANNEL)) || shouldUpdate;
		ImGui::helpTooltip("Assign each channel to a color set");
		ImGuiSameLine(115);
		shouldUpdate = ImGui::RadioButton("Track", (int*)(&_state.setOptions.mode), int(SetMode::TRACK)) || shouldUpdate;
		ImGui::helpTooltip("Assign each track to a color set");
		ImGuiSameLine(2*115);
		shouldUpdate = ImGui::RadioButton("Key", (int*)(&_state.setOptions.mode), int(SetMode::KEY)) || shouldUpdate;
		ImGui::helpTooltip("Assign each of the eight keys to a color set");
		ImGuiSameLine(3*115);
		shouldUpdate = ImGui::RadioButton("Chromatic", (int*)(&_state.setOptions.mode), int(SetMode::CHROMATIC)) || shouldUpdate;
		ImGui::helpTooltip("Assign each of the twelve keys to a color set");

		shouldUpdate = ImGui::RadioButton("Split", (int*)(&_state.setOptions.mode), int(SetMode::SPLIT)) || shouldUpdate;
		ImGui::helpTooltip("Assign keys to a color set based on a separating key");
		ImGuiSameLine();
		ImGuiPushItemWidth(ITEM_WIDTH);
		shouldUpdate = ImGui::Combo("##key", &_state.setOptions.key, midiKeysStrings, 128) || shouldUpdate;
		ImGui::PopItemWidth();

		ImGuiSameLine(2*115);
		shouldUpdate = ImGui::RadioButton("List", (int*)(&_state.setOptions.mode), int(SetMode::LIST)) || shouldUpdate;
		ImGui::helpTooltip("Assign keys to sets based on a list of keys, sets and timings");
		ImGuiSameLine();
		if(ImGui::Button("Configure...")){
			_showSetListEditor = true;
			_backupState = _state;
			ImGui::CloseCurrentPopup();
		}

		if(shouldUpdate){
			_state.setOptions.rebuild();
			_scene->updateSetsAndVisibleNotes(_state.setOptions, _state.filter);
		}
		ImGui::EndPopup();
	}

}

static constexpr char const* kSetsComboString = " 0\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0 11\0\0";

void Viewer::showSetEditor(){

	const unsigned int colWidth = 80;
	const unsigned int colButtonWidth = 50;
	const float offset = 8;

	// For editing.
	static SetOptions::KeyFrame newKey;

	// Initial window position.
	const ImVec2 & screenSize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.1f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowSize({360, 360}, ImGuiCond_FirstUseEver);

	if(ImGui::Begin("Set List Editor", &_showSetListEditor)){
		ImGui::paperBackground();

		bool refreshSetOptions = false;

		// Header
		ImGui::TextWrapped("Control points will determine which range of notes belong to each set at a given time. All notes below the given key will be assigned to the specified set (except if a set with a lower number is defined). Control points will kept being applied until a new one is encountered for the set.");

		// Load/save as CSV.
		if(ImGui::Button("Save control points...")){
			char* savePath = nullptr;
			int res = sr_gui_ask_save_file("Create CSV file", "", "csv", &savePath);
			System::forceLocale();
			if(res == SR_GUI_VALIDATED && savePath){
				const std::string content = _state.setOptions.toKeysString("\n");
				System::writeStringToFile(std::string(savePath), content);
			}
			free(savePath);
		}
		ImGuiSameLine();
		if(ImGui::Button("Load control points...")){
			// Read arguments.
			char* outPath = nullptr;
			int res = sr_gui_ask_load_file("Select CSV file", "", "csv", &outPath);
			System::forceLocale();
			if(res == SR_GUI_VALIDATED && outPath){
				const std::string str = System::loadStringFromFile(std::string(outPath));
				_state.setOptions.fromKeysString(str);
			}
			free(outPath);
			refreshSetOptions = true;
		}
		ImGuiSameLine();
		// Just restore the last backup.
		if(ImGui::Button("Reset")){
			_state = _backupState;
			refreshSetOptions = true;
		}
		ImGui::Separator();

		// List of existing keys.
		// Keep some room at the bottom for the "new key" section.
		ImVec2 listSize = ImGui::GetContentRegionAvail();
		listSize.y -= 1.5f * ImGui::GetTextLineHeightWithSpacing();

		if(ImGui::BeginTable("#List", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |  ImGuiTableFlags_BordersH, listSize)){
			const size_t rowCount = _state.setOptions.keys.size();

			// Header
			ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
			ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, _guiScale * colWidth);
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, _guiScale * colWidth);
			ImGui::TableSetupColumn("Set", ImGuiTableColumnFlags_WidthFixed, _guiScale * colWidth);
			ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_NoHeaderLabel | ImGuiTableColumnFlags_WidthFixed, colButtonWidth);
			ImGui::TableHeadersRow();

			int removeIndex = -1;

			for(size_t row = 0u; row < rowCount; ++row){

				SetOptions::KeyFrame& key = _state.setOptions.keys[row];

				ImGui::TableNextColumn();
				ImGui::PushID((unsigned int)row);

				ImGuiPushItemWidth(colWidth);
				if(ImGui::InputDouble("##Time", &key.time, 0, 0, "%.3fs")){
					key.time = (std::max)(key.time, 0.0);
				}
				// Postpone update until we are not focused anymore (else rows will jump around).
				if(ImGui::IsItemDeactivatedAfterEdit()){
					refreshSetOptions = true;
				}
				ImGui::PopItemWidth();

				ImGui::TableNextColumn();
				ImGuiPushItemWidth(colWidth);
				if(ImGui::Combo("##Key", &key.key, midiKeysStrings, 128)){
					refreshSetOptions = true;
				}
				ImGui::PopItemWidth();

				ImGui::TableNextColumn();
				ImGuiPushItemWidth(colWidth);
				// It is simpler to use a combo here (no weird focus issues when sorting rows).
				if(ImGui::Combo("##Set", &key.set, kSetsComboString)){
					refreshSetOptions = true;
				}
				ImGui::PopItemWidth();

				ImGui::TableNextColumn();
				if(ImGui::Button("x")){
					removeIndex = int(row);
				}

				ImGui::PopID();
			}

			ImGui::EndTable();

			// Remove after displaying the table.
			if(removeIndex >= 0){
				_state.setOptions.keys.erase(_state.setOptions.keys.begin() + removeIndex);
				refreshSetOptions = true;
			}
		}

		// Section to add a new key.
		// Mimic the inputs and size/alignment of the table items.
		ImGuiPushItemWidth(colWidth);
		if(ImGui::InputDouble("##Time", &newKey.time, 0, 0, "%.3fs")){
			newKey.time = (std::max)(0.0, newKey.time);
		}
		ImGui::PopItemWidth();
		ImGuiSameLine(int(colWidth + 2 * offset));
		ImGuiPushItemWidth(colWidth);
		ImGui::Combo("##Key", &newKey.key, midiKeysStrings, 128);
		ImGui::PopItemWidth();

		ImGuiSameLine(int(2 * colWidth + 3 * offset));
		ImGuiPushItemWidth(colWidth);
		ImGui::Combo("##Set", &newKey.set, kSetsComboString);
		ImGui::PopItemWidth();

		ImGuiSameLine(int(3 * colWidth + 4 * offset));
		if(ImGui::Button("Add")){
			auto insert = std::upper_bound(_state.setOptions.keys.begin(), _state.setOptions.keys.end(), newKey);
			_state.setOptions.keys.insert(insert, newKey);
			refreshSetOptions = true;
		}

		// Actions
		if(!_showSetListEditor){
			// If we are exiting, refresh the existing set.
			refreshSetOptions = true;
		}

		// If refresh is needed, ensure the set options are rebuilt and the scene udpated for live preview.
		if(refreshSetOptions){
			_state.setOptions.rebuild();
			if(_scene){
				_scene->updateSetsAndVisibleNotes(_state.setOptions, _state.filter);
			}
		}

		if(_showDebug){
			const double time = _timer * _state.scrollSpeed;

			std::array<int, SETS_COUNT> firstNoteInSet;
			std::array<int, SETS_COUNT> lastNoteInSet;
			firstNoteInSet.fill(-1);
			lastNoteInSet.fill(128);
			int lastSet = -1;
			for(int nid = 0; nid < 128; ++nid){
				int newSet = _state.setOptions.apply(nid, 0, 0, time);
				if(newSet != lastSet){
					if(lastSet >= 0){
						lastNoteInSet[lastSet] = nid - 1;
					}
					firstNoteInSet[newSet] = nid;
					lastSet = newSet;
				}
			}
			ImGui::Separator();
			ImGui::Text("Debug: ");
			ImGuiSameLine();
			ImGui::TextDisabled("(press D to hide)");
			ImGui::Text("At time %.3fs: ", time);

			for(unsigned int sid = 0; sid < SETS_COUNT; ++sid){
				int firstNote = firstNoteInSet[sid];
				int lastNote = lastNoteInSet[sid];

				if((firstNote != -1) || (lastNote != 128)){
					firstNote = glm::clamp(firstNote, 0, 127);
					lastNote  = glm::clamp(lastNote, 0, 127);

					ImGui::Text("* Set %u contains notes from %s to %s", sid, midiKeysStrings[firstNote], midiKeysStrings[lastNote]);
				}
			}

		}
	}
	ImGui::End();
}

void Viewer::showParticlesEditor(){

	const unsigned int colWidth = 270;
	const unsigned int colButtonWidth = 20;
	const float offset = 8;
	const unsigned int thumbSize = 24;
	const float thumbDisplaySize = _guiScale * thumbSize;

	// For previewing.
	static std::vector<GLuint> previewTextures;
	if(previewTextures.empty() && !_state.particles.imagePaths.empty()){
		previewTextures = generate2DViewsOfArray(_state.particles.tex, thumbSize);
	}

	// Initial window position.
	const ImVec2 & screenSize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.1f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowSize({360, 360}, ImGuiCond_FirstUseEver);

	if(ImGui::Begin("Particle Images Editor", &_showParticleEditor)){
		ImGui::paperBackground();

		bool refreshTextures = false;

		// Header
		ImGui::TextWrapped("You can select multiple images (PNG or JPEG). They should be square and in grey levels, where black indicates transparent regions, and white regions are fully opaque.");

		if(ImGui::Button("Clear all")){
			_state.particles.imagePaths.clear();
			refreshTextures = true;
		}
		ImGui::helpTooltip("Remove all particle images");
		ImGuiSameLine();
		// Just restore the last backup.
		if(ImGui::Button("Reset")){
			_state = _backupState;
			refreshTextures = true;
		}
		ImGui::helpTooltip("Restore the previous particle images set");
		ImGui::Separator();

		// List of existing keys.
		// Keep some room at the bottom for the "new key" section.
		ImVec2 listSize = ImGui::GetContentRegionAvail();
		listSize.y -= 1.5f * ImGui::GetTextLineHeightWithSpacing();

		if(ImGui::BeginTable("#List", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |  ImGuiTableFlags_BordersH, listSize)){
			const size_t rowCount = _state.particles.imagePaths.size();

			// Header
			ImGui::TableSetupScrollFreeze(0, 1); // Make top row always visible
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHeaderLabel, 1.5f * thumbDisplaySize);
			ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_NoHeaderLabel | ImGuiTableColumnFlags_WidthFixed, _guiScale * colButtonWidth);
			ImGui::TableHeadersRow();

			int removeIndex = -1;
			for(size_t row = 0u; row < rowCount; ++row){

				const std::string& path = _state.particles.imagePaths[row];

				ImGui::TableNextColumn();
				ImGui::PushID((unsigned int)row);
				if(ImGui::Selectable("##rowSelector", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap, ImVec2(0.f, thumbDisplaySize))) {
					// Open directory in file browser.
					sr_gui_open_in_explorer(path.c_str());
				}
				if(ImGui::IsItemHovered()){
					ImGui::SetTooltip("%s",path.c_str());
				}
				if(row < previewTextures.size()){
					ImGui::SameLine();
					ImGui::Image((ImTextureID)(uint64_t)previewTextures[row], ImVec2(thumbDisplaySize,thumbDisplaySize), ImVec2(0.f,1.f), ImVec2(1.f,0.f));
				}
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGuiPushItemWidth(colWidth);
				// Display the filename.
				std::string::size_type pos = path.find_last_of("/\\");
				pos = (pos == std::string::npos) ? 0 : (pos + 1);
				ImGui::Text("%s", path.c_str() + pos);

				ImGui::PopItemWidth();

				ImGui::TableNextColumn();
				if(ImGui::Button("x")){
					removeIndex = int(row);
				}
				ImGui::helpTooltip("Remove from the set");
				ImGui::PopID();
			}
			ImGui::EndTable();

			// Remove after displaying the table.
			if(removeIndex >= 0){
				_state.particles.imagePaths.erase(_state.particles.imagePaths.begin() + removeIndex);
				refreshTextures = true;
			}
		}

		// Section to add a new image.
		if(ImGui::Button("Add")){
			// Read arguments.
			char** outPaths = nullptr;
			int outCount = 0;
			int res = sr_gui_ask_load_files("Select images", "", "png,jpg,jpeg", &outPaths, &outCount);
			System::forceLocale();
			if((res == SR_GUI_VALIDATED) && outPaths && outCount > 0){
				bool wasEmpty = _state.particles.imagePaths.empty();
				for (unsigned int i = 0; i < (unsigned int)outCount; ++i) {
					const std::string imageFilePath = std::string(outPaths[i]);
					_state.particles.imagePaths.push_back(imageFilePath);
				}
				// Ensure particles are zoomed in enough.
				if(wasEmpty && (_state.particles.scale <= 9.0f)) {
					_state.particles.scale = 10.0f;
				}
				refreshTextures = true;
			}
			for(unsigned int i = 0; i < (unsigned int)outCount; ++i){
				free(outPaths[i]);
			}
			free(outPaths);
		}
		ImGui::helpTooltip("Add a new particle image to the set");

		// Actions
		if(!_showParticleEditor){
			// If we are exiting, refresh the existing set.
			refreshTextures = true;
		}

		// If refresh is needed, ensure that the texture array is up to date.
		if(refreshTextures){
			if (_state.particles.tex != ResourcesManager::getTextureFor("blankarray")) {
				glDeleteTextures(1, &_state.particles.tex);
			}
			glDeleteTextures((GLsizei)previewTextures.size(), previewTextures.data());
			previewTextures.clear();

			if(_state.particles.imagePaths.empty()){
				// Use a white square particle appearance by default.
				_state.particles.tex =  ResourcesManager::getTextureFor("blankarray");
				_state.particles.texCount = 1;
				_state.particles.scale = 1.f;

			} else {
				// Load new particles.
				_state.particles.tex = loadTextureArray(_state.particles.imagePaths, false, _state.particles.texCount);
				previewTextures = generate2DViewsOfArray(_state.particles.tex, thumbSize);
			}
		}
	}
	ImGui::End();
}

bool Viewer::drawPedalImageSettings(GLuint tex, const glm::vec2& size, bool labelsAfter, bool flipUV, PathCollection& path, unsigned int index, glm::vec3& color){
	bool refresh = false;
	const ImVec2 startUV(flipUV ? 1.f : 0.f, 1.f);
	const ImVec2 endUV(flipUV ? 0.f : 1.f, 0.f);

	ImGui::BeginGroup();

	if(labelsAfter){
		ImGui::Image((void*)(uint64_t)tex, size, startUV, endUV, color);
	} else {
		// Pad top buttons
		ImGui::Dummy(ImVec2(0.3f * size.x, 5));
		ImGuiSameLine(0);
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f,0.0f));
	ImGui::ColorEdit3("Picker", &color[0], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
	ImGui::PopStyleVar();
	ImGui::helpTooltip("Define the pedal color");

	ImGuiSameLine(0);
	if(ImGui::SmallButton("Load")){
		char* outPath = nullptr;
		int res = sr_gui_ask_load_file("Select image", "", "png,jpg,jpeg", &outPath);
		System::forceLocale();
		if((res == SR_GUI_VALIDATED) && outPath ){
			if(index >= path.size()){
				path.resize(index + 1, std::string(outPath));
			} else {
				path[index] = std::string(outPath);
			}
			refresh = true;
		}
		free(outPath);
	}
	ImGui::helpTooltip("Load an image for the pedal");
	ImGuiSameLine(0);
	if(ImGui::SmallButton("x")){
		path.clear();
		refresh = true;
	}
	ImGui::helpTooltip("Restore the default image");

	if(!labelsAfter){
		ImGui::Image((void*)(uint64_t)tex, size, startUV, endUV, color);
	}

	ImGui::EndGroup();
	return refresh;
};

void Viewer::refreshPedalTextures(State::PedalsState& pedals){
	const GLuint defaultCenter = ResourcesManager::getTextureFor("pedal_center");
	const GLuint defaultTop = ResourcesManager::getTextureFor("pedal_top");
	const GLuint defaultSide = ResourcesManager::getTextureFor("pedal_side");
	// Cleanup non-default textures.
	if(pedals.texCenter != defaultCenter){
		glDeleteTextures(1, &pedals.texCenter);
	}
	if(pedals.texTop != defaultTop){
		glDeleteTextures(1, &pedals.texTop);
	}
	// Clear existing side textures (potentially the same)
	for(unsigned int i = 0; i < 2; ++i){
		const bool neverEncountered = (i == 0) || (pedals.texSides[i] != pedals.texSides[i-1]);
		if((pedals.texSides[i] != defaultSide) && neverEncountered){
			glDeleteTextures(1, &pedals.texSides[i]);
		}
	}
	// Load new textures (or assign default)
	if(pedals.centerImagePath.empty()){
		pedals.texCenter = defaultCenter;
	} else {
		pedals.texCenter = loadTexture(pedals.centerImagePath[0], 1, false);
	}
	if(pedals.topImagePath.empty()){
		pedals.texTop = defaultTop;
	} else {
		pedals.texTop = loadTexture(pedals.topImagePath[0], 1, false);
	}
	// Load the new ones if present.
	const unsigned int newCount = (unsigned int)pedals.sideImagePaths.size();
	for(unsigned int i = 0; i < 2; ++i){
		if(i < newCount){
			pedals.texSides[i] = loadTexture(pedals.sideImagePaths[i], 1, false);
		} else {
			pedals.texSides[i] = newCount == 0 ? defaultSide : pedals.texSides[0];
		}
	}
 }

void Viewer::showPedalsEditor(){

	// Initial window position.
	const ImVec2 & screenSize = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.5f, screenSize.y * 0.1f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
	const float fixedWidth = _guiScale * 300.0f;
	const float fixedHeight = _guiScale * 440.0f;
	ImGui::SetNextWindowSize({fixedWidth, fixedHeight}, ImGuiCond_Always);

	if(ImGui::Begin("Pedal Images Editor", &_showPedalsEditor, ImGuiWindowFlags_NoResize)){
		ImGui::paperBackground();

		bool refreshTextures = false;

		// Header
		ImGui::TextWrapped("Load images to use as masks for each pedal. Overlap between pedals can be tweaked using offsets.");
		{
			if(ImGui::Button("Clear all")){
				_state.pedals.centerImagePath.clear();
				_state.pedals.topImagePath.clear();
				_state.pedals.sideImagePaths.clear();
				refreshTextures = true;
			}
			ImGui::helpTooltip("Remove all images");
			ImGuiSameLine();
			// Just restore the last backup.
			if(ImGui::Button("Reset")){
				_state = _backupState;
				refreshTextures = true;
			}
			ImGui::helpTooltip("Restore the previous images for all pedals");
		}
		ImGui::Separator();

		const float scale = 0.95f;
		const ImVec2 diagSize(scale * fixedWidth, scale * fixedWidth / 1.2f /* existing pedal ratio correction */); 
		const float topHeight = 0.20f;
		const float bottomHeight = 0.72f;
		const float sideWidth = 0.32f;
		const float centerWidth = 0.29f;
		const float topWidth = 0.99f;
		const glm::vec2 sideSize = glm::vec2(sideWidth * diagSize.x, bottomHeight * diagSize.y);
		const glm::vec2 topSize = glm::vec2(topWidth * diagSize.x, topHeight * diagSize.y);
		const glm::vec2 centerSize = glm::vec2(centerWidth * diagSize.x, bottomHeight * diagSize.y);

		// Approximately center buttons
		ImGui::PushID("Top");
		refreshTextures |= drawPedalImageSettings(_state.pedals.texTop, topSize, false, false, _state.pedals.topImagePath, 0, _state.pedals.topColor);
		ImGui::PopID();

		ImGui::PushID("Left");
		refreshTextures |= drawPedalImageSettings(_state.pedals.texSides[0], sideSize, true, false, _state.pedals.sideImagePaths, 0, _state.pedals.leftColor);
		ImGui::PopID();
		ImGuiSameLine(0);

		ImGui::PushID("Center");
		refreshTextures |= drawPedalImageSettings(_state.pedals.texCenter, centerSize, true, false, _state.pedals.centerImagePath, 0, _state.pedals.centerColor);
		ImGui::PopID();
		ImGuiSameLine(0);

		ImGui::PushID("Right");
		refreshTextures |= drawPedalImageSettings(_state.pedals.texSides[1], sideSize, true, _state.pedals.mirror, _state.pedals.sideImagePaths, 1, _state.pedals.rightColor);
		ImGui::PopID();

		ImGui::Separator();

		{
			ImGuiPushItemWidth(ITEM_WIDTH);
			if(ImGui::SliderFloat("##OffsetX", &_state.pedals.margin[0], -0.5f, 0.5f, "Horiz.: %.2f")){
				_state.pedals.margin[0] = glm::clamp(_state.pedals.margin[0], -0.5f, 0.5f);
			}
			ImGui::helpTooltip(s_pedal_img_offset_x_dsc);
			ImGuiSameLine();
			if(ImGui::SliderFloat("##OffsetY", &_state.pedals.margin[1], -0.5f, 0.5f, "Vert.: %.2f")){
				_state.pedals.margin[1] = glm::clamp(_state.pedals.margin[1], -0.5f, 0.5f);
			}
			ImGui::helpTooltip(s_pedal_img_offset_y_dsc);
			ImGui::PopItemWidth();
			ImGuiSameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Offsets");

			ImGui::Checkbox("Mirror right pedal", &_state.pedals.mirror);
			ImGui::helpTooltip(s_pedal_img_mirrored_dsc);
		}
		
		// Actions
		if(!_showPedalsEditor){
			// If we are exiting, refresh the existing set.
			refreshTextures = true;
		}

		// If refresh is needed, ensure that the texture array is up to date.
		if(refreshTextures){
			refreshPedalTextures(_state.pedals);
			// Auto-mirror if only one side pedal image.
			_state.pedals.mirror = _state.pedals.sideImagePaths.size() < 2;
		}
	}
	ImGui::End();
}

void Viewer::applyBackgroundColor(){
	// Clear all buffers with this color.
	glClearColor(_state.background.color[0], _state.background.color[1], _state.background.color[2], _useTransparency ? 0.0f : 1.0f);
	_particlesFramebuffer->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_particlesFramebuffer->unbind();
	_blurFramebuffer0->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_blurFramebuffer0->unbind();
	_blurFramebuffer1->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_blurFramebuffer1->unbind();
	// Update parameter.
	_blurringScreen.program().use();
	_blurringScreen.program().uniform("backgroundColor", _state.background.color);
	glUseProgram(0);
}

void Viewer::applyAllSettings() {
	// Apply all modifications.

	// Adjust tracks count.
	int trackCount = (std::max)(_scene->tracksCount(), int(_state.filter.tracks.size()));
	trackCount = (std::max)(trackCount, 1);
	_state.filter.tracks.resize(trackCount, true);

	// One-shot parameters.
	// TODO: (MV) just apply when needed?
	_renderer.setScaleAndMinorWidth(_state.scale, _state.background.minorsWidth);
	_renderer.setKeyboardSizeAndFadeout(_state.keyboard.size, _state.notes.fadeOut);
	_renderer.setMinorEdgesAndHeight(_state.keyboard.minorEdges, _state.keyboard.minorHeight);
	_renderer.setOrientation(_state.horizontalScroll);

	updateMinMaxKeys();

	// Reset buffers.
	applyBackgroundColor();
	_blurringScreen.program().use();
	_blurringScreen.program().uniform("attenuationFactor", _state.attenuation);
	glUseProgram(0);

	// Resize the framebuffers.
	updateSizes();

	// Finally, restore the track at the beginning.
	reset();
	// All other parameters are directly used at render.
}

void Viewer::clean() {

	// Clean objects.
	_renderer.clean();
	_blurringScreen.clean();
	_passthrough.clean();
	_backgroundTexture.clean();
	_fxaa.clean();
	_particlesFramebuffer->clean();
	_blurFramebuffer0->clean();
	_blurFramebuffer1->clean();
	_finalFramebuffer->clean();
	_renderFramebuffer->clean();
}

void Viewer::rescale(float scale){
	resizeAndRescale(_camera.screenSize()[0], _camera.screenSize()[1], scale);
}

void Viewer::resize(int width, int height) {
	resizeAndRescale(width, height, _camera.scale());
}

void Viewer::resizeAndRescale(int width, int height, float scale) {
	
	_backbufferSize[0] = width;
	_backbufferSize[1] = height;
	// Don't update the rendering buffers if the size is 0, to handle minimizing
	// or if we are currently recording.
	if(width == 0 || height == 0 || _recorder.isRecording()){
		return;
	}

	if (_verbose) {
		std::cout << "[INFO]: Resizing to " << width << " x " << height << std::endl;
	}
	// Update the projection matrix.
	_camera.screen(width, height, scale);
	updateSizes();
}

void Viewer::updateSizes(){
	// Resize the framebuffers.
	const auto &currentQuality = Quality::availables.at(_state.quality);
	const glm::vec2 baseRes(_camera.renderSize());
	_particlesFramebuffer->resize(currentQuality.particlesResolution * baseRes);
	_blurFramebuffer0->resize(currentQuality.blurResolution * baseRes);
	_blurFramebuffer1->resize(currentQuality.blurResolution * baseRes);
	_renderFramebuffer->resize(currentQuality.finalResolution * baseRes);
	_finalFramebuffer->resize(currentQuality.finalResolution * baseRes);
	_recorder.setSize(glm::ivec2(_finalFramebuffer->_width, _finalFramebuffer->_height));
}

void Viewer::keyPressed(int key, int action) {

	if (action == GLFW_PRESS) {
		if (key == GLFW_KEY_P) {
			_shouldPlay = !_shouldPlay;
			_timerStart = DEBUG_SPEED * glfwGetTime() - _timer;
			if(!_shouldPlay){
				if(auto fs = std::dynamic_pointer_cast<MIDISceneFile>(_scene)) fs->silenceAllNotes();
			}
		}
		else if (key == GLFW_KEY_R) {
			reset();
		}
		else if (key == GLFW_KEY_I) {
			_showGUI = !_showGUI;
		}
		else if (key == GLFW_KEY_K) {
			// Recover from stuck live notes without restarting/reconnecting.
			if(auto ls = std::dynamic_pointer_cast<MIDISceneLive>(_scene)) ls->resetStuckNotes();
		}
		else if (key == GLFW_KEY_J) {
			// YouTube-style seek: jump back 10 seconds (file playback only).
			seek(-10.0);
		}
		else if (key == GLFW_KEY_L) {
			// YouTube-style seek: jump forward 10 seconds (file playback only).
			seek(10.0);
		}
		else if (key == GLFW_KEY_D) {
			_showDebug = !_showDebug;
		}
		else if (key == GLFW_KEY_ESCAPE){
			_shouldQuit = 1;
		}
	}
}

void Viewer::reset() {
	_timer = -_state.prerollTime;
	_timerStart = DEBUG_SPEED * glfwGetTime() + (_shouldPlay ? _state.prerollTime : 0.0f);
	_scene->resetParticles();
	if(auto fs = std::dynamic_pointer_cast<MIDISceneFile>(_scene)) fs->silenceAllNotes();
}

void Viewer::seek(double deltaSeconds){
	// Only file playback has a timeline to scrub; live play is ignored.
	auto fs = std::dynamic_pointer_cast<MIDISceneFile>(_scene);
	if(!fs){
		return;
	}
	// The song position handed to the scene is scrollSpeed * _timer, so convert the
	// requested seconds of song time into timer units.
	const double speed = (std::max)(0.01f, _state.scrollSpeed);
	const double songPos = speed * _timer;
	const double newPos = (std::min)((std::max)(songPos + deltaSeconds, 0.0), _scene->duration());
	_timer = newPos / speed;
	// Keep the wall clock consistent whether we are playing or paused.
	_timerStart = DEBUG_SPEED * glfwGetTime() - _timer;
	// Avoid stuck notes on the external synth and drop stale particles across the jump.
	fs->silenceAllNotes();
	_scene->resetParticles();
}

void Viewer::setState(const State & state){
	_state = state;
	_state.setOptions.rebuild();
	_backupState = _state;
	
	// Update toggles.
	_layers[Layer::BGTEXTURE].toggle = &_state.background.image;
	_layers[Layer::BLUR].toggle = &_state.showBlur;
	_layers[Layer::ANNOTATIONS].toggle = &_state.showScore;
	_layers[Layer::KEYBOARD].toggle = &_state.showKeyboard;
	_layers[Layer::PARTICLES].toggle = &_state.showParticles;
	_layers[Layer::NOTES].toggle = &_state.showNotes;
	_layers[Layer::FLASHES].toggle = &_state.showFlashes;
	_layers[Layer::PEDAL].toggle = &_state.showPedal;
	_layers[Layer::WAVE].toggle = &_state.showWave;

	// Update split notes.
	if(_scene){
		_scene->setTranspose(_state.transpose);
		_scene->updateSetsAndVisibleNotes(_state.setOptions, _state.filter);
	}
	applyAllSettings();

	// Textures.
	// Load the background image.
	if(!_state.background.imagePath.empty()){
		glDeleteTextures(1, &_state.background.tex);
		_state.background.tex = loadTexture(_state.background.imagePath[0], 4, false);
		// Don't modify the rest of the potentially restored state.
	}
	// Load notes images
	if(!_state.notes.majorImagePath.empty()){
		glDeleteTextures(1, &_state.notes.majorTex);
		_state.notes.majorTex = loadTexture(_state.notes.majorImagePath[0], 4, false);
	}
	if(!_state.notes.minorImagePath.empty()){
		glDeleteTextures(1, &_state.notes.minorTex);
		_state.notes.minorTex = loadTexture(_state.notes.minorImagePath[0], 4, false);
	}
	if(!_state.flashes.imagePath.empty()){
		if (_state.flashes.tex != ResourcesManager::getTextureFor("flash")) {
			glDeleteTextures(1, &_state.flashes.tex);
		}
		_state.flashes.tex = loadTexture(_state.flashes.imagePath[0], 1, false);
	}

	if(!_state.particles.imagePaths.empty()){
		if (_state.particles.tex != ResourcesManager::getTextureFor("blankarray")) {
			glDeleteTextures(1, &_state.particles.tex);
		}
		// Load new particles.
		_state.particles.tex = loadTextureArray(state.particles.imagePaths, false, _state.particles.texCount);
	}

	refreshPedalTextures(_state.pedals);

	// Don't modify the rest of the potentially restored state.
}

void  Viewer::setGUIScale(float scale){
	_guiScale = (std::max)(0.25f, scale);
	ImGui::GetStyle() = ImGuiStyle();
	ImGui::configureStyle();
	// The font atlas is rasterized at a higher density, compensate for it.
	ImGui::GetIO().FontGlobalScale = _guiScale / ImGui::fontRasterScale();
	ImGui::GetStyle().ScaleAllSizes(_guiScale);
}


void Viewer::updateConfiguration(Configuration& config){
	// Reset
	config.lastMidiPath = "";
	config.lastMidiDevice = "";
	// Remembered MIDI output preference (independent of the active scene).
	config.lastMidiOutput = _midiOutputName;
	// General settings
	config.fullscreen = _fullscreen;
	config.useTransparency = _useTransparency;
	config.guiScale = _guiScale;
	// Settings file.
	config.lastConfigPath = _state.filePath();
	// MIDI File.
	std::shared_ptr<MIDISceneFile> fileScene = std::dynamic_pointer_cast<MIDISceneFile>(_scene);
	if(fileScene){
		config.lastMidiPath = fileScene->filePath();
	}
	// MIDI device.
	std::shared_ptr<MIDISceneLive> liveScene = std::dynamic_pointer_cast<MIDISceneLive>(_scene);
	if(liveScene){
		config.lastMidiDevice = liveScene->deviceName();
	}
}

bool Viewer::startDirectRecording(const Export& exporting, const glm::vec2 & size){
	const bool success = _recorder.setParameters(exporting);
	if(!success){
		std::cerr << "[EXPORT]: Unable to start direct export." << std::endl;
		return false;
	}
	_recorder.setSize(size);
	startRecording();
	_exitAfterRecording = true;
	return true;
}

void Viewer::startRecording(){
	// We need to provide some information for the recorder to start.
	_recorder.prepare(_state.prerollTime, float(_scene->duration()), _state.scrollSpeed);

	// Start by clearing up all buffers.
	// We need:
	// - the rendering res (taking into account quality and screen scaling) to be equal to requiredSize().
	// - the camera screen size and scale to remain the same afterwards (and during for a nice background display).
	// We can do this by leveraging the fact that camera parameters are not used during render.
	// We can thus:
	// - backup the camera parameters and backbuffer resolution
	// - trigger a buffers size update at the target resolution
	// - restore the camera parameters and backbuffer resolution
	// All that will be left is to trigger a size update at the end of the recording.

	const glm::ivec2 backSize = _camera.screenSize();
	const float backScale = _camera.scale();
	const glm::ivec2 backBufferSize = _backbufferSize;

	const auto &currentQuality = Quality::availables.at(_state.quality);
	const glm::vec2 finalSize = glm::vec2(_recorder.requiredSize()) / currentQuality.finalResolution;
	resizeAndRescale(int(finalSize[0]), int(finalSize[1]), 1.0f);

	_camera.screen(backSize[0], backSize[1], backScale);
	_backbufferSize = backBufferSize;

	// Reset buffers.
	glClearColor(_state.background.color[0], _state.background.color[1], _state.background.color[2], _recorder.isTransparent() ? 0.0f : 1.0f);
	_particlesFramebuffer->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_blurFramebuffer0->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_blurFramebuffer1->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_renderFramebuffer->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_finalFramebuffer->bind();
	glClear(GL_COLOR_BUFFER_BIT);
	_finalFramebuffer->unbind();

	_recorder.start(_verbose);
}

bool Viewer::channelColorEdit(const char * name, const char * displayName, ColorArray & colors){
	if(!_state.perSetColors){
		// If locked, display color sink.
		ImGuiPushItemWidth(25);
		const bool inter = ImGui::ColorEdit3(name, &colors[0][0], ImGuiColorEditFlags_NoInputs);
		ImGui::PopItemWidth();

		if(_state.lockParticleColor && ImGui::IsItemHovered()){
			ImGui::SetTooltip("(!) Effect colors\n    are synced");
		}

		if(inter){
			// Ensure synchronization.
			for(size_t cid = 1; cid < colors.size(); ++cid){
				colors[cid] = colors[0];
			}
		}
		return inter;
	}

	// Else, we display a drop down and a popup.
	if(ImGui::ArrowButton(name, ImGuiDir_Down)){
		ImGui::OpenPopup(name);
	}
	ImGuiSameLine(); ImGui::Text("%s", displayName);

	if(ImGui::BeginPopup(name)){
		ImGui::paperBackground();
		// Do 3 columns of color sinks.
		bool edit = false;
		ImGuiPushItemWidth(35);
		for(size_t cid = 0; cid < colors.size(); ++cid){
			const std::string nameC = "Set " + std::to_string(cid);
			edit = ImGui::ColorEdit3(nameC.c_str(), &colors[cid][0], ImGuiColorEditFlags_NoInputs) || edit;
			if(cid % 3 != 2 && cid != colors.size()-1){
				ImGuiSameLine(100 * (cid%3+1));
			}
		}
		ImGui::PopItemWidth();
		ImGui::EndPopup();
		return edit;
	}
	return false;
}


void Viewer::updateMinMaxKeys(){
	// Make sure keys are properly ordered.
	if(_state.minKey > _state.maxKey){
		std::swap(_state.minKey, _state.maxKey);
	}

	// Force edges to align with a major key.
	int realMinKey = _state.minKey;
	if(noteIsMinor[_state.minKey % 12]){
		realMinKey -= 1;
	}

	int realMaxKey = _state.maxKey;
	if(noteIsMinor[_state.maxKey % 12]){
		realMaxKey += 1;
	}

	// Convert to "major" only indices.
	const int minKeyMaj = (_state.minKey/12) * 7 + noteShift[realMinKey % 12];
	const int maxKeyMaj = (_state.maxKey/12) * 7 + noteShift[realMaxKey % 12];
	const int noteCount = (maxKeyMaj - minKeyMaj + 1);

	_renderer.setMinMaxKeys(realMinKey, minKeyMaj, noteCount);
}


void Viewer::ImGuiPushItemWidth(int w){
	ImGui::PushItemWidth(_guiScale * w);
}

void Viewer::ImGuiSameLine(int w){
	ImGui::SameLine(_guiScale * w);
}

void Viewer::guiSeparatorSpacing(){
	ImGui::Spacing();
}
