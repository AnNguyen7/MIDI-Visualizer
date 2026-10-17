#include "ImGuiStyle.h"

#include <gl3w/gl3w.h>
#include <imgui/imgui.h>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

#include "../resources/font_data_Lato.h"

static float s_fontRasterScale = 1.0f;
static ImFont* s_smallFont = nullptr;
static GLuint s_paperTexture = 0;

// Classic palette: ink on parchment, framed by a brass rim.
static ImVec4 hexColor(unsigned int rgb, float alpha = 1.0f){
	return ImVec4(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f, float(rgb & 0xFF) / 255.0f, alpha);
}

static ImVec4 withAlpha(const ImVec4& color, float alpha){
	return ImVec4(color.x, color.y, color.z, alpha);
}

static const ImVec4 kParchment        = hexColor(0xE9DFC8);
static const ImVec4 kParchmentLight   = hexColor(0xF3ECDC);
static const ImVec4 kParchmentLit     = hexColor(0xFBF7EC);
static const ImVec4 kParchmentShade   = hexColor(0xD9CCB0);
static const ImVec4 kParchmentPressed = hexColor(0xC9BA98);
static const ImVec4 kInk              = hexColor(0x1F1B16);
static const ImVec4 kFadedInk         = hexColor(0x6A5F4E);
static const ImVec4 kCharcoal         = hexColor(0x2A2A2C);
static const ImVec4 kCharcoalHover    = hexColor(0x3D3D40);
static const ImVec4 kBrass            = hexColor(0xB8963F);
static const ImVec4 kBrassLight       = hexColor(0xE2CB86);
static const ImVec4 kBrassShadow      = hexColor(0x7A6226);
static const ImVec4 kFeltRed          = hexColor(0x9E2F2F);

static float clampRasterScale(float displayScale){
	return (std::min)((std::max)(displayScale, 1.0f), 4.0f);
}

void ImGui::configureFont(ImFontConfig& font, float displayScale){
	// Rasterize at the display density (and compensate with the global font scale),
	// else glyphs are stretched and blurry on high density screens.
	s_fontRasterScale = clampRasterScale(displayScale);
	// This can be called again when the interface is rescaled.
	ImGui::GetIO().Fonts->Clear();

	font = ImFontConfig();
	font.FontData = (void*)(fontDataLato);
	font.FontDataSize = size_fontDataLato;
	font.SizePixels = std::round(17.0f * s_fontRasterScale);
	font.OversampleH = 2;
	// Font data is static
	font.FontDataOwnedByAtlas = false;

	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.Fonts->AddFont(&font);

	ImFontConfig smallConfig = font;
	smallConfig.SizePixels = std::round(14.0f * s_fontRasterScale);
	s_smallFont = io.Fonts->AddFont(&smallConfig);
}

float ImGui::fontRasterScale(){
	return s_fontRasterScale;
}

bool ImGui::fontNeedsRebuild(float displayScale){
	return std::abs(clampRasterScale(displayScale) - s_fontRasterScale) > 0.01f;
}

ImFont* ImGui::smallFont(){
	return s_smallFont;
}

const ImVec4& ImGui::onDarkTextColor(){
	return kParchmentLight;
}

const ImVec4& ImGui::darkHoverColor(){
	return kCharcoalHover;
}

const ImVec4& ImGui::dangerColor(){
	return kFeltRed;
}

