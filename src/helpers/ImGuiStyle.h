#ifndef ImGuiStyle_h
#define ImGuiStyle_h

struct ImFontConfig;
struct ImFont;
struct ImVec4;

namespace ImGui {

/// Load the interface fonts, rasterized for a display with the given pixel density.
void configureFont(ImFontConfig& font, float displayScale);
/// Density the fonts were rasterized at, to be compensated in the global font scale.
float fontRasterScale();
/// Check if the fonts should be rasterized again to stay sharp at the given pixel density.
bool fontNeedsRebuild(float displayScale);
/// Smaller font for secondary text (status line, section titles).
ImFont* smallFont();

void configureStyle();
/// Text color to use on top of the dark blocks (selected tab, destructive button).
const ImVec4& onDarkTextColor();
/// Hovered color of the dark blocks.
const ImVec4& darkHoverColor();
/// Color for destructive confirmations.
const ImVec4& dangerColor();

/// Create the paper grain texture, requires an OpenGL context.
void createPaperTexture();
/// Draw the paper grain and the brass rim of the current window, to call right after Begin.
void paperBackground();

bool SliderPercent( const char* label, float* v, float v_min, float v_max );
void helpTooltip( const char* msg );

}





#endif