void ImGui::configureStyle(){
	// Everything is ink on parchment: light fields with a thin ink outline, no filled boxes.
	// The only dark blocks are the selected tab and the destructive button.
	ImGui::StyleColorsLight();
	ImGuiStyle & style = ImGui::GetStyle();
	ImVec4* colors = style.Colors;

	const ImVec4 none = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

	// Text / chrome.
	colors[ImGuiCol_Text]                   = kInk;
	colors[ImGuiCol_TextDisabled]           = kFadedInk;
	colors[ImGuiCol_WindowBg]               = kParchment;
	colors[ImGuiCol_ChildBg]                = none;
	colors[ImGuiCol_PopupBg]                = kParchment;
	colors[ImGuiCol_Border]                 = withAlpha(kInk, 0.55f);
	colors[ImGuiCol_BorderShadow]           = none;
	colors[ImGuiCol_MenuBarBg]              = kParchment;
	colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

	// Frames / inputs.
	colors[ImGuiCol_FrameBg]                = kParchmentLight;
	colors[ImGuiCol_FrameBgHovered]         = kParchmentLit;
	colors[ImGuiCol_FrameBgActive]          = kParchmentShade;

	// Title bars.
	colors[ImGuiCol_TitleBg]                = kParchmentShade;
	colors[ImGuiCol_TitleBgActive]          = kParchmentShade;
	colors[ImGuiCol_TitleBgCollapsed]       = kParchmentShade;

	// Scrollbars.
	colors[ImGuiCol_ScrollbarBg]            = none;
	colors[ImGuiCol_ScrollbarGrab]          = withAlpha(kInk, 0.30f);
	colors[ImGuiCol_ScrollbarGrabHovered]   = withAlpha(kInk, 0.45f);
	colors[ImGuiCol_ScrollbarGrabActive]    = withAlpha(kInk, 0.65f);

	// Interactive state.
	colors[ImGuiCol_CheckMark]              = kInk;
	colors[ImGuiCol_SliderGrab]             = kInk;
	colors[ImGuiCol_SliderGrabActive]       = kInk;
	colors[ImGuiCol_Button]                 = kParchmentLight;
	colors[ImGuiCol_ButtonHovered]          = kParchmentLit;
	colors[ImGuiCol_ButtonActive]           = kParchmentPressed;
	colors[ImGuiCol_Header]                 = kParchmentShade;
	colors[ImGuiCol_HeaderHovered]          = kParchmentShade;
	colors[ImGuiCol_HeaderActive]           = kParchmentPressed;
	colors[ImGuiCol_Separator]              = withAlpha(kInk, 0.35f);
	colors[ImGuiCol_SeparatorHovered]       = withAlpha(kInk, 0.60f);
	colors[ImGuiCol_SeparatorActive]        = kInk;
	colors[ImGuiCol_ResizeGrip]             = withAlpha(kInk, 0.20f);
	colors[ImGuiCol_ResizeGripHovered]      = withAlpha(kInk, 0.40f);
	colors[ImGuiCol_ResizeGripActive]       = withAlpha(kInk, 0.60f);

	// Tabs: plain labels, the selected one is a dark block (the viewer switches its text color).
	colors[ImGuiCol_Tab]                    = none;
	colors[ImGuiCol_TabHovered]             = kParchmentShade;
	colors[ImGuiCol_TabActive]              = kCharcoal;
	colors[ImGuiCol_TabUnfocused]           = none;
	colors[ImGuiCol_TabUnfocusedActive]     = kCharcoal;

	// Plots.
	colors[ImGuiCol_PlotLines]              = kInk;
	colors[ImGuiCol_PlotLinesHovered]       = kBrassShadow;
	colors[ImGuiCol_PlotHistogram]          = kInk;
	colors[ImGuiCol_PlotHistogramHovered]   = kBrassShadow;

	// Tables.
	colors[ImGuiCol_TableHeaderBg]          = kParchmentShade;
	colors[ImGuiCol_TableBorderStrong]      = withAlpha(kInk, 0.40f);
	colors[ImGuiCol_TableBorderLight]       = withAlpha(kInk, 0.20f);
	colors[ImGuiCol_TableRowBg]             = none;
	colors[ImGuiCol_TableRowBgAlt]          = withAlpha(kInk, 0.04f);

	// Navigation / selection.
	colors[ImGuiCol_TextSelectedBg]         = withAlpha(kBrassLight, 0.80f);
	colors[ImGuiCol_NavHighlight]           = kInk;
	colors[ImGuiCol_NavWindowingHighlight]  = kInk;
	colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.30f);
	colors[ImGuiCol_DragDropTarget]         = kBrassShadow;

	// Geometry: outlined fields, generous targets.
	style.WindowRounding    = 10.0f;
	style.ChildRounding     = 6.0f;
	style.FrameRounding     = 6.0f;
	style.PopupRounding     = 10.0f;
	style.ScrollbarRounding = 6.0f;
	style.GrabRounding      = 2.0f;
	style.TabRounding       = 5.0f;
	style.WindowBorderSize  = 1.0f;
	style.FrameBorderSize   = 1.0f;
	style.PopupBorderSize   = 1.0f;
	style.TabBorderSize     = 0.0f;
	style.ScrollbarSize     = 10.0f;
	// Diameter of the ring knob of the sliders.
	style.GrabMinSize       = 14.0f;
	style.WindowPadding     = ImVec2(14.0f, 12.0f);
	style.FramePadding      = ImVec2(8.0f, 5.0f);
	style.ItemSpacing       = ImVec2(8.0f, 8.0f);
	style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
	style.IndentSpacing     = 16.0f;
	style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
}

void ImGui::createPaperTexture(){
	if(s_paperTexture != 0){
		return;
	}
	// Tiling grain: a fine speckle and faint diagonal fibres, stored as the opacity of a dark tint.
	const int size = 256;
	uint32_t seed = 0x9E3779B9u;
	auto random = [&seed](){
		seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
		return float(seed & 0xFFFF) / 65535.0f;
	};
	std::vector<float> fibresA(size), fibresB(size);
	for(int i = 0; i < size; ++i){
		fibresA[i] = random();
		fibresB[i] = random();
	}
	std::vector<unsigned char> pixels(size * size * 4);
	for(int y = 0; y < size; ++y){
		for(int x = 0; x < size; ++x){
			// Both combinations wrap around with the texture, so the grain tiles.
			const int ia = (x + 3 * y) & (size - 1);
			const int ib = (2 * x + 5 * y) & (size - 1);
			const float fibreA = 0.5f * (fibresA[ia] + fibresA[(ia + 1) & (size - 1)]);
			const float fibreB = fibresB[ib];
			const float speckle = random();
			const float grain = 0.070f * fibreA * fibreA + 0.035f * fibreB * fibreB + 0.030f * speckle * speckle;
			unsigned char* pixel = &pixels[4 * (y * size + x)];
			pixel[0] = 0x5A; pixel[1] = 0x45; pixel[2] = 0x25;
			pixel[3] = (unsigned char)((std::min)(grain, 1.0f) * 255.0f);
		}
	}
	glGenTextures(1, &s_paperTexture);
	glBindTexture(GL_TEXTURE_2D, s_paperTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void ImGui::paperBackground(){
	const ImVec2 minPos = ImGui::GetWindowPos();
	const ImVec2 size = ImGui::GetWindowSize();
	const ImVec2 maxPos(minPos.x + size.x, minPos.y + size.y);
	const float rounding = ImGui::GetStyle().WindowRounding;
	// Interface unit, following the GUI scale.
	const float unit = ImGui::GetFontSize() / 17.0f;

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	// The window content is clipped inside the padding, the rim sits on the edge.
	drawList->PushClipRect(minPos, maxPos, false);

	if(s_paperTexture != 0){
		// One tile of grain every 128 units.
		const float tile = 128.0f * unit;
		drawList->AddImageRounded((ImTextureID)(uintptr_t)s_paperTexture, minPos, maxPos, ImVec2(0.0f, 0.0f), ImVec2(size.x / tile, size.y / tile), IM_COL32_WHITE, rounding);
	}

	// Brass rim: a band with a highlight on its outer edge and a shadow on its inner edge.
	const float band = 3.0f * unit;
	auto inset = [&](float d, const ImVec4& color, float thickness){
		drawList->AddRect(ImVec2(minPos.x + d, minPos.y + d), ImVec2(maxPos.x - d, maxPos.y - d), ImGui::GetColorU32(color), (std::max)(rounding - d, 0.0f), 0, thickness);
	};
	inset(1.0f + 0.5f * band, kBrass, band);
	inset(1.0f + 0.25f * band, withAlpha(kBrassLight, 0.9f), (std::max)(1.0f, 0.3f * band));
	inset(1.0f + band, withAlpha(kBrassShadow, 0.9f), 1.0f);

	drawList->PopClipRect();
}

bool ImGui::SliderPercent( const char* label, float* v, float v_min, float v_max )
{
	float tmp = ( *v ) * 100.0f;
	const bool press = ImGui::SliderFloat( label, &tmp, v_min * 100.0f, v_max * 100.0f, "%.0f%%" );
	if( press )
	{
		*v = tmp / 100.0f;
	}
	return press;
}

void ImGui::helpTooltip( const char* msg )
{
	if( ImGui::IsItemHovered() )
	{
		ImGui::SetTooltip( "%s", msg );
	}
}