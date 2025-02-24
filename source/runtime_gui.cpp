/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "version.h"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ini_file.hpp"
#include "addon_manager.hpp"
#include "input.hpp"
#include "input_gamepad.hpp"
#include "imgui_widgets.hpp"
#include "localization.hpp"
#include "platform_utils.hpp"
#include "fonts/forkawesome.inl"
#include "fonts/glyph_ranges.hpp"
#include <cmath> // std::abs, std::ceil, std::floor
#include <cctype> // std::tolower
#include <cstdlib> // std::lldiv, std::strtol
#include <cstring> // std::memcmp, std::memcpy
#include <algorithm> // std::any_of, std::count_if, std::find, std::find_if, std::max, std::min, std::replace, std::rotate, std::search, std::swap, std::transform
#ifdef GAME_MW
#include "NFSMW_PreFEngHook.h"
#endif
#ifdef GAME_CARBON
#include "NFSC_PreFEngHook.h"
#endif
#ifdef GAME_UG2
#include "NFSU2_PreFEngHook.h"
#endif
#ifdef GAME_UG
#include "NFSU_PreFEngHook.h"
#endif
#ifdef GAME_PS
#include "NFSPS_PreFEngHook.h"
#endif
#ifdef GAME_UC
#include "NFSUC_PreFEngHook.h"
#endif

extern bool resolve_path(std::filesystem::path &path, std::error_code &ec);

static bool string_contains(const std::string_view text, const std::string_view filter)
{
	return filter.empty() ||
		std::search(text.cbegin(), text.cend(), filter.cbegin(), filter.cend(),
			[](const char c1, const char c2) { // Search case-insensitive
				return (('a' <= c1 && c1 <= 'z') ? static_cast<char>(c1 - ' ') : c1) == (('a' <= c2 && c2 <= 'z') ? static_cast<char>(c2 - ' ') : c2);
			}) != text.cend();
}
static auto is_invalid_path_element(ImGuiInputTextCallbackData *data) -> int
{
	return data->EventChar == L'\"' || data->EventChar == L'*' || data->EventChar == L':' || data->EventChar == L'<' || data->EventChar == L'>' || data->EventChar == L'?' || data->EventChar == L'|';
}
static auto is_invalid_filename_element(ImGuiInputTextCallbackData *data) -> int
{
	// A file name cannot contain any of the following characters
	return is_invalid_path_element(data) || data->EventChar == L'/' || data->EventChar == L'\\';
}

template <typename F>
static void parse_errors(const std::string_view errors, F &&callback)
{
	for (size_t offset = 0, next; offset != std::string_view::npos; offset = next)
	{
		const size_t pos_error = errors.find(": ", offset);
		const size_t pos_error_line = errors.rfind('(', pos_error); // Paths can contain '(', but no ": ", so search backwards from the error location to find the line info
		if (pos_error == std::string_view::npos || pos_error_line == std::string_view::npos || pos_error_line < offset)
			break;

		const size_t pos_linefeed = errors.find('\n', pos_error);

		next = pos_linefeed != std::string_view::npos ? pos_linefeed + 1 : std::string_view::npos;

		const std::string_view error_file = errors.substr(offset, pos_error_line - offset);
		int error_line = static_cast<int>(std::strtol(errors.data() + pos_error_line + 1, nullptr, 10));
		const std::string_view error_text = errors.substr(pos_error + 2 /* skip space */, pos_linefeed - pos_error - 2);

		callback(error_file, error_line, error_text);
	}
}

template <typename T>
static std::string_view get_localized_annotation(T &object, const std::string_view ann_name, [[maybe_unused]] std::string language)
{
#if RESHADE_LOCALIZATION
	if (language.size() >= 2)
	{
		// Transform language name from e.g. 'en-US' to 'en_us'
		std::replace(language.begin(), language.end(), '-', '_');
		std::transform(language.begin(), language.end(), language.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::tolower(c));
			});

		for (int attempt = 0; attempt < 2; ++attempt)
		{
			const std::string_view localized_result = object.annotation_as_string(std::string(ann_name) + '_' + language);
			if (!localized_result.empty())
				return localized_result;
			else if (attempt == 0)
				language.erase(2); // Remove location information from language name, so that it e.g. becomes 'en'
		}
	}
#endif
	return object.annotation_as_string(ann_name);
}

static const ImVec4 COLOR_RED = ImColor(240, 100, 100);
static const ImVec4 COLOR_YELLOW = ImColor(204, 204, 0);

void reshade::runtime::init_gui()
{
	// Default shortcut: Home
	_overlay_key_data[0] = 0x24;
	_overlay_key_data[1] = false;
	_overlay_key_data[2] = false;
	_overlay_key_data[3] = false;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	_imgui_context = ImGui::CreateContext();

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.IniFilename = nullptr;
	imgui_io.ConfigFlags = ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset;

	ImGuiStyle &imgui_style = _imgui_context->Style;
	// Disable rounding by default
	imgui_style.GrabRounding = 0.0f;
	imgui_style.FrameRounding = 0.0f;
	imgui_style.ChildRounding = 0.0f;
	imgui_style.ScrollbarRounding = 0.0f;
	imgui_style.WindowRounding = 0.0f;
	imgui_style.WindowBorderSize = 0.0f;

	// Restore previous context in case this was called from a new runtime being created from an add-on event triggered by an existing runtime
	ImGui::SetCurrentContext(backup_context);
}
void reshade::runtime::deinit_gui()
{
	ImGui::DestroyContext(_imgui_context);
}

void reshade::runtime::build_font_atlas()
{
	ImFontAtlas *const atlas = _imgui_context->IO.Fonts;

	if (atlas->IsBuilt())
		return;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	// Remove any existing fonts from atlas first
	atlas->Clear();

	std::error_code ec;
	const ImWchar *glyph_ranges = nullptr;
	std::filesystem::path resolved_font_path;

#if RESHADE_LOCALIZATION
	std::string language = _selected_language;
	if (language.empty())
		language = resources::get_current_language();

	if (language.find("bg") == 0 || language.find("ru") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesCyrillic();

		_default_font_path = L"C:\\Windows\\Fonts\\calibri.ttf";
	}
	else
	if (language.find("ja") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesJapanese();

		// Morisawa BIZ UDGothic Regular, available since Windows 10 October 2018 Update (1809) Build 17763.1
		_default_font_path = L"C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc";
		if (!std::filesystem::exists(_default_font_path, ec))
			_default_font_path = L"C:\\Windows\\Fonts\\msgothic.ttc"; // MS Gothic
	}
	else
	if (language.find("ko") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesKorean();

		_default_font_path = L"C:\\Windows\\Fonts\\malgun.ttf"; // Malgun Gothic
	}
	else
	if (language.find("zh") == 0)
	{
		glyph_ranges = GetGlyphRangesChineseSimplifiedGB2312();

		_default_font_path = L"C:\\Windows\\Fonts\\msyh.ttc"; // Microsoft YaHei
		if (!std::filesystem::exists(_default_font_path, ec))
			_default_font_path = L"C:\\Windows\\Fonts\\simsun.ttc"; // SimSun
	}
	else
#endif
	{
		glyph_ranges = atlas->GetGlyphRangesDefault();

		_default_font_path.clear();
	}

	const auto add_font_from_file = [atlas](std::filesystem::path &font_path, ImFontConfig cfg, const ImWchar *glyph_ranges, std::error_code &ec) -> bool {
		if (font_path.empty())
			return true;

		if (!resolve_path(font_path, ec))
			return false;

		if (FILE *const file = _wfsopen(font_path.c_str(), L"rb", SH_DENYNO))
		{
			fseek(file, 0, SEEK_END);
			const size_t data_size = ftell(file);
			fseek(file, 0, SEEK_SET);

			void *data = IM_ALLOC(data_size);
			const size_t data_size_read = fread(data, 1, data_size, file);
			fclose(file);
			if (data_size_read != data_size)
			{
				IM_FREE(data);
				return false;
			}

			ImFormatString(cfg.Name, IM_ARRAYSIZE(cfg.Name), "%s, %.0fpx", font_path.stem().u8string().c_str(), cfg.SizePixels);

			return atlas->AddFontFromMemoryTTF(data, static_cast<int>(data_size), cfg.SizePixels, &cfg, glyph_ranges) != nullptr;
		}

		return false;
	};

	ImFontConfig cfg;
	cfg.GlyphOffset.y = std::floor(_font_size / 13.0f); // Not used in AddFontDefault()
	cfg.SizePixels = static_cast<float>(_font_size);

#if RESHADE_LOCALIZATION
	// Add latin font
	resolved_font_path = _latin_font_path;
	if (!_default_font_path.empty())
	{
		if (!add_font_from_file(resolved_font_path, cfg, atlas->GetGlyphRangesDefault(), ec))
		{
			log::message(log::level::error, "Failed to load latin font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);

		cfg.MergeMode = true;
	}
#endif

	// Add main font
	resolved_font_path = _font_path.empty() ? _default_font_path : _font_path;
	{
		if (!add_font_from_file(resolved_font_path, cfg, glyph_ranges, ec))
		{
			log::message(log::level::error, "Failed to load font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		// Use default font if custom font failed to load
		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);

		// Merge icons into main font
		cfg.MergeMode = true;
		cfg.PixelSnapH = true;

		// This need to be static so that it doesn't fall out of scope before the atlas is built below
		static constexpr ImWchar icon_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 }; // Zero-terminated list

		atlas->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_FK, cfg.SizePixels, &cfg, icon_ranges);
	}

	// Add editor font
	resolved_font_path = _editor_font_path.empty() ? _default_editor_font_path : _editor_font_path;
	if (resolved_font_path != _font_path || _editor_font_size != _font_size)
	{
		cfg = ImFontConfig();
		cfg.SizePixels = static_cast<float>(_editor_font_size);

		if (!add_font_from_file(resolved_font_path, cfg, glyph_ranges, ec))
		{
			log::message(log::level::error, "Failed to load editor font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);
	}

	if (atlas->Build())
	{
#if RESHADE_VERBOSE_LOG
		log::message(log::level::debug, "Font atlas size: %dx%d", atlas->TexWidth, atlas->TexHeight);
#endif
	}
	else
	{
		log::message(log::level::error, "Failed to build font atlas!");

		_font_path.clear();
		_latin_font_path.clear();
		_editor_font_path.clear();

		atlas->Clear();

		// If unable to build font atlas due to an invalid custom font, revert to the default font
		for (int i = 0; i < (_editor_font_size != _font_size ? 2 : 1); ++i)
		{
			cfg = ImFontConfig();
			cfg.SizePixels = static_cast<float>(i == 0 ? _font_size : _editor_font_size);

			atlas->AddFontDefault(&cfg);
		}
	}

	ImGui::SetCurrentContext(backup_context);

	_show_splash = true;

	int width, height;
	unsigned char *pixels;
	// This will also build the font atlas again if that previously failed above
	atlas->GetTexDataAsRGBA32(&pixels, &width, &height);

	// Make sure font atlas is not currently in use before destroying it
	_graphics_queue->wait_idle();

	_device->destroy_resource(_font_atlas_tex);
	_font_atlas_tex = {};
	_device->destroy_resource_view(_font_atlas_srv);
	_font_atlas_srv = {};

	const api::subresource_data initial_data = { pixels, static_cast<uint32_t>(width * 4), static_cast<uint32_t>(width * height * 4) };

	// Create font atlas texture and upload it
	if (!_device->create_resource(
			api::resource_desc(width, height, 1, 1, api::format::r8g8b8a8_unorm, 1, api::memory_heap::gpu_only, api::resource_usage::shader_resource),
			&initial_data, api::resource_usage::shader_resource, &_font_atlas_tex))
	{
		log::message(log::level::error, "Failed to create front atlas resource!");
		return;
	}

	// Texture data is now uploaded, so can free the memory
	atlas->ClearTexData();

	if (!_device->create_resource_view(_font_atlas_tex, api::resource_usage::shader_resource, api::resource_view_desc(api::format::r8g8b8a8_unorm), &_font_atlas_srv))
	{
		log::message(log::level::error, "Failed to create font atlas resource view!");
		return;
	}

	_device->set_resource_name(_font_atlas_tex, "ImGui font atlas");
}

void reshade::runtime::load_config_gui(const ini_file &config)
{
	if (_input_gamepad != nullptr)
		_imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	else
		_imgui_context->IO.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

	const auto config_get = [&config](const std::string &section, const std::string &key, auto &values) {
		if (config.get(section, key, values))
			return true;
		// Fall back to global configuration when an entry does not exist in the local configuration
		return global_config().get(section, key, values);
	};

	config_get("INPUT", "KeyOverlay", _overlay_key_data);
	config_get("INPUT", "KeyFPS", _fps_key_data);
	config_get("INPUT", "KeyFrameTime", _frametime_key_data);
	config_get("INPUT", "InputProcessing", _input_processing_mode);
	config.get("INPUT", "NFSToggleFrontend", _toggle_fe_key_data);

#if RESHADE_LOCALIZATION
	config_get("OVERLAY", "Language", _selected_language);
#endif

	config.get("OVERLAY", "ClockFormat", _clock_format);
	config.get("OVERLAY", "FPSPosition", _fps_pos);
	config.get("OVERLAY", "NoFontScaling", _no_font_scaling);
	config.get("OVERLAY", "ShowClock", _show_clock);
#if RESHADE_FX
	config.get("OVERLAY", "ShowForceLoadEffectsButton", _show_force_load_effects_button);
#endif
	config.get("OVERLAY", "ShowFPS", _show_fps);
	config.get("OVERLAY", "ShowFrameTime", _show_frametime);
	config.get("OVERLAY", "ShowPresetName", _show_preset_name);
	config.get("OVERLAY", "ShowScreenshotMessage", _show_screenshot_message);
#if RESHADE_FX
	if (!global_config().get("OVERLAY", "TutorialProgress", _tutorial_index))
		config.get("OVERLAY", "TutorialProgress", _tutorial_index);
	config.get("OVERLAY", "VariableListHeight", _variable_editor_height);
	config.get("OVERLAY", "VariableListUseTabs", _variable_editor_tabs);
	config.get("OVERLAY", "AutoSavePreset", _auto_save_preset);
	config.get("OVERLAY", "ShowPresetTransitionMessage", _show_preset_transition_message);
#endif

	ImGuiStyle &imgui_style = _imgui_context->Style;
	config.get("STYLE", "Alpha", imgui_style.Alpha);
	config.get("STYLE", "ChildRounding", imgui_style.ChildRounding);
	config.get("STYLE", "ColFPSText", _fps_col);
	config.get("STYLE", "EditorFont", _editor_font_path);
	config.get("STYLE", "EditorFontSize", _editor_font_size);
	config.get("STYLE", "EditorStyleIndex", _editor_style_index);
	config.get("STYLE", "Font", _font_path);
	config.get("STYLE", "FontSize", _font_size);
	config.get("STYLE", "FPSScale", _fps_scale);
	config.get("STYLE", "FrameRounding", imgui_style.FrameRounding);
	config.get("STYLE", "GrabRounding", imgui_style.GrabRounding);
	config.get("STYLE", "LatinFont", _latin_font_path);
	config.get("STYLE", "PopupRounding", imgui_style.PopupRounding);
	config.get("STYLE", "ScrollbarRounding", imgui_style.ScrollbarRounding);
	config.get("STYLE", "StyleIndex", _style_index);
	config.get("STYLE", "TabRounding", imgui_style.TabRounding);
	config.get("STYLE", "WindowRounding", imgui_style.WindowRounding);
	config.get("STYLE", "HdrOverlayBrightness", _hdr_overlay_brightness);
	config.get("STYLE", "HdrOverlayOverwriteColorSpaceTo", reinterpret_cast<int &>(_hdr_overlay_overwrite_color_space));

	// For compatibility with older versions, set the alpha value if it is missing
	if (_fps_col[3] == 0.0f)
		_fps_col[3]  = 1.0f;

	load_custom_style();

	if (_imgui_context->SettingsLoaded)
		return;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	// Call all pre-read handlers, before reading config data (since they affect state that is then updated in the read handlers below)
	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
		if (handler.ReadInitFn)
			handler.ReadInitFn(_imgui_context, &handler);

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
	{
		if (std::vector<std::string> lines;
			config.get("OVERLAY", handler.TypeName, lines))
		{
			void *entry_data = nullptr;

			for (const std::string &line : lines)
			{
				if (line.empty())
					continue;

				if (line[0] == '[')
				{
					const size_t name_beg = line.find('[', 1) + 1;
					const size_t name_end = line.rfind(']');

					entry_data = handler.ReadOpenFn(_imgui_context, &handler, line.substr(name_beg, name_end - name_beg).c_str());
				}
				else
				{
					assert(entry_data != nullptr);
					handler.ReadLineFn(_imgui_context, &handler, entry_data, line.c_str());
				}
			}
		}
	}

	_imgui_context->SettingsLoaded = true;

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
		if (handler.ApplyAllFn)
			handler.ApplyAllFn(_imgui_context, &handler);

	ImGui::SetCurrentContext(backup_context);
}
void reshade::runtime::save_config_gui(ini_file &config) const
{
	config.set("INPUT", "KeyOverlay", _overlay_key_data);
	config.set("INPUT", "KeyFPS", _fps_key_data);
	config.set("INPUT", "KeyFrametime", _frametime_key_data);
	config.set("INPUT", "InputProcessing", _input_processing_mode);
	config.set("INPUT", "NFSToggleFrontend", _toggle_fe_key_data);

#if RESHADE_LOCALIZATION
	config.set("OVERLAY", "Language", _selected_language);
#endif

	config.set("OVERLAY", "ClockFormat", _clock_format);
	config.set("OVERLAY", "FPSPosition", _fps_pos);
	config.set("OVERLAY", "ShowClock", _show_clock);
#if RESHADE_FX
	config.set("OVERLAY", "ShowForceLoadEffectsButton", _show_force_load_effects_button);
#endif
	config.set("OVERLAY", "ShowFPS", _show_fps);
	config.set("OVERLAY", "ShowFrameTime", _show_frametime);
	config.set("OVERLAY", "ShowPresetName", _show_preset_name);
	config.set("OVERLAY", "ShowScreenshotMessage", _show_screenshot_message);
#if RESHADE_FX
	global_config().set("OVERLAY", "TutorialProgress", _tutorial_index);
	config.set("OVERLAY", "TutorialProgress", _tutorial_index);
	config.set("OVERLAY", "VariableListHeight", _variable_editor_height);
	config.set("OVERLAY", "VariableListUseTabs", _variable_editor_tabs);
	config.set("OVERLAY", "AutoSavePreset", _auto_save_preset);
	config.set("OVERLAY", "ShowPresetTransitionMessage", _show_preset_transition_message);
#endif

	const ImGuiStyle &imgui_style = _imgui_context->Style;
	config.set("STYLE", "Alpha", imgui_style.Alpha);
	config.set("STYLE", "ChildRounding", imgui_style.ChildRounding);
	config.set("STYLE", "ColFPSText", _fps_col);
	config.set("STYLE", "EditorFont", _editor_font_path);
	config.set("STYLE", "EditorFontSize", _editor_font_size);
	config.set("STYLE", "EditorStyleIndex", _editor_style_index);
	config.set("STYLE", "Font", _font_path);
	config.set("STYLE", "FontSize", _font_size);
	config.set("STYLE", "FPSScale", _fps_scale);
	config.set("STYLE", "FrameRounding", imgui_style.FrameRounding);
	config.set("STYLE", "GrabRounding", imgui_style.GrabRounding);
	config.set("STYLE", "LatinFont", _latin_font_path);
	config.set("STYLE", "PopupRounding", imgui_style.PopupRounding);
	config.set("STYLE", "ScrollbarRounding", imgui_style.ScrollbarRounding);
	config.set("STYLE", "StyleIndex", _style_index);
	config.set("STYLE", "TabRounding", imgui_style.TabRounding);
	config.set("STYLE", "WindowRounding", imgui_style.WindowRounding);
	config.set("STYLE", "HdrOverlayBrightness", _hdr_overlay_brightness);
	config.set("STYLE", "HdrOverlayOverwriteColorSpaceTo", static_cast<int>(_hdr_overlay_overwrite_color_space));

	// Do not save custom style colors by default, only when actually used and edited

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
	{
		ImGuiTextBuffer buffer;
		handler.WriteAllFn(_imgui_context, &handler, &buffer);

		std::vector<std::string> lines;
		for (int i = 0, offset = 0; i < buffer.size(); ++i)
		{
			if (buffer[i] == '\n')
			{
				lines.emplace_back(buffer.c_str() + offset, i - offset);
				offset = i + 1;
			}
		}

		if (!lines.empty())
			config.set("OVERLAY", handler.TypeName, lines);
	}

	ImGui::SetCurrentContext(backup_context);
}

void reshade::runtime::load_custom_style()
{
	const ini_file &config = ini_file::load_cache(_config_path);

	ImVec4 *const colors = _imgui_context->Style.Colors;
	switch (_style_index)
	{
	case 0:
		ImGui::StyleColorsDark(&_imgui_context->Style);
		break;
	case 1:
		ImGui::StyleColorsLight(&_imgui_context->Style);
		break;
	case 2:
		colors[ImGuiCol_Text] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.58f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.00f);
		colors[ImGuiCol_Border] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.30f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.470588f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.588235f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.45f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.35f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.58f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.57f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.31f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 0.92f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.80f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.784314f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Button] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.44f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Header] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.76f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Separator] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.32f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.78f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.20f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Tab] = colors[ImGuiCol_Button];
		colors[ImGuiCol_TabSelected] = colors[ImGuiCol_ButtonActive];
		colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_ButtonActive];
		colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
		colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
		colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
		colors[ImGuiCol_TabDimmedSelectedOverline] = colors[ImGuiCol_TabDimmedSelected];
		colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_Header] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.43f);
		break;
	case 5:
		colors[ImGuiCol_Text] = ImColor(0xff969483);
		colors[ImGuiCol_TextDisabled] = ImColor(0xff756e58);
		colors[ImGuiCol_WindowBg] = ImColor(0xff362b00);
		colors[ImGuiCol_ChildBg] = ImColor();
		colors[ImGuiCol_PopupBg] = ImColor(0xfc362b00); // Customized
		colors[ImGuiCol_Border] = ImColor(0xff423607);
		colors[ImGuiCol_BorderShadow] = ImColor();
		colors[ImGuiCol_FrameBg] = ImColor(0xfc423607); // Customized
		colors[ImGuiCol_FrameBgHovered] = ImColor(0xff423607);
		colors[ImGuiCol_FrameBgActive] = ImColor(0xff423607);
		colors[ImGuiCol_TitleBg] = ImColor(0xff362b00);
		colors[ImGuiCol_TitleBgActive] = ImColor(0xff362b00);
		colors[ImGuiCol_TitleBgCollapsed] = ImColor(0xff362b00);
		colors[ImGuiCol_MenuBarBg] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarBg] = ImColor(0xff362b00);
		colors[ImGuiCol_ScrollbarGrab] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarGrabActive] = ImColor(0xff423607);
		colors[ImGuiCol_CheckMark] = ImColor(0xff756e58);
		colors[ImGuiCol_SliderGrab] = ImColor(0xff5e5025); // Customized
		colors[ImGuiCol_SliderGrabActive] = ImColor(0xff5e5025); // Customized
		colors[ImGuiCol_Button] = ImColor(0xff423607);
		colors[ImGuiCol_ButtonHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ButtonActive] = ImColor(0xff362b00);
		colors[ImGuiCol_Header] = ImColor(0xff423607);
		colors[ImGuiCol_HeaderHovered] = ImColor(0xff423607);
		colors[ImGuiCol_HeaderActive] = ImColor(0xff423607);
		colors[ImGuiCol_Separator] = ImColor(0xff423607);
		colors[ImGuiCol_SeparatorHovered] = ImColor(0xff423607);
		colors[ImGuiCol_SeparatorActive] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGrip] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGripHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGripActive] = ImColor(0xff756e58);
		colors[ImGuiCol_Tab] = ImColor(0xff362b00);
		colors[ImGuiCol_TabHovered] = ImColor(0xff423607);
		colors[ImGuiCol_TabSelected] = ImColor(0xff423607);
		colors[ImGuiCol_TabSelectedOverline] = ImColor(0xff423607);
		colors[ImGuiCol_TabDimmed] = ImColor(0xff362b00);
		colors[ImGuiCol_TabDimmedSelected] = ImColor(0xff423607);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImColor(0xff423607);
		colors[ImGuiCol_DockingPreview] = ImColor(0xee837b65); // Customized
		colors[ImGuiCol_DockingEmptyBg] = ImColor();
		colors[ImGuiCol_PlotLines] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotLinesHovered] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotHistogram] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotHistogramHovered] = ImColor(0xff756e58);
		colors[ImGuiCol_TextSelectedBg] = ImColor(0xff756e58);
		colors[ImGuiCol_DragDropTarget] = ImColor(0xff756e58);
		colors[ImGuiCol_NavCursor] = ImColor();
		colors[ImGuiCol_NavWindowingHighlight] = ImColor(0xee969483); // Customized
		colors[ImGuiCol_NavWindowingDimBg] = ImColor(0x20e3f6fd); // Customized
		colors[ImGuiCol_ModalWindowDimBg] = ImColor(0x20e3f6fd); // Customized
		break;
	case 6:
		colors[ImGuiCol_Text] = ImColor(0xff837b65);
		colors[ImGuiCol_TextDisabled] = ImColor(0xffa1a193);
		colors[ImGuiCol_WindowBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_ChildBg] = ImColor();
		colors[ImGuiCol_PopupBg] = ImColor(0xfce3f6fd); // Customized
		colors[ImGuiCol_Border] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_BorderShadow] = ImColor();
		colors[ImGuiCol_FrameBg] = ImColor(0xfcd5e8ee); // Customized
		colors[ImGuiCol_FrameBgHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_FrameBgActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TitleBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TitleBgActive] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TitleBgCollapsed] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_MenuBarBg] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_ScrollbarGrab] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarGrabActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_CheckMark] = ImColor(0xffa1a193);
		colors[ImGuiCol_SliderGrab] = ImColor(0xffc3d3d9); // Customized
		colors[ImGuiCol_SliderGrabActive] = ImColor(0xffc3d3d9); // Customized
		colors[ImGuiCol_Button] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ButtonHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ButtonActive] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_Header] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_HeaderHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_HeaderActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_Separator] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_SeparatorHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_SeparatorActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGrip] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGripHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGripActive] = ImColor(0xffa1a193);
		colors[ImGuiCol_Tab] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TabHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabSelected] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabSelectedOverline] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabDimmed] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TabDimmedSelected] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_DockingPreview] = ImColor(0xeea1a193); // Customized
		colors[ImGuiCol_DockingEmptyBg] = ImColor();
		colors[ImGuiCol_PlotLines] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotLinesHovered] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotHistogram] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotHistogramHovered] = ImColor(0xffa1a193);
		colors[ImGuiCol_TextSelectedBg] = ImColor(0xffa1a193);
		colors[ImGuiCol_DragDropTarget] = ImColor(0xffa1a193);
		colors[ImGuiCol_NavCursor] = ImColor();
		colors[ImGuiCol_NavWindowingHighlight] = ImColor(0xee837b65); // Customized
		colors[ImGuiCol_NavWindowingDimBg] = ImColor(0x20362b00); // Customized
		colors[ImGuiCol_ModalWindowDimBg] = ImColor(0x20362b00); // Customized
		break;
	default:
		for (ImGuiCol i = 0; i < ImGuiCol_COUNT; i++)
			config.get("STYLE", ImGui::GetStyleColorName(i), (float(&)[4])colors[i]);
		break;
	}

	switch (_editor_style_index)
	{
	case 0: // Dark
		_editor_palette[imgui::code_editor::color_default] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_keyword] = 0xffd69c56;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff00ff00;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff7070e0;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff409090;
		_editor_palette[imgui::code_editor::color_identifier] = 0xffaaaaaa;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff9bc64d;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc040a0;
		_editor_palette[imgui::code_editor::color_comment] = 0xff206020;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff406020;
		_editor_palette[imgui::code_editor::color_background] = 0xff101010;
		_editor_palette[imgui::code_editor::color_cursor] = 0xffe0e0e0;
		_editor_palette[imgui::code_editor::color_selection] = 0x80a06020;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x800020ff;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x8000ffff;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff707000;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x40000000;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x40808080;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x40a0a0a0;
		break;
	case 1: // Light
		_editor_palette[imgui::code_editor::color_default] = 0xff000000;
		_editor_palette[imgui::code_editor::color_keyword] = 0xffff0c06;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff008000;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff2020a0;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff000000;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff409090;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff404040;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff606010;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc040a0;
		_editor_palette[imgui::code_editor::color_comment] = 0xff205020;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff405020;
		_editor_palette[imgui::code_editor::color_background] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff000000;
		_editor_palette[imgui::code_editor::color_selection] = 0x80600000;
		_editor_palette[imgui::code_editor::color_error_marker] = 0xa00010ff;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x8000ffff;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff505000;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x40000000;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x40808080;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x40000000;
		break;
	case 3: // Solarized Dark
		_editor_palette[imgui::code_editor::color_default] = 0xff969483;
		_editor_palette[imgui::code_editor::color_keyword] = 0xff0089b5;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff969483;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff164bcb;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff969483;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff969483;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc4716c;
		_editor_palette[imgui::code_editor::color_comment] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_background] = 0xff362b00;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff969483;
		_editor_palette[imgui::code_editor::color_selection] = 0xa0756e58;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x7f2f32dc;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x7f0089b5;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x7f423607;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x7f423607;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x7f423607;
		break;
	case 4: // Solarized Light
		_editor_palette[imgui::code_editor::color_default] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_keyword] = 0xff0089b5;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff164bcb;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc4716c;
		_editor_palette[imgui::code_editor::color_comment] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_background] = 0xffe3f6fd;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_selection] = 0x60a1a193;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x7f2f32dc;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x7f0089b5;
		_editor_palette[imgui::code_editor::color_line_number] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x7fd5e8ee;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x7fd5e8ee;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x7fd5e8ee;
		break;
	case 2:
	default:
		ImVec4 value;
		for (ImGuiCol i = 0; i < imgui::code_editor::color_palette_max; i++)
			value = ImGui::ColorConvertU32ToFloat4(_editor_palette[i]), // Get default value first
			config.get("STYLE",  imgui::code_editor::get_palette_color_name(i), (float(&)[4])value),
			_editor_palette[i] = ImGui::ColorConvertFloat4ToU32(value);
		break;
	}
}
void reshade::runtime::save_custom_style() const
{
	ini_file &config = ini_file::load_cache(_config_path);

	if (_style_index == 3 || _style_index == 4) // Custom Simple, Custom Advanced
	{
		for (ImGuiCol i = 0; i < ImGuiCol_COUNT; i++)
			config.set("STYLE", ImGui::GetStyleColorName(i), (const float(&)[4])_imgui_context->Style.Colors[i]);
	}

	if (_editor_style_index == 2) // Custom
	{
		ImVec4 value;
		for (ImGuiCol i = 0; i < imgui::code_editor::color_palette_max; i++)
			value = ImGui::ColorConvertU32ToFloat4(_editor_palette[i]),
			config.set("STYLE",  imgui::code_editor::get_palette_color_name(i), (const float(&)[4])value);
	}
}

void reshade::runtime::draw_gui()
{
	assert(_is_initialized);

	bool show_overlay = _show_overlay;
	api::input_source show_overlay_source = api::input_source::keyboard;

	if (_input != nullptr)
	{
		if (_show_overlay && !_ignore_shortcuts && !_imgui_context->IO.NavVisible && _input->is_key_pressed(0x1B /* VK_ESCAPE */) &&
			((_input_processing_mode == 2 || (_input_processing_mode == 1 && (_input->is_blocking_any_mouse_input() || _input->is_blocking_any_keyboard_input())))))
			show_overlay = false; // Close when pressing the escape button and not currently navigating with the keyboard
		else if (!_ignore_shortcuts && _input->is_key_pressed(_overlay_key_data, _force_shortcut_modifiers) && _imgui_context->ActiveId == 0)
			show_overlay = !_show_overlay;

		if (!_ignore_shortcuts)
		{
			if (_input->is_key_pressed(_fps_key_data, _force_shortcut_modifiers))
				_show_fps = _show_fps ? 0 : 1;
			if (_input->is_key_pressed(_frametime_key_data, _force_shortcut_modifiers))
				_show_frametime = _show_frametime ? 0 : 1;
		}
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_button_down(input_gamepad::button_left_shoulder) &&
			_input_gamepad->is_button_down(input_gamepad::button_right_shoulder) &&
			_input_gamepad->is_button_pressed(input_gamepad::button_start))
		{
			show_overlay = !_show_overlay;
			show_overlay_source = api::input_source::gamepad;
		}
	}

	if (show_overlay != _show_overlay)
		open_overlay(show_overlay, show_overlay_source);

#if RESHADE_FX
	const bool show_splash_window = _show_splash && (is_loading() || (_reload_count <= 1 && (_last_present_time - _last_reload_time) < std::chrono::seconds(5)) || (!_show_overlay && _tutorial_index == 0 && _input != nullptr));
#else
	const bool show_splash_window = _show_splash && (_last_present_time - _last_reload_time) < std::chrono::seconds(5);
#endif

	// Do not show this message in the same frame the screenshot is taken (so that it won't show up on the GUI screenshot)
	const bool show_screenshot_message = (_show_screenshot_message || !_last_screenshot_save_successful) && !_should_save_screenshot && (_last_present_time - _last_screenshot_time) < std::chrono::seconds(_last_screenshot_save_successful ? 3 : 5);
#if RESHADE_FX
	const bool show_preset_transition_message = _show_preset_transition_message && _is_in_preset_transition;
#else
	const bool show_preset_transition_message = false;
#endif
	const bool show_message_window = show_screenshot_message || show_preset_transition_message || !_preset_save_successful;

	const bool show_clock = _show_clock == 1 || (_show_overlay && _show_clock > 1);
	const bool show_fps = _show_fps == 1 || (_show_overlay && _show_fps > 1);
	const bool show_frametime = _show_frametime == 1 || (_show_overlay && _show_frametime > 1);
	const bool show_preset_name = _show_preset_name == 1 || (_show_overlay && _show_preset_name > 1);
	bool show_statistics_window = show_clock || show_fps || show_frametime || show_preset_name;
#if RESHADE_ADDON
	for (const addon_info &info : addon_loaded_info)
	{
		for (const addon_info::overlay_callback &widget : info.overlay_callbacks)
		{
			if (widget.title == "OSD")
			{
				show_statistics_window = true;
				break;
			}
		}
	}
#endif

	_ignore_shortcuts = false;
	_block_input_next_frame = false;
#if RESHADE_FX
	_gather_gpu_statistics = false;
	_effects_expanded_state &= 2;
#endif

	if (!show_splash_window && !show_message_window && !show_statistics_window && !_show_overlay
#if RESHADE_FX
		&& _preview_texture == 0
#endif
#if RESHADE_ADDON
		&& !has_addon_event<addon_event::reshade_overlay>()
#endif
		)
	{
		if (_input != nullptr)
		{
			_input->block_mouse_input(false);
			_input->block_keyboard_input(false);
			_input->block_mouse_cursor_warping(false);
		}
		return; // Early-out to avoid costly ImGui calls when no GUI elements are on the screen
	}

	build_font_atlas();
	if (_font_atlas_srv == 0)
		return; // Cannot render GUI without font atlas

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.DeltaTime = _last_frame_duration.count() * 1e-9f;
	imgui_io.DisplaySize.x = static_cast<float>(_width);
	imgui_io.DisplaySize.y = static_cast<float>(_height);
	imgui_io.Fonts->TexID = _font_atlas_srv.handle;

	if (_input != nullptr)
	{
		imgui_io.MouseDrawCursor = _show_overlay && (!_should_save_screenshot || !_screenshot_save_ui);

		// Scale mouse position in case render resolution does not match the window size
		unsigned int max_position[2];
		_input->max_mouse_position(max_position);
		imgui_io.AddMousePosEvent(
			_input->mouse_position_x() * (imgui_io.DisplaySize.x / max_position[0]),
			_input->mouse_position_y() * (imgui_io.DisplaySize.y / max_position[1]));

		// Add wheel delta to the current absolute mouse wheel position
		imgui_io.AddMouseWheelEvent(0.0f, _input->mouse_wheel_delta());

		// Update all the button states
		constexpr std::pair<ImGuiKey, unsigned int> key_mappings[] = {
			{ ImGuiKey_Tab, 0x09 /* VK_TAB */ },
			{ ImGuiKey_LeftArrow, 0x25 /* VK_LEFT */ },
			{ ImGuiKey_RightArrow, 0x27 /* VK_RIGHT */ },
			{ ImGuiKey_UpArrow, 0x26 /* VK_UP */ },
			{ ImGuiKey_DownArrow, 0x28 /* VK_DOWN */ },
			{ ImGuiKey_PageUp, 0x21 /* VK_PRIOR */ },
			{ ImGuiKey_PageDown, 0x22 /* VK_NEXT */ },
			{ ImGuiKey_End, 0x23 /* VK_END */ },
			{ ImGuiKey_Home, 0x24 /* VK_HOME */ },
			{ ImGuiKey_Insert, 0x2D /* VK_INSERT */ },
			{ ImGuiKey_Delete, 0x2E /* VK_DELETE */ },
			{ ImGuiKey_Backspace, 0x08 /* VK_BACK */ },
			{ ImGuiKey_Space, 0x20 /* VK_SPACE */ },
			{ ImGuiKey_Enter, 0x0D /* VK_RETURN */ },
			{ ImGuiKey_Escape, 0x1B /* VK_ESCAPE */ },
			{ ImGuiKey_LeftCtrl, 0xA2 /* VK_LCONTROL */ },
			{ ImGuiKey_LeftShift, 0xA0 /* VK_LSHIFT */ },
			{ ImGuiKey_LeftAlt, 0xA4 /* VK_LMENU */ },
			{ ImGuiKey_LeftSuper, 0x5B /* VK_LWIN */ },
			{ ImGuiKey_RightCtrl, 0xA3 /* VK_RCONTROL */ },
			{ ImGuiKey_RightShift, 0xA1 /* VK_RSHIFT */ },
			{ ImGuiKey_RightAlt, 0xA5 /* VK_RMENU */ },
			{ ImGuiKey_RightSuper, 0x5C /* VK_RWIN */ },
			{ ImGuiKey_Menu, 0x5D /* VK_APPS */ },
			{ ImGuiKey_0, '0' },
			{ ImGuiKey_1, '1' },
			{ ImGuiKey_2, '2' },
			{ ImGuiKey_3, '3' },
			{ ImGuiKey_4, '4' },
			{ ImGuiKey_5, '5' },
			{ ImGuiKey_6, '6' },
			{ ImGuiKey_7, '7' },
			{ ImGuiKey_8, '8' },
			{ ImGuiKey_9, '9' },
			{ ImGuiKey_A, 'A' },
			{ ImGuiKey_B, 'B' },
			{ ImGuiKey_C, 'C' },
			{ ImGuiKey_D, 'D' },
			{ ImGuiKey_E, 'E' },
			{ ImGuiKey_F, 'F' },
			{ ImGuiKey_G, 'G' },
			{ ImGuiKey_H, 'H' },
			{ ImGuiKey_I, 'I' },
			{ ImGuiKey_J, 'J' },
			{ ImGuiKey_K, 'K' },
			{ ImGuiKey_L, 'L' },
			{ ImGuiKey_M, 'M' },
			{ ImGuiKey_N, 'N' },
			{ ImGuiKey_O, 'O' },
			{ ImGuiKey_P, 'P' },
			{ ImGuiKey_Q, 'Q' },
			{ ImGuiKey_R, 'R' },
			{ ImGuiKey_S, 'S' },
			{ ImGuiKey_T, 'T' },
			{ ImGuiKey_U, 'U' },
			{ ImGuiKey_V, 'V' },
			{ ImGuiKey_W, 'W' },
			{ ImGuiKey_X, 'X' },
			{ ImGuiKey_Y, 'Y' },
			{ ImGuiKey_Z, 'Z' },
			{ ImGuiKey_F1, 0x70 /* VK_F1 */ },
			{ ImGuiKey_F2, 0x71 /* VK_F2 */ },
			{ ImGuiKey_F3, 0x72 /* VK_F3 */ },
			{ ImGuiKey_F4, 0x73 /* VK_F4 */ },
			{ ImGuiKey_F5, 0x74 /* VK_F5 */ },
			{ ImGuiKey_F6, 0x75 /* VK_F6 */ },
			{ ImGuiKey_F7, 0x76 /* VK_F7 */ },
			{ ImGuiKey_F8, 0x77 /* VK_F8 */ },
			{ ImGuiKey_F9, 0x78 /* VK_F9 */ },
			{ ImGuiKey_F10, 0x79 /* VK_F10 */ },
			{ ImGuiKey_F11, 0x80 /* VK_F11 */ },
			{ ImGuiKey_F12, 0x81 /* VK_F12 */ },
			{ ImGuiKey_Apostrophe, 0xDE /* VK_OEM_7 */ },
			{ ImGuiKey_Comma, 0xBC /* VK_OEM_COMMA */ },
			{ ImGuiKey_Minus, 0xBD /* VK_OEM_MINUS */ },
			{ ImGuiKey_Period, 0xBE /* VK_OEM_PERIOD */ },
			{ ImGuiKey_Slash, 0xBF /* VK_OEM_2 */ },
			{ ImGuiKey_Semicolon, 0xBA /* VK_OEM_1 */ },
			{ ImGuiKey_Equal, 0xBB /* VK_OEM_PLUS */ },
			{ ImGuiKey_LeftBracket, 0xDB /* VK_OEM_4 */ },
			{ ImGuiKey_Backslash, 0xDC /* VK_OEM_5 */ },
			{ ImGuiKey_RightBracket, 0xDD /* VK_OEM_6 */ },
			{ ImGuiKey_GraveAccent, 0xC0 /* VK_OEM_3 */ },
			{ ImGuiKey_CapsLock, 0x14 /* VK_CAPITAL */ },
			{ ImGuiKey_ScrollLock, 0x91 /* VK_SCROLL */ },
			{ ImGuiKey_NumLock, 0x90 /* VK_NUMLOCK */ },
			{ ImGuiKey_PrintScreen, 0x2C /* VK_SNAPSHOT */ },
			{ ImGuiKey_Pause, 0x13 /* VK_PAUSE */ },
			{ ImGuiKey_Keypad0, 0x60 /* VK_NUMPAD0 */ },
			{ ImGuiKey_Keypad1, 0x61 /* VK_NUMPAD1 */ },
			{ ImGuiKey_Keypad2, 0x62 /* VK_NUMPAD2 */ },
			{ ImGuiKey_Keypad3, 0x63 /* VK_NUMPAD3 */ },
			{ ImGuiKey_Keypad4, 0x64 /* VK_NUMPAD4 */ },
			{ ImGuiKey_Keypad5, 0x65 /* VK_NUMPAD5 */ },
			{ ImGuiKey_Keypad6, 0x66 /* VK_NUMPAD6 */ },
			{ ImGuiKey_Keypad7, 0x67 /* VK_NUMPAD7 */ },
			{ ImGuiKey_Keypad8, 0x68 /* VK_NUMPAD8 */ },
			{ ImGuiKey_Keypad9, 0x69 /* VK_NUMPAD9 */ },
			{ ImGuiKey_KeypadDecimal, 0x6E /* VK_DECIMAL */ },
			{ ImGuiKey_KeypadDivide, 0x6F /* VK_DIVIDE */ },
			{ ImGuiKey_KeypadMultiply, 0x6A /* VK_MULTIPLY */ },
			{ ImGuiKey_KeypadSubtract, 0x6D /* VK_SUBTRACT */ },
			{ ImGuiKey_KeypadAdd, 0x6B /* VK_ADD */ },
			{ ImGuiMod_Ctrl, 0x11 /* VK_CONTROL */ },
			{ ImGuiMod_Shift, 0x10 /* VK_SHIFT */ },
			{ ImGuiMod_Alt, 0x12 /* VK_MENU */ },
			{ ImGuiMod_Super, 0x5D /* VK_APPS */ },
		};

		for (const std::pair<ImGuiKey, unsigned int> &mapping : key_mappings)
			imgui_io.AddKeyEvent(mapping.first, _input->is_key_down(mapping.second));
		for (ImGuiMouseButton i = 0; i < ImGuiMouseButton_COUNT; i++)
			imgui_io.AddMouseButtonEvent(i, _input->is_mouse_button_down(i));
		for (ImWchar16 c : _input->text_input())
			imgui_io.AddInputCharacterUTF16(c);
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_connected())
		{
			imgui_io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			constexpr std::pair<ImGuiKey, input_gamepad::button> button_mappings[] = {
				{ ImGuiKey_GamepadStart, input_gamepad::button_start },
				{ ImGuiKey_GamepadBack, input_gamepad::button_back },
				{ ImGuiKey_GamepadFaceLeft, input_gamepad::button_x },
				{ ImGuiKey_GamepadFaceRight, input_gamepad::button_b },
				{ ImGuiKey_GamepadFaceUp, input_gamepad::button_y },
				{ ImGuiKey_GamepadFaceDown, input_gamepad::button_a },
				{ ImGuiKey_GamepadDpadLeft, input_gamepad::button_dpad_left },
				{ ImGuiKey_GamepadDpadRight, input_gamepad::button_dpad_right },
				{ ImGuiKey_GamepadDpadUp, input_gamepad::button_dpad_up },
				{ ImGuiKey_GamepadDpadDown, input_gamepad::button_dpad_down },
				{ ImGuiKey_GamepadL1, input_gamepad::button_left_shoulder },
				{ ImGuiKey_GamepadR1, input_gamepad::button_right_shoulder },
				{ ImGuiKey_GamepadL3, input_gamepad::button_left_thumb },
				{ ImGuiKey_GamepadR3, input_gamepad::button_right_thumb },
			};

			for (const std::pair<ImGuiKey, input_gamepad::button> &mapping : button_mappings)
				imgui_io.AddKeyEvent(mapping.first, _input_gamepad->is_button_down(mapping.second));

			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, _input_gamepad->left_trigger_position() != 0.0f, _input_gamepad->left_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, _input_gamepad->right_trigger_position() != 0.0f, _input_gamepad->right_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, _input_gamepad->left_thumb_axis_x() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, _input_gamepad->left_thumb_axis_x() > 0.0f, std::max(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, _input_gamepad->left_thumb_axis_y() > 0.0f, std::max(_input_gamepad->left_thumb_axis_y(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, _input_gamepad->left_thumb_axis_y() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_y(), 0.0f));
		}
		else
		{
			imgui_io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		}
	}

	ImGui::NewFrame();

#if RESHADE_LOCALIZATION
	const std::string prev_language = resources::set_current_language(_selected_language);
	_current_language = resources::get_current_language();
#endif

	ImVec2 viewport_offset = ImVec2(0, 0);
#if RESHADE_FX
	const bool show_spinner = _reload_count > 1 && _tutorial_index != 0;
#else
	const bool show_spinner = false;
#endif

	// Create ImGui widgets and windows
	if (show_splash_window && !(show_spinner && show_overlay))
	{
		ImGui::SetNextWindowPos(_imgui_context->Style.WindowPadding);
		ImGui::SetNextWindowSize(ImVec2(imgui_io.DisplaySize.x - 20.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.862745f, 0.862745f, 0.862745f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.117647f, 0.117647f, 0.117647f, show_spinner ? 0.0f : 0.7f));
		ImGui::Begin("Splash Window", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoFocusOnAppearing);

#if RESHADE_FX
		if (show_spinner)
		{
			imgui::spinner((_effects.size() - _reload_remaining_effects) / float(_effects.size()), 16.0f * _font_size / 13, 10.0f * _font_size / 13);
		}
		else
#endif
		{
			ImGui::TextUnformatted("ReShade " VERSION_STRING_PRODUCT);

			if ((s_latest_version[0] > VERSION_MAJOR) ||
				(s_latest_version[0] == VERSION_MAJOR && s_latest_version[1] > VERSION_MINOR) ||
				(s_latest_version[0] == VERSION_MAJOR && s_latest_version[1] == VERSION_MINOR && s_latest_version[2] > VERSION_REVISION))
			{
				ImGui::TextColored(COLOR_YELLOW, _(
					"An update is available! Please visit %s and install the new version (v%u.%u.%u)."),
					"https://reshade.me",
					s_latest_version[0], s_latest_version[1], s_latest_version[2]);
			}
			else
			{
				ImGui::Text(_("Visit %s for news, updates, effects and discussion."), "https://reshade.me");
			}

			ImGui::Spacing();

#if RESHADE_FX
			if (_reload_remaining_effects != 0 && _reload_remaining_effects != std::numeric_limits<size_t>::max())
			{
				ImGui::ProgressBar((_effects.size() - _reload_remaining_effects) / float(_effects.size()), ImVec2(ImGui::GetContentRegionAvail().x, 0), "");
				ImGui::SameLine(15);
				ImGui::Text(_(
					"Compiling (%zu effects remaining) ... "
					"This might take a while. The application could become unresponsive for some time."),
					_reload_remaining_effects.load());
			}
			else
#endif
			{
				ImGui::ProgressBar(0.0f, ImVec2(ImGui::GetContentRegionAvail().x, 0), "");
				ImGui::SameLine(15);

				if (_input == nullptr)
				{
					ImGui::TextColored(COLOR_YELLOW, _("No keyboard or mouse input available."));
					if (_input_gamepad != nullptr)
					{
						ImGui::SameLine();
						ImGui::TextColored(COLOR_YELLOW, _("Use gamepad instead: Press 'left + right shoulder + start button' to open the configuration overlay."));
					}
				}
#if RESHADE_FX
				else if (_tutorial_index == 0)
				{
					const std::string label = _("ReShade is now installed successfully! Press '%s' to start the tutorial.");
					const size_t key_offset = label.find("%s");

					ImGui::TextUnformatted(label.c_str(), label.c_str() + key_offset);
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
					ImGui::TextUnformatted(input::key_name(_overlay_key_data).c_str());
					ImGui::PopStyleColor();
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextUnformatted(label.c_str() + key_offset + 2, label.c_str() + label.size());
				}
#endif
				else
				{
					const std::string label = _("Press '%s' to open the configuration overlay.");
					const size_t key_offset = label.find("%s");

					ImGui::TextUnformatted(label.c_str(), label.c_str() + key_offset);
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
					ImGui::TextUnformatted(input::key_name(_overlay_key_data).c_str());
					ImGui::PopStyleColor();
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextUnformatted(label.c_str() + key_offset + 2, label.c_str() + label.size());
				}
			}

			std::string error_message;
#if RESHADE_ADDON
			if (!addon_all_loaded)
				error_message += _("There were errors loading some add-ons."),
				error_message += ' ';
#endif
#if RESHADE_FX
			if (!_last_reload_successful)
				error_message += _("There were errors loading some effects."),
				error_message += ' ';
#endif
			if (!error_message.empty())
			{
				error_message += _("Check the log for more details.");
				ImGui::Spacing();
				ImGui::TextColored(COLOR_RED, error_message.c_str());
			}
		}

		viewport_offset.y += ImGui::GetWindowHeight() + _imgui_context->Style.WindowPadding.x; // Add small space between windows

		ImGui::End();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
	}

	if (show_message_window)
	{
		ImGui::SetNextWindowPos(_imgui_context->Style.WindowPadding + viewport_offset);
		ImGui::SetNextWindowSize(ImVec2(imgui_io.DisplaySize.x - 20.0f, 0.0f));
		ImGui::Begin("Message Window", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoFocusOnAppearing);

		if (!_preset_save_successful)
		{
#if RESHADE_FX
			ImGui::TextColored(COLOR_RED, _("Unable to save configuration and/or current preset. Make sure file permissions are set up to allow writing to these paths and their parent directories:\n%s\n%s"), _config_path.u8string().c_str(), _current_preset_path.u8string().c_str());
#else
			ImGui::TextColored(COLOR_RED, _("Unable to save configuration. Make sure file permissions are set up to allow writing to %s."), _config_path.u8string().c_str());
#endif
		}
		else if (show_screenshot_message)
		{
			if (!_last_screenshot_save_successful)
				if (_screenshot_directory_creation_successful)
					ImGui::TextColored(COLOR_RED, _("Unable to save screenshot because of an internal error (the format may not be supported or the drive may be full)."));
				else
					ImGui::TextColored(COLOR_RED, _("Unable to save screenshot because path could not be created: %s"), (g_reshade_base_path / _screenshot_path).u8string().c_str());
			else
				ImGui::Text(_("Screenshot successfully saved to %s"), _last_screenshot_file.u8string().c_str());
		}
#if RESHADE_FX
		else if (show_preset_transition_message)
		{
			ImGui::Text(_("Switching preset to %s ..."), _current_preset_path.stem().u8string().c_str());
		}
#endif

		viewport_offset.y += ImGui::GetWindowHeight() + _imgui_context->Style.WindowPadding.x; // Add small space between windows

		ImGui::End();
	}

	if (show_statistics_window && !show_splash_window && !show_message_window)
	{
		ImVec2 fps_window_pos(5, 5);
		ImVec2 fps_window_size(200, 0);

		// Get last calculated window size (because of 'ImGuiWindowFlags_AlwaysAutoResize')
		if (ImGuiWindow *const fps_window = ImGui::FindWindowByName("OSD"))
		{
			fps_window_size  = fps_window->Size;
			fps_window_size.y = std::max(fps_window_size.y, _imgui_context->Style.FramePadding.y * 4.0f + _imgui_context->Style.ItemSpacing.y +
				(_imgui_context->Style.ItemSpacing.y + _imgui_context->FontBaseSize * _fps_scale) * ((show_clock ? 1 : 0) + (show_fps ? 1 : 0) + (show_frametime ? 1 : 0) + (show_preset_name ? 1 : 0)));
		}

		if (_fps_pos % 2)
			fps_window_pos.x = imgui_io.DisplaySize.x - fps_window_size.x - 5;
		if (_fps_pos > 1)
			fps_window_pos.y = imgui_io.DisplaySize.y - fps_window_size.y - 5;

		ImGui::SetNextWindowPos(fps_window_pos);
		ImGui::PushStyleColor(ImGuiCol_Text, (const ImVec4 &)_fps_col);
		ImGui::Begin("OSD", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoBackground |
			ImGuiWindowFlags_AlwaysAutoResize);

		ImGui::SetWindowFontScale(_fps_scale);

		const float content_width = ImGui::GetContentRegionAvail().x;
		char temp[512];

		if (show_clock)
		{
			const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
			struct tm tm; localtime_s(&tm, &t);

			int temp_size;
			switch (_clock_format)
			{
			default:
			case 0:
				temp_size = ImFormatString(temp, sizeof(temp), "%02d:%02d", tm.tm_hour, tm.tm_min);
				break;
			case 1:
				temp_size = ImFormatString(temp, sizeof(temp), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
				break;
			case 2:
				temp_size = ImFormatString(temp, sizeof(temp), "%.4d-%.2d-%.2d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
				break;
			}
			if (_fps_pos % 2) // Align text to the right of the window
				ImGui::SetCursorPosX(content_width - ImGui::CalcTextSize(temp, temp + temp_size).x + _imgui_context->Style.ItemSpacing.x);
			ImGui::TextUnformatted(temp, temp + temp_size);
		}
		if (show_fps)
		{
			const int temp_size = ImFormatString(temp, sizeof(temp), "%.0f fps", imgui_io.Framerate);
			if (_fps_pos % 2)
				ImGui::SetCursorPosX(content_width - ImGui::CalcTextSize(temp, temp + temp_size).x + _imgui_context->Style.ItemSpacing.x);
			ImGui::TextUnformatted(temp, temp + temp_size);
		}
		if (show_frametime)
		{
			const int temp_size = ImFormatString(temp, sizeof(temp), "%5.2f ms", 1000.0f / imgui_io.Framerate);
			if (_fps_pos % 2)
				ImGui::SetCursorPosX(content_width - ImGui::CalcTextSize(temp, temp + temp_size).x + _imgui_context->Style.ItemSpacing.x);
			ImGui::TextUnformatted(temp, temp + temp_size);
		}
#if RESHADE_FX
		if (show_preset_name)
		{
			const std::string preset_name = _current_preset_path.stem().u8string();
			if (_fps_pos % 2)
				ImGui::SetCursorPosX(content_width - ImGui::CalcTextSize(preset_name.c_str(), preset_name.c_str() + preset_name.size()).x + _imgui_context->Style.ItemSpacing.x);
			ImGui::TextUnformatted(preset_name.c_str(), preset_name.c_str() + preset_name.size());
		}
#endif

		ImGui::Dummy(ImVec2(200, 0)); // Force a minimum window width

		ImGui::End();
		ImGui::PopStyleColor();
	}

	if (_show_overlay)
	{
		const ImGuiViewport *const viewport = ImGui::GetMainViewport();

		// Change font size if user presses the control key and moves the mouse wheel
		if (!_no_font_scaling && imgui_io.KeyCtrl && imgui_io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
		{
			_font_size = ImClamp(_font_size + static_cast<int>(imgui_io.MouseWheel), 8, 64);
			_editor_font_size = ImClamp(_editor_font_size + static_cast<int>(imgui_io.MouseWheel), 8, 64);
			imgui_io.Fonts->TexReady = false;
			save_config();

			_is_font_scaling = true;
		}

		if (_is_font_scaling)
		{
			if (!imgui_io.KeyCtrl)
				_is_font_scaling = false;

			ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, _imgui_context->Style.WindowPadding * 2.0f);
			ImGui::Begin("FontScaling", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
			ImGui::Text(_("Scaling font size (%d) with 'Ctrl' + mouse wheel"), _font_size);
			ImGui::End();
			ImGui::PopStyleVar();
		}

		const std::pair<std::string, void(runtime::*)()> overlay_callbacks[] = {
#if RESHADE_FX
			{ _("Home###home"), &runtime::draw_gui_home },
#endif
#if RESHADE_ADDON
			{ _("Add-ons###addons"), &runtime::draw_gui_addons },
#endif
			{ _("Settings###settings"), &runtime::draw_gui_settings },
			{ _("Statistics###statistics"), &runtime::draw_gui_statistics },
			{ _("Log###log"), &runtime::draw_gui_log },
			{ _("About###about"), &runtime::draw_gui_about },
			{ _("NFS Tweaks"), &runtime::draw_gui_nfs }

		};

		const ImGuiID root_space_id = ImGui::GetID("ViewportDockspace");

		// Set up default dock layout if this was not done yet
		const bool init_window_layout = !ImGui::DockBuilderGetNode(root_space_id);
		if (init_window_layout)
		{
			// Add the root node
			ImGui::DockBuilderAddNode(root_space_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(root_space_id, viewport->Size);

			// Split root node into two spaces
			ImGuiID main_space_id = 0;
			ImGuiID right_space_id = 0;
			ImGui::DockBuilderSplitNode(root_space_id, ImGuiDir_Left, 0.35f, &main_space_id, &right_space_id);

			// Attach most windows to the main dock space
			for (const std::pair<std::string, void(runtime::*)()> &widget : overlay_callbacks)
				ImGui::DockBuilderDockWindow(widget.first.c_str(), main_space_id);

			// Attach editor window to the remaining dock space
			ImGui::DockBuilderDockWindow("###editor", right_space_id);

			// Commit the layout
			ImGui::DockBuilderFinish(root_space_id);
		}

		ImGui::SetNextWindowPos(viewport->Pos + viewport_offset);
		ImGui::SetNextWindowSize(viewport->Size - viewport_offset);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::Begin("Viewport", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoDocking | // This is the background viewport, the docking space is a child of it
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoBackground);
		ImGui::DockSpace(root_space_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
		ImGui::End();

		if (_imgui_context->NavInputSource > ImGuiInputSource_Mouse && _imgui_context->NavWindowingTarget == nullptr)
		{
			// Reset input source to mouse when the cursor is moved
			if (_input != nullptr && (_input->mouse_movement_delta_x() != 0 || _input->mouse_movement_delta_y() != 0))
				_imgui_context->NavInputSource = ImGuiInputSource_Mouse;
			// Ensure there is always a window that has navigation focus when keyboard or gamepad navigation is used (choose the first overlay window created next)
			else if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
				ImGui::SetNextWindowFocus();
		}

		for (const std::pair<std::string, void(runtime:: *)()> &widget : overlay_callbacks)
		{
			if (ImGui::Begin(widget.first.c_str(), nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) // No focus so that window state is preserved between opening/closing the GUI
				(this->*widget.second)();
			ImGui::End();
		}

#if RESHADE_FX
		if (!_editors.empty())
		{
			if (ImGui::Begin(_("Edit###editor"), nullptr, ImGuiWindowFlags_NoFocusOnAppearing) &&
				ImGui::BeginTabBar("editor_tabs"))
			{
				for (auto it = _editors.begin(); it != _editors.end();)
				{
					std::string title = it->entry_point_name.empty() ? it->file_path.filename().u8string() : it->entry_point_name;
					title += " ###editor" + std::to_string(std::distance(_editors.begin(), it));

					bool is_open = true;
					ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
					if (it->editor.is_modified())
						flags |= ImGuiTabItemFlags_UnsavedDocument;
					if (it->selected)
						flags |= ImGuiTabItemFlags_SetSelected;

					if (ImGui::BeginTabItem(title.c_str(), &is_open, flags))
					{
						draw_code_editor(*it);
						ImGui::EndTabItem();
					}

					it->selected = false;

					if (!is_open)
						it = _editors.erase(it);
					else
						++it;
				}

				ImGui::EndTabBar();
			}
			ImGui::End();
		}
#endif
	}

#if RESHADE_ADDON == 1
	if (addon_enabled)
#endif
#if RESHADE_ADDON
	{
		for (const addon_info &info : addon_loaded_info)
		{
			for (const addon_info::overlay_callback &widget : info.overlay_callbacks)
			{
				if (widget.title == "OSD" ? show_splash_window : !_show_overlay)
					continue;

				if (ImGui::Begin(widget.title.c_str(), nullptr, ImGuiWindowFlags_NoFocusOnAppearing))
					widget.callback(this);
				ImGui::End();
			}
		}

		invoke_addon_event<addon_event::reshade_overlay>(this);
	}
#endif

#if RESHADE_FX
	if (_preview_texture != 0 && _effects_enabled)
	{
		if (!_show_overlay)
		{
			// Create a temporary viewport window to attach image to when overlay is not open
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(imgui_io.DisplaySize.x, imgui_io.DisplaySize.y));
			ImGui::Begin("Viewport", nullptr,
				ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_NoNav |
				ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoDocking |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoBringToFrontOnFocus |
				ImGuiWindowFlags_NoBackground);
			ImGui::End();
		}

		// Scale image to fill the entire viewport by default
		ImVec2 preview_min = ImVec2(0, 0);
		ImVec2 preview_max = imgui_io.DisplaySize;

		// Positing image in the middle of the viewport when using original size
		if (_preview_size[0])
		{
			preview_min.x = (preview_max.x * 0.5f) - (_preview_size[0] * 0.5f);
			preview_max.x = (preview_max.x * 0.5f) + (_preview_size[0] * 0.5f);
		}
		if (_preview_size[1])
		{
			preview_min.y = (preview_max.y * 0.5f) - (_preview_size[1] * 0.5f);
			preview_max.y = (preview_max.y * 0.5f) + (_preview_size[1] * 0.5f);
		}

		ImGui::FindWindowByName("Viewport")->DrawList->AddImage(_preview_texture.handle, preview_min, preview_max, ImVec2(0, 0), ImVec2(1, 1), _preview_size[2]);
	}
#endif

#if RESHADE_LOCALIZATION
	resources::set_current_language(prev_language);
#endif

	// Disable keyboard shortcuts while typing into input boxes
	_ignore_shortcuts |= ImGui::IsAnyItemActive();

	// Render ImGui widgets and windows
	ImGui::Render();

	if (_input != nullptr)
	{
		const bool block_input = _input_processing_mode != 0 && (_show_overlay || _block_input_next_frame);
		const bool block_mouse_input = block_input && (imgui_io.WantCaptureMouse || _input_processing_mode == 2);
		const bool block_keyboard_input = block_input && (imgui_io.WantCaptureKeyboard || _input_processing_mode == 2);

		_input->block_mouse_input(block_mouse_input);
		_input->block_keyboard_input(block_keyboard_input);
		_input->block_mouse_cursor_warping(_show_overlay || _block_input_next_frame || block_mouse_input);
	}

	if (ImDrawData *const draw_data = ImGui::GetDrawData();
		draw_data != nullptr && draw_data->CmdListsCount != 0 && draw_data->TotalVtxCount != 0)
	{
		api::command_list *const cmd_list = _graphics_queue->get_immediate_command_list();

		if (_back_buffer_resolved != 0)
		{
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[0]);
		}
		else
		{
			uint32_t back_buffer_index = get_current_back_buffer_index() * 2;
			const api::resource back_buffer_resource = _device->get_resource_from_view(_back_buffer_targets[back_buffer_index]);

			cmd_list->barrier(back_buffer_resource, api::resource_usage::present, api::resource_usage::render_target);
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[back_buffer_index]);
			cmd_list->barrier(back_buffer_resource, api::resource_usage::render_target, api::resource_usage::present);
		}
	}

	ImGui::SetCurrentContext(backup_context);
}

#if RESHADE_FX
void reshade::runtime::draw_gui_home()
{
	std::string tutorial_text;

	// It is not possible to follow some of the tutorial steps while performance mode is active, so skip them
	if (_performance_mode && _tutorial_index <= 3)
		_tutorial_index = 4;

	const float auto_save_button_spacing = 2.0f;
	const float button_width = 12.5f * _font_size;

	if (_tutorial_index > 0)
	{
		if (_tutorial_index == 1)
		{
			tutorial_text = _(
				"This is the preset selection. All changes will be saved to the selected preset file.\n\n"
				"Click on the '+' button to add a new one.\n"
				"Use the right mouse button and click on the preset button to open a context menu with additional options.");

			ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR_RED);
			ImGui::PushStyleColor(ImGuiCol_Button, COLOR_RED);
		}

		const float button_height = ImGui::GetFrameHeight();
		const float button_spacing = _imgui_context->Style.ItemInnerSpacing.x;

		bool reload_preset = false;

		// Loading state may change below, so keep track of current state so that 'ImGui::Push/Pop*' is executed the correct amount of times
		const bool was_loading = is_loading();
		if (was_loading)
			ImGui::BeginDisabled();

		if (ImGui::ArrowButtonEx("<", ImGuiDir_Left, ImVec2(button_height, button_height), ImGuiButtonFlags_NoNavFocus))
			if (switch_to_next_preset(_current_preset_path.parent_path(), true))
				reload_preset = true;
		ImGui::SetItemTooltip(_("Previous preset"));

		ImGui::SameLine(0, button_spacing);

		if (ImGui::ArrowButtonEx(">", ImGuiDir_Right, ImVec2(button_height, button_height), ImGuiButtonFlags_NoNavFocus))
			if (switch_to_next_preset(_current_preset_path.parent_path(), false))
				reload_preset = true;
		ImGui::SetItemTooltip(_("Next preset"));

		ImGui::SameLine();

		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

		const auto browse_button_pos = ImGui::GetCursorScreenPos();
		const auto browse_button_width = ImGui::GetContentRegionAvail().x - (button_height + button_spacing + _imgui_context->Style.ItemSpacing.x + auto_save_button_spacing + button_width);

		if (ImGui::ButtonEx((_current_preset_path.stem().u8string() + "###browse_button").c_str(), ImVec2(browse_button_width, 0), ImGuiButtonFlags_NoNavFocus))
		{
			_file_selection_path = _current_preset_path;
			ImGui::OpenPopup("##browse");
		}

		if (_preset_is_modified)
			ImGui::RenderBullet(ImGui::GetWindowDrawList(), browse_button_pos + ImVec2(browse_button_width - _font_size * 0.5f - _imgui_context->Style.FramePadding.x, button_height * 0.5f), ImGui::GetColorU32(ImGuiCol_Text));

		ImGui::PopStyleVar();

		if (_input != nullptr &&
			ImGui::BeginPopupContextItem())
		{
			auto preset_shortcut_it = std::find_if(_preset_shortcuts.begin(), _preset_shortcuts.end(),
				[this](const preset_shortcut &shortcut) { return shortcut.preset_path == _current_preset_path; });

			preset_shortcut shortcut;
			if (preset_shortcut_it != _preset_shortcuts.end())
				shortcut = *preset_shortcut_it;
			else
				shortcut.preset_path = _current_preset_path;

			ImGui::SetNextItemWidth(18.0f * _font_size);
			if (imgui::key_input_box("##toggle_key", shortcut.key_data, *_input))
			{
				if (preset_shortcut_it != _preset_shortcuts.end())
					*preset_shortcut_it = std::move(shortcut);
				else
					_preset_shortcuts.push_back(std::move(shortcut));
			}

			ImGui::EndPopup();
		}

		ImGui::SameLine(0, button_spacing);

		if (ImGui::Button(ICON_FK_FOLDER, ImVec2(button_height, button_height)))
			utils::open_explorer(_current_preset_path);
		ImGui::SetItemTooltip(_("Open folder in explorer"));

		ImGui::SameLine();

		const bool was_auto_save_preset = _auto_save_preset;

		if (imgui::toggle_button(
				(std::string(was_auto_save_preset ? _("Auto Save on") : _("Auto Save")) + "###auto_save").c_str(),
				_auto_save_preset,
				(was_auto_save_preset ? 0.0f : auto_save_button_spacing) + button_width - (button_spacing + button_height) * (was_auto_save_preset ? 2 : 3)))
		{
			if (!was_auto_save_preset)
				save_current_preset();
			save_config();

			_preset_is_modified = false;
		}

		ImGui::SetItemTooltip(_("Save current preset automatically on every modification."));

		if (was_auto_save_preset)
		{
			ImGui::SameLine(0, button_spacing + auto_save_button_spacing);
		}
		else
		{
			ImGui::SameLine(0, button_spacing);

			ImGui::BeginDisabled(!_preset_is_modified);

			if (imgui::confirm_button(ICON_FK_UNDO, button_height, _("Do you really want to reset all techniques and values?")))
				reload_preset = true;

			ImGui::SetItemTooltip(_("Reset all techniques and values to those of the current preset."));

			ImGui::EndDisabled();

			ImGui::SameLine(0, button_spacing);
		}

		// Cannot save in performance mode, since there are no variables to retrieve values from then
		ImGui::BeginDisabled(_performance_mode || _is_in_preset_transition);

		const auto save_and_clean_preset = _auto_save_preset || (_imgui_context->IO.KeyCtrl || _imgui_context->IO.KeyShift);

		if (ImGui::ButtonEx(ICON_FK_FLOPPY, ImVec2(button_height, button_height), ImGuiButtonFlags_NoNavFocus))
		{
			if (save_and_clean_preset)
				ini_file::load_cache(_current_preset_path).clear();
			save_current_preset();
			ini_file::flush_cache(_current_preset_path);

			_preset_is_modified = false;
		}

		ImGui::SetItemTooltip(save_and_clean_preset ?
			_("Clean up and save the current preset (removes all values for disabled techniques).") : _("Save the current preset."));

		ImGui::EndDisabled();

		ImGui::SameLine(0, button_spacing);
		if (ImGui::ButtonEx(ICON_FK_PLUS, ImVec2(button_height, button_height), ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClick))
		{
			_inherit_current_preset = false;
			_template_preset_path.clear();
			_file_selection_path = _current_preset_path.parent_path();
			ImGui::OpenPopup("##create");
		}

		ImGui::SetItemTooltip(_("Add a new preset."));

		if (was_loading)
			ImGui::EndDisabled();

		ImGui::SetNextWindowPos(browse_button_pos + ImVec2(-_imgui_context->Style.WindowPadding.x, ImGui::GetFrameHeightWithSpacing()));
		if (imgui::file_dialog("##browse", _file_selection_path, std::max(browse_button_width, 450.0f), { L".ini", L".txt" }, { _config_path, g_reshade_base_path / L"ReShade.ini" }))
		{
			std::error_code ec;
			if (std::filesystem::path resolved_path = std::filesystem::canonical(_file_selection_path, ec); !ec)
				_file_selection_path = std::move(resolved_path);

			// Check that this is actually a valid preset file
			if (ini_file::load_cache(_file_selection_path).has({}, "Techniques"))
			{
				reload_preset = true;
				_current_preset_path = _file_selection_path;
			}
			else
			{
				ini_file::clear_cache(_file_selection_path);
				ImGui::OpenPopup("##preseterror");
			}
		}

		if (ImGui::BeginPopup("##create"))
		{
			ImGui::Checkbox(_("Inherit current preset"), &_inherit_current_preset);

			if (!_inherit_current_preset)
				imgui::file_input_box(_("Template"), nullptr, _template_preset_path, _file_selection_path, { L".ini", L".txt" });

			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();

			char preset_name[260] = "";
			if (ImGui::InputText(_("Preset name"), preset_name, sizeof(preset_name), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter, &is_invalid_filename_element) && preset_name[0] != '\0')
			{
				std::filesystem::path new_preset_path = _current_preset_path.parent_path() / std::filesystem::u8path(preset_name);
				if (new_preset_path.extension() != L".ini" && new_preset_path.extension() != L".txt")
					new_preset_path += L".ini";

				std::error_code ec;
				resolve_path(new_preset_path, ec);

				if (const std::filesystem::file_type file_type = std::filesystem::status(new_preset_path, ec).type();
					file_type != std::filesystem::file_type::directory)
				{
					reload_preset =
						file_type == std::filesystem::file_type::not_found ||
						ini_file::load_cache(new_preset_path).has({}, "Techniques");

					if (file_type == std::filesystem::file_type::not_found)
					{
						if (_inherit_current_preset)
						{
							_current_preset_path = new_preset_path;
							save_current_preset();
						}
						else if (!_template_preset_path.empty() && !std::filesystem::copy_file(_template_preset_path, new_preset_path, std::filesystem::copy_options::overwrite_existing, ec))
						{
							log::message(log::level::error, "Failed to copy preset template '%s' to '%s' with error code %d!", _template_preset_path.u8string().c_str(), new_preset_path.u8string().c_str(), ec.value());
						}
					}
				}

				if (reload_preset)
				{
					ImGui::CloseCurrentPopup();
					_current_preset_path = new_preset_path;
				}
				else
				{
					ImGui::SetKeyboardFocusHere(-1);
				}
			}

			ImGui::EndPopup();
		}

		if (ImGui::BeginPopup("##preseterror"))
		{
			ImGui::TextColored(COLOR_RED, _("The selected file is not a valid preset!"));
			ImGui::EndPopup();
		}

		if (reload_preset)
		{
			save_config();
			load_current_preset();

			_show_splash = true;
			_preset_is_modified = false;
			_last_preset_switching_time = _last_present_time;
			_is_in_preset_transition = true;

#if RESHADE_ADDON
			if (!is_loading()) // Will be called by 'update_effects' when 'load_current_preset' forced a reload
				invoke_addon_event<addon_event::reshade_set_current_preset_path>(this, _current_preset_path.u8string().c_str());
#endif
		}

		if (_tutorial_index == 1)
			ImGui::PopStyleColor(2);
	}
	else
	{
		tutorial_text = _(
			"Welcome! Since this is the first time you start ReShade, we'll go through a quick tutorial covering the most important features.\n\n"
			"If you have difficulties reading this text, press the 'Ctrl' key and adjust the font size with your mouse wheel. "
			"The window size is variable as well, just grab the right edge and move it around.\n\n"
			"You can also use the keyboard for navigation in case mouse input does not work. Use the arrow keys to navigate, space bar to confirm an action or enter a control and the 'Esc' key to leave a control. "
			"Press 'Ctrl + Tab' to switch between tabs and windows (use this to focus this page in case the other navigation keys do not work at first).\n\n"
			"Click on the 'Continue' button to continue the tutorial.");
	}

	if (_tutorial_index > 1)
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	}

	if (_reload_remaining_effects != std::numeric_limits<size_t>::max())
	{
		ImGui::SetCursorPos(ImGui::GetWindowSize() * 0.5f - ImVec2(21, 21));
		imgui::spinner((_effects.size() - _reload_remaining_effects) / float(_effects.size()), 16.0f * _font_size / 13, 10.0f * _font_size / 13);
		return; // Cannot show techniques and variables while effects are loading, since they are being modified in other threads during that time
	}

	if (_tutorial_index > 1)
	{
		if (imgui::search_input_box(_effect_filter, sizeof(_effect_filter), -((_variable_editor_tabs ? 1 : 2) * (_imgui_context->Style.ItemSpacing.x + 2.0f + button_width))))
		{
			_effects_expanded_state = 3;

			for (technique &tech : _techniques)
			{
				std::string_view label = tech.annotation_as_string("ui_label");
				if (label.empty())
					label = tech.name;

				tech.hidden = tech.annotation_as_int("hidden") != 0 || !(string_contains(label, _effect_filter) || string_contains(_effects[tech.effect_index].source_file.filename().u8string(), _effect_filter));
			}
		}

		ImGui::SameLine();

		ImGui::BeginDisabled(_is_in_preset_transition);

		if (ImGui::Button(_("Active to top"), ImVec2(auto_save_button_spacing + button_width, 0)))
		{
			std::vector<size_t> technique_indices = _technique_sorting;

			for (auto it = technique_indices.begin(), target_it = it; it != technique_indices.end(); ++it)
			{
				const technique &tech = _techniques[*it];

				if (tech.enabled || tech.toggle_key_data[0] != 0)
				{
					target_it = std::rotate(target_it, it, std::next(it));
				}
			}

			reorder_techniques(std::move(technique_indices));

			if (_auto_save_preset)
				save_current_preset();
			else
				_preset_is_modified = true;
		}

		ImGui::EndDisabled();

		if (!_variable_editor_tabs)
		{
			ImGui::SameLine();

			if (ImGui::Button((_effects_expanded_state & 2) ? _("Collapse all") : _("Expand all"), ImVec2(auto_save_button_spacing + button_width, 0)))
				_effects_expanded_state = (~_effects_expanded_state & 2) | 1;
		}

		if (_tutorial_index == 2)
		{
			tutorial_text = _(
				"This is the list of effects. It contains all techniques exposed by effect files (.fx) found in the effect search paths specified in the settings.\n\n"
				"Enter text in the \"Search\" box at the top to filter it and search for specific techniques.\n\n"
				"Click on a technique to enable or disable it or drag it to a new location in the list to change the order in which the effects are applied (from top to bottom).\n"
				"Use the right mouse button and click on an item to open a context menu with additional options.");

			ImGui::PushStyleColor(ImGuiCol_Border, COLOR_RED);
		}

		ImGui::Spacing();

		if (!_last_reload_successful)
		{
			ImGui::PushTextWrapPos();
			ImGui::PushStyleColor(ImGuiCol_Text, COLOR_RED);
			ImGui::TextUnformatted(_("There were errors loading some effects."));
			ImGui::TextUnformatted(_("Hover the cursor over any red entries below to see the related error messages and/or check the log for more details if there are none."));
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
		}

		if (!_effects_enabled)
		{
			ImGui::Text(_("Effects are disabled. Press '%s' to enable them again."), input::key_name(_effects_key_data).c_str());
			ImGui::Spacing();
		}

		float bottom_height = _variable_editor_height;
		bottom_height = std::max(bottom_height, 20.0f);
		bottom_height = ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y + (
			_performance_mode ? 0 : (17 /* splitter */ + (bottom_height + (_tutorial_index == 3 ? 175 : 0))));
		bottom_height = std::min(bottom_height, ImGui::GetContentRegionAvail().y - 20.0f);

		if (ImGui::BeginChild("##techniques", ImVec2(0, -bottom_height), ImGuiChildFlags_Borders))
		{
			if (_effect_load_skipping && _show_force_load_effects_button)
			{
				const size_t skipped_effects = std::count_if(_effects.cbegin(), _effects.cend(),
					[](const effect &effect) { return effect.skipped; });

				if (skipped_effects > 0)
				{
					char temp[64];
					const int temp_size = ImFormatString(temp, sizeof(temp), _("Force load all effects (%zu remaining)"), skipped_effects);

					if (ImGui::ButtonEx((std::string(temp, temp_size) + "###force_reload_button").c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
					{
						reload_effects(true);

						ImGui::EndChild();
						return;
					}
				}
			}

			ImGui::BeginDisabled(_is_in_preset_transition);
			draw_technique_editor();
			ImGui::EndDisabled();
		}
		ImGui::EndChild();

		if (_tutorial_index == 2)
			ImGui::PopStyleColor();
	}

	if (_tutorial_index > 2 && !_performance_mode)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		ImGui::ButtonEx("##splitter", ImVec2(ImGui::GetContentRegionAvail().x, 5));
		ImGui::PopStyleVar();

		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (ImGui::IsItemActive())
		{
			ImVec2 move_delta = _imgui_context->IO.MouseDelta;
			move_delta += ImGui::GetKeyMagnitude2d(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown) * _imgui_context->IO.DeltaTime * 500.0f;

			_variable_editor_height = std::max(_variable_editor_height - move_delta.y, 0.0f);
			save_config();
		}

		if (_tutorial_index == 3)
		{
			tutorial_text = _(
				"This is the list of variables. It contains all tweakable options the active effects expose. Values here apply in real-time.\n\n"
				"Press 'Ctrl' and click on a widget to manually edit the value (can also hold 'Ctrl' while adjusting the value in a widget to have it ignore any minimum or maximum values).\n"
				"Use the right mouse button and click on an item to open a context menu with additional options.\n\n"
				"Once you have finished tweaking your preset, be sure to enable the 'Performance Mode' check box. "
				"This will reload all effects into a more optimal representation that can give a performance boost, but disables variable tweaking and this list.");

			ImGui::PushStyleColor(ImGuiCol_Border, COLOR_RED);
		}

		const float bottom_height = ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y + (_tutorial_index == 3 ? 175 : 0);

		if (ImGui::BeginChild("##variables", ImVec2(0, -bottom_height), ImGuiChildFlags_Borders))
		{
			ImGui::BeginDisabled(_is_in_preset_transition);
			draw_variable_editor();
			ImGui::EndDisabled();
		}
		ImGui::EndChild();

		if (_tutorial_index == 3)
			ImGui::PopStyleColor();
	}

	if (_tutorial_index > 3)
	{
		ImGui::Spacing();

		if (ImGui::Button((ICON_FK_REFRESH " " + std::string(_("Reload"))).c_str(), ImVec2(-(auto_save_button_spacing + button_width), 0)))
		{
			load_config(); // Reload configuration too

			if (!_no_effect_cache && (_imgui_context->IO.KeyCtrl || _imgui_context->IO.KeyShift))
				clear_effect_cache();

			reload_effects();
		}

		ImGui::SetItemTooltip(_("Reload all effects (can hold 'Ctrl' while clicking to clear the effect cache before loading)."));

		ImGui::SameLine();

		if (ImGui::Checkbox(_("Performance Mode"), &_performance_mode))
		{
			save_config();
			reload_effects(); // Reload effects after switching
		}

		ImGui::SetItemTooltip(_("Reload all effects into a more optimal representation that can give a performance boost, but disables variable tweaking."));
	}
	else
	{
		const float max_frame_width = ImGui::GetContentRegionAvail().x;
		const float max_frame_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - _imgui_context->Style.ItemSpacing.y;
		const float required_frame_height =
			ImGui::CalcTextSize(tutorial_text.data(), tutorial_text.data() + tutorial_text.size(), false, max_frame_width - _imgui_context->Style.FramePadding.x * 2).y +
			_imgui_context->Style.FramePadding.y * 2;

		if (ImGui::BeginChild("##tutorial", ImVec2(max_frame_width, std::min(max_frame_height, required_frame_height)), ImGuiChildFlags_FrameStyle))
		{
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted(tutorial_text.data(), tutorial_text.data() + tutorial_text.size());
			ImGui::PopTextWrapPos();
		}
		ImGui::EndChild();

		if (_tutorial_index == 0)
		{
			if (ImGui::Button((std::string(_("Continue")) + "###tutorial_button").c_str(), ImVec2(max_frame_width * 0.66666666f, 0)))
			{
				_tutorial_index++;

				save_config();
			}

			ImGui::SameLine();

			if (ImGui::Button(_("Skip Tutorial"), ImVec2(max_frame_width * 0.33333333f - _imgui_context->Style.ItemSpacing.x, 0)))
			{
				_tutorial_index = 4;

				save_config();
			}
		}
		else
		{
			if (ImGui::Button((std::string(_tutorial_index == 3 ? _("Finish") : _("Continue")) + "###tutorial_button").c_str(), ImVec2(max_frame_width, 0)))
			{
				_tutorial_index++;

				if (_tutorial_index == 4)
					save_config();
			}
		}
	}
}
#endif
void reshade::runtime::draw_gui_settings()
{
	if (ImGui::Button((ICON_FK_FOLDER " " + std::string(_("Open base folder in explorer"))).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		utils::open_explorer(_config_path);

	ImGui::Spacing();

	bool modified = false;
	bool modified_custom_style = false;

	if (ImGui::CollapsingHeader(_("General"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (_input != nullptr)
		{
			std::string input_processing_mode_items = _(
				"Pass on all input\n"
				"Block input when cursor is on overlay\n"
				"Block all input when overlay is visible\n");
			std::replace(input_processing_mode_items.begin(), input_processing_mode_items.end(), '\n', '\0');
			modified |= ImGui::Combo(_("Input processing"), reinterpret_cast<int *>(&_input_processing_mode), input_processing_mode_items.c_str());

			modified |= imgui::key_input_box(_("Overlay key"), _overlay_key_data, *_input);
			modified |= imgui::key_input_box("NFS HUD toggle key", _toggle_fe_key_data, *_input);

#if RESHADE_FX
			modified |= imgui::key_input_box(_("Effect toggle key"), _effects_key_data, *_input);
			modified |= imgui::key_input_box(_("Effect reload key"), _reload_key_data, *_input);

			modified |= imgui::key_input_box(_("Performance mode toggle key"), _performance_mode_key_data, *_input);

			modified |= imgui::key_input_box(_("Previous preset key"), _prev_preset_key_data, *_input);
			modified |= imgui::key_input_box(_("Next preset key"), _next_preset_key_data, *_input);

			modified |= ImGui::SliderInt(_("Preset transition duration"), reinterpret_cast<int *>(&_preset_transition_duration), 0, 10 * 1000);
			ImGui::SetItemTooltip(_(
				"Make a smooth transition when switching presets, but only for floating point values.\n"
				"Recommended for multiple presets that contain the same effects, otherwise set this to zero.\n"
				"Values are in milliseconds."));
#endif

			ImGui::Spacing();
		}

#if RESHADE_FX
		modified |= imgui::file_input_box(_("Start-up preset"), nullptr, _startup_preset_path, _file_selection_path, { L".ini", L".txt" });
		ImGui::SetItemTooltip(_("When not empty, reset the current preset to this file during reloads."));

		ImGui::Spacing();

		modified |= imgui::path_list(_("Effect search paths"), _effect_search_paths, _file_selection_path, g_reshade_base_path);
		ImGui::SetItemTooltip(_("List of directory paths to be searched for effect files (.fx).\nPaths that end in \"\\**\" are searched recursively."));
		modified |= imgui::path_list(_("Texture search paths"), _texture_search_paths, _file_selection_path, g_reshade_base_path);
		ImGui::SetItemTooltip(_("List of directory paths to be searched for image files used as source for textures.\nPaths that end in \"\\**\" are searched recursively."));

		if (ImGui::Checkbox(_("Load only enabled effects"), &_effect_load_skipping))
		{
			modified = true;

			// Force load all effects in case some where skipped after load skipping was disabled
			reload_effects(!_effect_load_skipping);
		}

		if (ImGui::Button(_("Clear effect cache"), ImVec2(ImGui::CalcItemWidth(), 0)))
			clear_effect_cache();
		ImGui::SetItemTooltip(_("Clear effect cache located in \"%s\"."), _effect_cache_path.u8string().c_str());
#endif
	}

	if (ImGui::CollapsingHeader(_("Screenshots"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (_input != nullptr)
		{
			modified |= imgui::key_input_box(_("Screenshot key"), _screenshot_key_data, *_input);
		}

		modified |= imgui::directory_input_box(_("Screenshot path"), _screenshot_path, _file_selection_path);

		char name[260];
		name[_screenshot_name.copy(name, sizeof(name) - 1)] = '\0';
		if (ImGui::InputText(_("Screenshot name"), name, sizeof(name), ImGuiInputTextFlags_CallbackCharFilter, &is_invalid_path_element))
		{
			modified = true;
			_screenshot_name = name;

			// Strip any leading slashes, to avoid starting at drive root, rather than the screenshot path
			_screenshot_name = trim(_screenshot_name, " \t\\");
		}

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			ImGui::SetTooltip(_(
				"Macros you can add that are resolved during saving:\n"
				"  %%AppName%%         Name of the application (%s)\n"
				"  %%PresetName%%      File name without extension of the current preset file (%s)\n"
				"  %%BeforeAfter%%     Term describing the moment the screenshot was taken ('Before', 'After' or 'Overlay')\n"
				"  %%Date%%            Current date in format '%s'\n"
				"  %%DateYear%%        Year component of current date\n"
				"  %%DateMonth%%       Month component of current date\n"
				"  %%DateDay%%         Day component of current date\n"
				"  %%Time%%            Current time in format '%s'\n"
				"  %%TimeHour%%        Hour component of current time\n"
				"  %%TimeMinute%%      Minute component of current time\n"
				"  %%TimeSecond%%      Second component of current time\n"
				"  %%TimeMS%%          Milliseconds fraction of current time\n"
				"  %%Count%%           Number of screenshots taken this session\n"),
				g_target_executable_path.stem().u8string().c_str(),
#if RESHADE_FX
				_current_preset_path.stem().u8string().c_str(),
#else
				"..."
#endif
				"yyyy-MM-dd",
				"HH-mm-ss");
		}

		// HDR screenshots only support PNG, and have no alpha channel
		if (_back_buffer_format == reshade::api::format::r16g16b16a16_float ||
			_back_buffer_color_space == reshade::api::color_space::hdr10_st2084)
		{
			modified |= ImGui::SliderInt(_("HDR PNG quality"), reinterpret_cast<int *>(&_screenshot_hdr_bits), 7, 16, "%d bit", ImGuiSliderFlags_AlwaysClamp);
		}
		else
		{
			modified |= ImGui::Combo(_("Screenshot format"), reinterpret_cast<int *>(&_screenshot_format), "Bitmap (*.bmp)\0Portable Network Graphics (*.png)\0JPEG (*.jpeg)\0");

			if (_screenshot_format == 2)
				modified |= ImGui::SliderInt(_("JPEG quality"), reinterpret_cast<int *>(&_screenshot_jpeg_quality), 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
			else
				modified |= ImGui::Checkbox(_("Clear alpha channel"), &_screenshot_clear_alpha);
		}

#if RESHADE_FX
		modified |= ImGui::Checkbox(_("Save current preset file"), &_screenshot_include_preset);
		modified |= ImGui::Checkbox(_("Save before and after images"), &_screenshot_save_before);
#endif
   		modified |= ImGui::Checkbox(_("Save separate image with the overlay visible"), &_screenshot_save_ui);

		modified |= ImGui::Checkbox(_("NFS HUD on screenshot"), &_screenshot_nfs_hud);

		modified |= imgui::file_input_box(_("Screenshot sound"), "sound.wav", _screenshot_sound_path, _file_selection_path, { L".wav" });
		ImGui::SetItemTooltip(_("Audio file that is played when taking a screenshot."));

		modified |= imgui::file_input_box(_("Post-save command"), "command.bat", _screenshot_post_save_command, _file_selection_path, { L".exe", L".bat", L".cmd", L".ps1", L".py" });
		ImGui::SetItemTooltip(_(
			"Executable or script that is called after saving a screenshot.\n"
			"This can be used to perform additional processing on the image (e.g. compressing it with an image optimizer)."));

		char arguments[260];
		arguments[_screenshot_post_save_command_arguments.copy(arguments, sizeof(arguments) - 1)] = '\0';
		if (ImGui::InputText(_("Post-save command arguments"), arguments, sizeof(arguments)))
		{
			modified = true;
			_screenshot_post_save_command_arguments = arguments;
		}

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			const std::string extension = _screenshot_format == 0 ? ".bmp" : _screenshot_format == 1 ? ".png" : ".jpg";

			ImGui::SetTooltip(_(
				"Macros you can add that are resolved during command execution:\n"
				"  %%AppName%%         Name of the application (%s)\n"
				"  %%PresetName%%      File name without extension of the current preset file (%s)\n"
				"  %%BeforeAfter%%     Term describing the moment the screenshot was taken ('Before', 'After' or 'Overlay')\n"
				"  %%Date%%            Current date in format '%s'\n"
				"  %%DateYear%%        Year component of current date\n"
				"  %%DateMonth%%       Month component of current date\n"
				"  %%DateDay%%         Day component of current date\n"
				"  %%Time%%            Current time in format '%s'\n"
				"  %%TimeHour%%        Hour component of current time\n"
				"  %%TimeMinute%%      Minute component of current time\n"
				"  %%TimeSecond%%      Second component of current time\n"
				"  %%TimeMS%%          Milliseconds fraction of current time\n"
				"  %%TargetPath%%      Full path to the screenshot file (%s)\n"
				"  %%TargetDir%%       Full path to the screenshot directory (%s)\n"
				"  %%TargetFileName%%  File name of the screenshot file (%s)\n"
				"  %%TargetExt%%       File extension of the screenshot file (%s)\n"
				"  %%TargetName%%      File name without extension of the screenshot file (%s)\n"
				"  %%Count%%           Number of screenshots taken this session\n"),
				g_target_executable_path.stem().u8string().c_str(),
#if RESHADE_FX
				_current_preset_path.stem().u8string().c_str(),
#else
				"..."
#endif
				"yyyy-MM-dd",
				"HH-mm-ss",
				(_screenshot_path / (_screenshot_name + extension)).u8string().c_str(),
				_screenshot_path.u8string().c_str(),
				(_screenshot_name + extension).c_str(),
				extension.c_str(),
				_screenshot_name.c_str());
		}

		modified |= imgui::directory_input_box(_("Post-save command working directory"), _screenshot_post_save_command_working_directory, _file_selection_path);
		modified |= ImGui::Checkbox(_("Hide post-save command window"), &_screenshot_post_save_command_hide_window);
	}

	if (ImGui::CollapsingHeader(_("Overlay & Styling"), ImGuiTreeNodeFlags_DefaultOpen))
	{
#if RESHADE_LOCALIZATION
		{
			std::vector<std::string> languages = resources::get_languages();

			int lang_index = 0;
			if (const auto it = std::find(languages.begin(), languages.end(), _selected_language); it != languages.end())
				lang_index = static_cast<int>(std::distance(languages.begin(), it) + 1);

			if (ImGui::Combo(_("Language"), &lang_index,
					[](void *data, int idx) -> const char * {
						return idx == 0 ? "System Default" : (*static_cast<const std::vector<std::string> *>(data))[idx - 1].c_str();
					}, &languages, static_cast<int>(languages.size() + 1)))
			{
				modified = true;
				if (lang_index == 0)
					_selected_language.clear();
				else
					_selected_language = languages[lang_index - 1];
				// Rebuild font atlas in case language needs a special font or glyph range
				_imgui_context->IO.Fonts->TexReady = false;
			}
		}
#endif

#if RESHADE_FX
		if (ImGui::Button(_("Restart tutorial"), ImVec2(ImGui::CalcItemWidth(), 0)))
			_tutorial_index = 0;
#endif

		modified |= ImGui::Checkbox(_("Show screenshot message"), &_show_screenshot_message);

#if RESHADE_FX
		ImGui::BeginDisabled(_preset_transition_duration == 0);
		modified |= ImGui::Checkbox(_("Show preset transition message"), &_show_preset_transition_message);
		ImGui::EndDisabled();
		if (_preset_transition_duration == 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
			ImGui::SetTooltip(_("Preset transition duration has to be non-zero for the preset transition message to show up."));

		if (_effect_load_skipping)
			modified |= ImGui::Checkbox(_("Show \"Force load all effects\" button"), &_show_force_load_effects_button);
#endif

#if RESHADE_FX
		modified |= ImGui::Checkbox(_("Group effect files with tabs instead of a tree"), &_variable_editor_tabs);
#endif

		#pragma region Style
		if (ImGui::Combo(_("Global style"), &_style_index, "Dark\0Light\0Default\0Custom Simple\0Custom Advanced\0Solarized Dark\0Solarized Light\0"))
		{
			modified = true;
			load_custom_style();
		}

		if (_style_index == 3) // Custom Simple
		{
			ImVec4 *const colors = _imgui_context->Style.Colors;

			if (ImGui::BeginChild("##colors", ImVec2(0, 105), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_AlwaysVerticalScrollbar))
			{
				ImGui::PushItemWidth(-160);
				modified_custom_style |= ImGui::ColorEdit3("Background", &colors[ImGuiCol_WindowBg].x);
				modified_custom_style |= ImGui::ColorEdit3("ItemBackground", &colors[ImGuiCol_FrameBg].x);
				modified_custom_style |= ImGui::ColorEdit3("Text", &colors[ImGuiCol_Text].x);
				modified_custom_style |= ImGui::ColorEdit3("ActiveItem", &colors[ImGuiCol_ButtonActive].x);
				ImGui::PopItemWidth();
			}
			ImGui::EndChild();

			// Change all colors using the above as base
			if (modified_custom_style)
			{
				colors[ImGuiCol_PopupBg] = colors[ImGuiCol_WindowBg]; colors[ImGuiCol_PopupBg].w = 0.92f;

				colors[ImGuiCol_ChildBg] = colors[ImGuiCol_FrameBg]; colors[ImGuiCol_ChildBg].w = 0.00f;
				colors[ImGuiCol_MenuBarBg] = colors[ImGuiCol_FrameBg]; colors[ImGuiCol_MenuBarBg].w = 0.57f;
				colors[ImGuiCol_ScrollbarBg] = colors[ImGuiCol_FrameBg]; colors[ImGuiCol_ScrollbarBg].w = 1.00f;

				colors[ImGuiCol_TextDisabled] = colors[ImGuiCol_Text]; colors[ImGuiCol_TextDisabled].w = 0.58f;
				colors[ImGuiCol_Border] = colors[ImGuiCol_Text]; colors[ImGuiCol_Border].w = 0.30f;
				colors[ImGuiCol_Separator] = colors[ImGuiCol_Text]; colors[ImGuiCol_Separator].w = 0.32f;
				colors[ImGuiCol_SeparatorHovered] = colors[ImGuiCol_Text]; colors[ImGuiCol_SeparatorHovered].w = 0.78f;
				colors[ImGuiCol_SeparatorActive] = colors[ImGuiCol_Text]; colors[ImGuiCol_SeparatorActive].w = 1.00f;
				colors[ImGuiCol_PlotLines] = colors[ImGuiCol_Text]; colors[ImGuiCol_PlotLines].w = 0.63f;
				colors[ImGuiCol_PlotHistogram] = colors[ImGuiCol_Text]; colors[ImGuiCol_PlotHistogram].w = 0.63f;

				colors[ImGuiCol_FrameBgHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_FrameBgHovered].w = 0.68f;
				colors[ImGuiCol_FrameBgActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_FrameBgActive].w = 1.00f;
				colors[ImGuiCol_TitleBg] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_TitleBg].w = 0.45f;
				colors[ImGuiCol_TitleBgCollapsed] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_TitleBgCollapsed].w = 0.35f;
				colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_TitleBgActive].w = 0.58f;
				colors[ImGuiCol_ScrollbarGrab] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ScrollbarGrab].w = 0.31f;
				colors[ImGuiCol_ScrollbarGrabHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ScrollbarGrabHovered].w = 0.78f;
				colors[ImGuiCol_ScrollbarGrabActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ScrollbarGrabActive].w = 1.00f;
				colors[ImGuiCol_CheckMark] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_CheckMark].w = 0.80f;
				colors[ImGuiCol_SliderGrab] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_SliderGrab].w = 0.24f;
				colors[ImGuiCol_SliderGrabActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_SliderGrabActive].w = 1.00f;
				colors[ImGuiCol_Button] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_Button].w = 0.44f;
				colors[ImGuiCol_ButtonHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ButtonHovered].w = 0.86f;
				colors[ImGuiCol_Header] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_Header].w = 0.76f;
				colors[ImGuiCol_HeaderHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_HeaderHovered].w = 0.86f;
				colors[ImGuiCol_HeaderActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_HeaderActive].w = 1.00f;
				colors[ImGuiCol_ResizeGrip] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ResizeGrip].w = 0.20f;
				colors[ImGuiCol_ResizeGripHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ResizeGripHovered].w = 0.78f;
				colors[ImGuiCol_ResizeGripActive] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_ResizeGripActive].w = 1.00f;
				colors[ImGuiCol_PlotLinesHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_PlotLinesHovered].w = 1.00f;
				colors[ImGuiCol_PlotHistogramHovered] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_PlotHistogramHovered].w = 1.00f;
				colors[ImGuiCol_TextSelectedBg] = colors[ImGuiCol_ButtonActive]; colors[ImGuiCol_TextSelectedBg].w = 0.43f;

				colors[ImGuiCol_Tab] = colors[ImGuiCol_Button];
				colors[ImGuiCol_TabSelected] = colors[ImGuiCol_ButtonActive];
				colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_TabSelected];
				colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
				colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
				colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
				colors[ImGuiCol_TabDimmedSelectedOverline] = colors[ImGuiCol_TabDimmedSelected];
				colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_Header] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
				colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
			}
		}
		if (_style_index == 4) // Custom Advanced
		{
			if (ImGui::BeginChild("##colors", ImVec2(0, 300), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_AlwaysVerticalScrollbar))
			{
				ImGui::PushItemWidth(-160);
				for (ImGuiCol i = 0; i < ImGuiCol_COUNT; i++)
				{
					ImGui::PushID(i);
					modified_custom_style |= ImGui::ColorEdit4("##color", &_imgui_context->Style.Colors[i].x, ImGuiColorEditFlags_AlphaBar);
					ImGui::SameLine();
					ImGui::TextUnformatted(ImGui::GetStyleColorName(i));
					ImGui::PopID();
				}
				ImGui::PopItemWidth();
			}
			ImGui::EndChild();
		}
		#pragma endregion

		#pragma region Editor Style
		if (ImGui::Combo(_("Text editor style"), &_editor_style_index, "Dark\0Light\0Custom\0Solarized Dark\0Solarized Light\0"))
		{
			modified = true;
			load_custom_style();
		}

		if (_editor_style_index == 2)
		{
			if (ImGui::BeginChild("##editor_colors", ImVec2(0, 300), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_AlwaysVerticalScrollbar))
			{
				ImGui::PushItemWidth(-160);
				for (ImGuiCol i = 0; i < imgui::code_editor::color_palette_max; i++)
				{
					ImVec4 color = ImGui::ColorConvertU32ToFloat4(_editor_palette[i]);
					ImGui::PushID(i);
					modified_custom_style |= ImGui::ColorEdit4("##editor_color", &color.x, ImGuiColorEditFlags_AlphaBar);
					ImGui::SameLine();
					ImGui::TextUnformatted(imgui::code_editor::get_palette_color_name(i));
					ImGui::PopID();
					_editor_palette[i] = ImGui::ColorConvertFloat4ToU32(color);
				}
				ImGui::PopItemWidth();
			}
			ImGui::EndChild();
		}
		#pragma endregion

		if (imgui::font_input_box(_("Global font"), _default_font_path.empty() ? "ProggyClean.ttf" : _default_font_path.u8string().c_str(), _font_path, _file_selection_path, _font_size))
		{
			modified = true;
			_imgui_context->IO.Fonts->TexReady = false;
		}

		if (_imgui_context->IO.Fonts->Fonts[0]->ConfigDataCount > 2 && // Latin font + main font + icon font
			imgui::font_input_box(_("Latin font"), "ProggyClean.ttf", _latin_font_path, _file_selection_path, _font_size))
		{
			modified = true;
			_imgui_context->IO.Fonts->TexReady = false;
		}

		if (imgui::font_input_box(_("Text editor font"), _default_editor_font_path.empty() ? "ProggyClean.ttf" : _default_editor_font_path.u8string().c_str(), _editor_font_path, _file_selection_path, _editor_font_size))
		{
			modified = true;
			_imgui_context->IO.Fonts->TexReady = false;
		}

		if (float &alpha = _imgui_context->Style.Alpha; ImGui::SliderFloat(_("Global alpha"), &alpha, 0.1f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			// Prevent user from setting alpha to zero
			alpha = std::max(alpha, 0.1f);
			modified = true;
		}

		// Only show on possible HDR swap chains
		if (_back_buffer_format == reshade::api::format::r16g16b16a16_float ||
			_back_buffer_color_space == reshade::api::color_space::hdr10_st2084)
		{
			if (ImGui::SliderFloat(_("HDR overlay brightness"), &_hdr_overlay_brightness, 20.f, 400.f, "%.0f nits", ImGuiSliderFlags_AlwaysClamp))
				modified = true;

			if (ImGui::Combo(_("Overlay color space"), reinterpret_cast<int *>(&_hdr_overlay_overwrite_color_space), "Auto\0SDR\0scRGB\0HDR10\0HLG\0"))
				modified = true;
		}

		if (float &rounding = _imgui_context->Style.FrameRounding; ImGui::SliderFloat(_("Frame rounding"), &rounding, 0.0f, 12.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
		{
			// Apply the same rounding to everything
			_imgui_context->Style.WindowRounding = rounding;
			_imgui_context->Style.ChildRounding = rounding;
			_imgui_context->Style.PopupRounding = rounding;
			_imgui_context->Style.ScrollbarRounding = rounding;
			_imgui_context->Style.GrabRounding = rounding;
			_imgui_context->Style.TabRounding = rounding;
			modified = true;
		}

		if (!_is_vr)
		{
			ImGui::Spacing();

			ImGui::BeginGroup();
			modified |= imgui::checkbox_tristate(_("Show clock"), &_show_clock);
			ImGui::SameLine(0, 10);
			modified |= imgui::checkbox_tristate(_("Show FPS"), &_show_fps);
			ImGui::SameLine(0, 10);
			modified |= imgui::checkbox_tristate(_("Show frame time"), &_show_frametime);
#if RESHADE_FX
			modified |= imgui::checkbox_tristate(_("Show preset name"), &_show_preset_name);
#endif
			ImGui::EndGroup();
			ImGui::SetItemTooltip(_("Check to always show, fill out to only show while overlay is open."));

			if (_input != nullptr)
			{
				modified |= imgui::key_input_box(_("FPS key"), _fps_key_data, *_input);
				modified |= imgui::key_input_box(_("Frame time key"), _frametime_key_data, *_input);
			}

			if (_show_clock)
				modified |= ImGui::Combo(_("Clock format"), reinterpret_cast<int *>(&_clock_format), "HH:mm\0HH:mm:ss\0yyyy-MM-dd HH:mm:ss\0");

			modified |= ImGui::SliderFloat(_("OSD text size"), &_fps_scale, 0.2f, 2.5f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			modified |= ImGui::ColorEdit4(_("OSD text color"), _fps_col, ImGuiColorEditFlags_AlphaBar);

			std::string fps_pos_items = _("Top left\nTop right\nBottom left\nBottom right\n");
			std::replace(fps_pos_items.begin(), fps_pos_items.end(), '\n', '\0');
			modified |= ImGui::Combo(_("OSD position on screen"), reinterpret_cast<int *>(&_fps_pos), fps_pos_items.c_str());
		}
	}

	if (modified)
		save_config();
	if (modified_custom_style)
		save_custom_style();
}
void reshade::runtime::draw_gui_statistics()
{
	unsigned int gpu_digits = 1;
#if RESHADE_FX
	unsigned int cpu_digits = 1;
	uint64_t post_processing_time_cpu = 0;
	uint64_t post_processing_time_gpu = 0;

	if (!is_loading() && _effects_enabled)
	{
		for (const technique &tech : _techniques)
		{
			cpu_digits = std::max(cpu_digits, tech.average_cpu_duration >= 100'000'000 ? 3u : tech.average_cpu_duration >= 10'000'000 ? 2u : 1u);
			post_processing_time_cpu += tech.average_cpu_duration;
			gpu_digits = std::max(gpu_digits, tech.average_gpu_duration >= 100'000'000 ? 3u : tech.average_gpu_duration >= 10'000'000 ? 2u : 1u);
			post_processing_time_gpu += tech.average_gpu_duration;
		}
	}
#endif

	if (ImGui::CollapsingHeader(_("General"), ImGuiTreeNodeFlags_DefaultOpen))
	{
#if RESHADE_FX
		_gather_gpu_statistics = true;
#endif

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::PlotLines("##framerate",
			_imgui_context->FramerateSecPerFrame, static_cast<int>(std::size(_imgui_context->FramerateSecPerFrame)),
			_imgui_context->FramerateSecPerFrameIdx,
			nullptr,
			_imgui_context->FramerateSecPerFrameAccum / static_cast<int>(std::size(_imgui_context->FramerateSecPerFrame)) * 0.5f,
			_imgui_context->FramerateSecPerFrameAccum / static_cast<int>(std::size(_imgui_context->FramerateSecPerFrame)) * 1.5f,
			ImVec2(0, 50));

		const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		struct tm tm; localtime_s(&tm, &t);

		ImGui::BeginGroup();

		ImGui::TextUnformatted(_("API:"));
		ImGui::TextUnformatted(_("Hardware:"));
		ImGui::TextUnformatted(_("Application:"));
		ImGui::TextUnformatted(_("Time:"));
#if RESHADE_FX
		ImGui::TextUnformatted(_("Resolution:"));
#endif
		ImGui::Text(_("Frame %llu:"), _frame_count + 1);
#if RESHADE_FX
		ImGui::TextUnformatted(_("Post-Processing:"));
#endif

		ImGui::EndGroup();
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.33333333f);
		ImGui::BeginGroup();

		const char *api_name = "Unknown";
		switch (_device->get_api())
		{
		case api::device_api::d3d9:
			api_name = "D3D9";
			break;
		case api::device_api::d3d10:
			api_name = "D3D10";
			break;
		case api::device_api::d3d11:
			api_name = "D3D11";
			break;
		case api::device_api::d3d12:
			api_name = "D3D12";
			break;
		case api::device_api::opengl:
			api_name = "OpenGL";
			break;
		case api::device_api::vulkan:
			api_name = "Vulkan";
			break;
		}

		ImGui::TextUnformatted(api_name);
		if (_vendor_id != 0)
			ImGui::Text("VEN_%X", _vendor_id);
		else
			ImGui::TextUnformatted("Unknown");
		ImGui::TextUnformatted(g_target_executable_path.filename().u8string().c_str());
		ImGui::Text("%.4d-%.2d-%.2d %d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
#if RESHADE_FX
		ImGui::Text("%ux%u", _effect_permutations[0].width, _effect_permutations[0].height);
#endif
		ImGui::Text("%.2f fps", _imgui_context->IO.Framerate);
#if RESHADE_FX
		ImGui::Text("%*.3f ms CPU", cpu_digits + 4, post_processing_time_cpu * 1e-6f);
#endif

		ImGui::EndGroup();
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.66666666f);
		ImGui::BeginGroup();

		ImGui::Text("0x%X", _renderer_id);
		if (_device_id != 0)
			ImGui::Text("DEV_%X", _device_id);
		else
			ImGui::TextUnformatted("Unknown");
		ImGui::Text("0x%X", static_cast<unsigned int>(std::hash<std::string>()(g_target_executable_path.stem().u8string()) & 0xFFFFFFFF));
		ImGui::Text("%.0f ms", std::chrono::duration_cast<std::chrono::nanoseconds>(_last_present_time - _start_time).count() * 1e-6f);
#if RESHADE_FX
		ImGui::Text("Format %u (%u bpc)", static_cast<unsigned int>(_effect_permutations[0].color_format), api::format_bit_depth(_effect_permutations[0].color_format));
#endif
		ImGui::Text("%*.3f ms", gpu_digits + 4, _last_frame_duration.count() * 1e-6f);
#if RESHADE_FX
		if (_gather_gpu_statistics && post_processing_time_gpu != 0)
			ImGui::Text("%*.3f ms GPU", gpu_digits + 4, (post_processing_time_gpu * 1e-6f));
#endif

		ImGui::EndGroup();
	}

#if RESHADE_FX
	if (ImGui::CollapsingHeader(_("Techniques"), ImGuiTreeNodeFlags_DefaultOpen) && !is_loading() && _effects_enabled)
	{
		// Only need to gather GPU statistics if the statistics are actually visible
		_gather_gpu_statistics = true;

		ImGui::BeginGroup();

		std::vector<bool> long_technique_name(_techniques.size());
		for (size_t technique_index : _technique_sorting)
		{
			const reshade::technique &tech = _techniques[technique_index];

			if (!tech.enabled)
				continue;

			if (tech.permutations[0].passes.size() > 1)
				ImGui::Text("%s (%zu passes)", tech.name.c_str(), tech.permutations[0].passes.size());
			else
				ImGui::TextUnformatted(tech.name.c_str(), tech.name.c_str() + tech.name.size());

			long_technique_name[technique_index] = (ImGui::GetItemRectSize().x + 10.0f) > (ImGui::GetWindowWidth() * 0.33333333f);
			if (long_technique_name[technique_index])
				ImGui::NewLine();
		}

		ImGui::EndGroup();
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.33333333f);
		ImGui::BeginGroup();

		for (size_t technique_index : _technique_sorting)
		{
			const reshade::technique &tech = _techniques[technique_index];

			if (!tech.enabled)
				continue;

			if (long_technique_name[technique_index])
				ImGui::NewLine();

			if (tech.average_cpu_duration != 0)
				ImGui::Text("%*.3f ms CPU", cpu_digits + 4, tech.average_cpu_duration * 1e-6f);
			else
				ImGui::NewLine();
		}

		ImGui::EndGroup();
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.66666666f);
		ImGui::BeginGroup();

		for (size_t technique_index : _technique_sorting)
		{
			const reshade::technique &tech = _techniques[technique_index];

			if (!tech.enabled)
				continue;

			if (long_technique_name[technique_index])
				ImGui::NewLine();

			// GPU timings are not available for all APIs
			if (_gather_gpu_statistics && tech.average_gpu_duration != 0)
				ImGui::Text("%*.3f ms GPU", gpu_digits + 4, tech.average_gpu_duration * 1e-6f);
			else
				ImGui::NewLine();
		}

		ImGui::EndGroup();
	}

	if (ImGui::CollapsingHeader(_("Render Targets & Textures"), ImGuiTreeNodeFlags_DefaultOpen) && !is_loading())
	{
		static const char *texture_formats[] = {
			"unknown",
			"R8", "R16", "R16F", "R32I", "R32U", "R32F", "RG8", "RG16", "RG16F", "RG32F", "RGBA8", "RGBA16", "RGBA16F", "RGBA32I", "RGBA32U", "RGBA32F", "RGB10A2"
		};
		static constexpr uint32_t pixel_sizes[] = {
			0,
			1 /*R8*/, 2 /*R16*/, 2 /*R16F*/, 4 /*R32I*/, 4 /*R32U*/, 4 /*R32F*/, 2 /*RG8*/, 4 /*RG16*/, 4 /*RG16F*/, 8 /*RG32F*/, 4 /*RGBA8*/, 8 /*RGBA16*/, 8 /*RGBA16F*/, 16 /*RGBA32I*/, 16 /*RGBA32U*/, 16 /*RGBA32F*/, 4 /*RGB10A2*/
		};

		static_assert((std::size(texture_formats) - 1) == static_cast<size_t>(reshadefx::texture_format::rgb10a2));

		const float total_width = ImGui::GetContentRegionAvail().x;
		int texture_index = 0;
		const unsigned int num_columns = std::max(1u, static_cast<unsigned int>(std::ceil(total_width / (55.0f * _font_size))));
		const float single_image_width = (total_width / num_columns) - 5.0f;

		// Variables used to calculate memory size of textures
		lldiv_t memory_view;
		int64_t post_processing_memory_size = 0;
		const char *memory_size_unit;

		for (const texture &tex : _textures)
		{
			if (tex.resource == 0 || !tex.semantic.empty() ||
				!std::any_of(tex.shared.cbegin(), tex.shared.cend(),
					[this](size_t effect_index) { return _effects[effect_index].rendering; }))
				continue;

			ImGui::PushID(texture_index);
			ImGui::BeginGroup();

			int64_t memory_size = 0;
			for (uint32_t level = 0, width = tex.width, height = tex.height, depth = tex.depth; level < tex.levels; ++level, width /= 2, height /= 2, depth /= 2)
				memory_size += static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(depth) * pixel_sizes[static_cast<int>(tex.format)];

			post_processing_memory_size += memory_size;

			if (memory_size >= 1024 * 1024)
			{
				memory_view = std::lldiv(memory_size, 1024 * 1024);
				memory_view.rem /= 1000;
				memory_size_unit = "MiB";
			}
			else
			{
				memory_view = std::lldiv(memory_size, 1024);
				memory_size_unit = "KiB";
			}

			ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s%s", tex.unique_name.c_str(), tex.shared.size() > 1 ? " (pooled)" : "");
			switch (tex.type)
			{
			case reshadefx::texture_type::texture_1d:
				ImGui::Text("%u | %u mipmap(s) | %s | %lld.%03lld %s",
					tex.width,
					tex.levels - 1,
					texture_formats[static_cast<int>(tex.format)],
					memory_view.quot, memory_view.rem, memory_size_unit);
				break;
			case reshadefx::texture_type::texture_2d:
				ImGui::Text("%ux%u | %u mipmap(s) | %s | %lld.%03lld %s",
					tex.width,
					tex.height,
					tex.levels - 1,
					texture_formats[static_cast<int>(tex.format)],
					memory_view.quot, memory_view.rem, memory_size_unit);
				break;
			case reshadefx::texture_type::texture_3d:
				ImGui::Text("%ux%ux%u | %u mipmap(s) | %s | %lld.%03lld %s",
					tex.width,
					tex.height,
					tex.depth,
					tex.levels - 1,
					texture_formats[static_cast<int>(tex.format)],
					memory_view.quot, memory_view.rem, memory_size_unit);
				break;
			}

			size_t num_referenced_passes = 0;
			std::vector<std::pair<size_t, std::vector<std::string>>> references;
			for (const technique &tech : _techniques)
			{
				if (std::find(tex.shared.cbegin(), tex.shared.cend(), tech.effect_index) == tex.shared.cend())
					continue;

				std::pair<size_t, std::vector<std::string>> &reference = references.emplace_back();
				reference.first = tech.effect_index;

				for (size_t pass_index = 0; pass_index < tech.permutations[0].passes.size(); ++pass_index)
				{
					std::string pass_name = tech.permutations[0].passes[pass_index].name;
					if (pass_name.empty())
						pass_name = "pass " + std::to_string(pass_index);
					pass_name = tech.name + ' ' + pass_name;

					bool referenced = false;
					for (const reshadefx::texture_binding &binding : tech.permutations[0].passes[pass_index].texture_bindings)
					{
						if (_effects[tech.effect_index].permutations[0].module.samplers[binding.index].texture_name == tex.unique_name)
						{
							referenced = true;
							reference.second.emplace_back(pass_name + " (sampler)");
							break;
						}
					}

					for (const reshadefx::storage_binding &binding : tech.permutations[0].passes[pass_index].storage_bindings)
					{
						if (_effects[tech.effect_index].permutations[0].module.storages[binding.index].texture_name == tex.unique_name)
						{
							referenced = true;
							reference.second.emplace_back(pass_name + " (storage)");
							break;
						}
					}

					for (const std::string &render_target : tech.permutations[0].passes[pass_index].render_target_names)
					{
						if (render_target == tex.unique_name)
						{
							referenced = true;
							reference.second.emplace_back(pass_name + " (render target)");
							break;
						}
					}

					if (referenced)
						num_referenced_passes++;
				}
			}

			const bool supports_saving =
				tex.type != reshadefx::texture_type::texture_3d && (
				tex.format == reshadefx::texture_format::r8 ||
				tex.format == reshadefx::texture_format::rg8 ||
				tex.format == reshadefx::texture_format::rgba8 ||
				tex.format == reshadefx::texture_format::rgb10a2);

			const float button_size = ImGui::GetFrameHeight();
			const float button_spacing = _imgui_context->Style.ItemInnerSpacing.x;
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
			if (const std::string label = "Referenced by " + std::to_string(num_referenced_passes) + " pass(es) in " + std::to_string(tex.shared.size()) + " effect(s) ...";
				ImGui::ButtonEx(label.c_str(), ImVec2(single_image_width - (supports_saving ? button_spacing + button_size : 0), 0)))
				ImGui::OpenPopup("##references");
			if (supports_saving)
			{
				ImGui::SameLine(0, button_spacing);
				if (ImGui::Button(ICON_FK_FLOPPY, ImVec2(button_size, 0)))
					save_texture(tex);
				ImGui::SetItemTooltip(_("Save %s"), tex.unique_name.c_str());
			}
			ImGui::PopStyleVar();

			if (!references.empty() && ImGui::BeginPopup("##references"))
			{
				bool is_open = false;
				size_t effect_index = std::numeric_limits<size_t>::max();

				for (const std::pair<size_t, std::vector<std::string>> &reference : references)
				{
					if (reference.first != effect_index)
					{
						effect_index = reference.first;
						is_open = ImGui::TreeNodeEx(_effects[effect_index].source_file.filename().u8string().c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
					}

					if (is_open)
					{
						for (const std::string &pass : reference.second)
						{
							ImGui::Dummy(ImVec2(_imgui_context->Style.IndentSpacing, 0.0f));
							ImGui::SameLine(0.0f, 0.0f);
							ImGui::TextUnformatted(pass.c_str(), pass.c_str() + pass.size());
						}
					}
				}

				ImGui::EndPopup();
			}

			if (tex.type == reshadefx::texture_type::texture_2d)
			{
				if (bool check = _preview_texture == tex.srv[0] && _preview_size[0] == 0; ImGui::RadioButton(_("Preview scaled"), check))
				{
					_preview_size[0] = 0;
					_preview_size[1] = 0;
					_preview_texture = !check ? tex.srv[0] : api::resource_view { 0 };
				}
				ImGui::SameLine();
				if (bool check = _preview_texture == tex.srv[0] && _preview_size[0] != 0; ImGui::RadioButton(_("Preview original"), check))
				{
					_preview_size[0] = tex.width;
					_preview_size[1] = tex.height;
					_preview_texture = !check ? tex.srv[0] : api::resource_view { 0 };
				}

				bool r = (_preview_size[2] & 0x000000FF) != 0;
				bool g = (_preview_size[2] & 0x0000FF00) != 0;
				bool b = (_preview_size[2] & 0x00FF0000) != 0;
				ImGui::SameLine();
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(_imgui_context->Style.FramePadding.x, 0));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 0, 0, 1));
				imgui::toggle_button("R", r, 0.0f, ImGuiButtonFlags_AlignTextBaseLine);
				ImGui::PopStyleColor();
				if (tex.format >= reshadefx::texture_format::rg8)
				{
					ImGui::SameLine(0, 1);
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 1, 0, 1));
					imgui::toggle_button("G", g, 0.0f, ImGuiButtonFlags_AlignTextBaseLine);
					ImGui::PopStyleColor();
					if (tex.format >= reshadefx::texture_format::rgba8)
					{
						ImGui::SameLine(0, 1);
						ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 1, 1));
						imgui::toggle_button("B", b, 0.0f, ImGuiButtonFlags_AlignTextBaseLine);
						ImGui::PopStyleColor();
					}
				}
				ImGui::PopStyleVar();
				_preview_size[2] = (r ? 0x000000FF : 0) | (g ? 0x0000FF00 : 0) | (b ? 0x00FF0000 : 0) | 0xFF000000;

				const float aspect_ratio = static_cast<float>(tex.width) / static_cast<float>(tex.height);
				imgui::image_with_checkerboard_background(tex.srv[0].handle, ImVec2(single_image_width, single_image_width / aspect_ratio), _preview_size[2]);
			}

			ImGui::EndGroup();
			ImGui::PopID();

			if ((texture_index++ % num_columns) != (num_columns - 1))
				ImGui::SameLine(0.0f, 5.0f);
			else
				ImGui::Spacing();
		}

		if ((texture_index % num_columns) != 0)
			ImGui::NewLine(); // Reset ImGui::SameLine() so the following starts on a new line

		ImGui::Separator();

		if (post_processing_memory_size >= 1024 * 1024)
		{
			memory_view = std::lldiv(post_processing_memory_size, 1024 * 1024);
			memory_view.rem /= 1000;
			memory_size_unit = "MiB";
		}
		else
		{
			memory_view = std::lldiv(post_processing_memory_size, 1024);
			memory_size_unit = "KiB";
		}

		ImGui::Text(_("Total memory usage: %lld.%03lld %s"), memory_view.quot, memory_view.rem, memory_size_unit);
	}
#endif
}
void reshade::runtime::draw_gui_log()
{
	std::error_code ec;
	std::filesystem::path log_path = global_config().path();
	log_path.replace_extension(L".log");

	const bool filter_changed = imgui::search_input_box(_log_filter, sizeof(_log_filter), -(16.0f * _font_size + 2 * _imgui_context->Style.ItemSpacing.x));

	ImGui::SameLine();

	imgui::toggle_button(_("Word Wrap"), _log_wordwrap, 8.0f * _font_size);

	ImGui::SameLine();

	if (ImGui::Button(_("Clear Log"), ImVec2(8.0f * _font_size, 0.0f)))
		// Close and open the stream again, which will clear the file too
		log::open_log_file(log_path, ec);

	ImGui::Spacing();

	if (ImGui::BeginChild("##log", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y)), ImGuiChildFlags_Borders, _log_wordwrap ? 0 : ImGuiWindowFlags_AlwaysHorizontalScrollbar))
	{
		const uintmax_t file_size = std::filesystem::file_size(log_path, ec);
		if (filter_changed || _last_log_size != file_size)
		{
			_log_lines.clear();

			if (FILE *const file = _wfsopen(log_path.c_str(), L"r", SH_DENYNO))
			{
				char line_data[2048];
				while (fgets(line_data, sizeof(line_data), file))
					if (string_contains(line_data, _log_filter))
						_log_lines.push_back(line_data);

				fclose(file);
			}

			_last_log_size = file_size;
		}

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(_log_lines.size()), ImGui::GetTextLineHeightWithSpacing());
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				ImVec4 textcol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

				if (_log_lines[i].find("ERROR |") != std::string::npos || _log_lines[i].find("error") != std::string::npos)
					textcol = COLOR_RED;
				else if (_log_lines[i].find("WARN  |") != std::string::npos || _log_lines[i].find("warning") != std::string::npos)
					textcol = COLOR_YELLOW;
				else if (_log_lines[i].find("DEBUG |") != std::string::npos)
					textcol = ImColor(100, 100, 255);

				if (_log_wordwrap)
					ImGui::PushTextWrapPos();
				ImGui::PushStyleColor(ImGuiCol_Text, textcol);
				ImGui::TextUnformatted(_log_lines[i].c_str(), _log_lines[i].c_str() + _log_lines[i].size());
				ImGui::PopStyleColor();
				if (_log_wordwrap)
					ImGui::PopTextWrapPos();
			}
		}
	}
	ImGui::EndChild();

	ImGui::Spacing();

	if (ImGui::Button((ICON_FK_FOLDER " " + std::string(_("Open folder in explorer"))).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		utils::open_explorer(log_path);
}
void reshade::runtime::draw_gui_about()
{
	ImGui::TextUnformatted("ReShade " VERSION_STRING_PRODUCT);

	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("https://reshade.me").x, ImGui::GetStyle().ItemSpacing.x);
	ImGui::TextLinkOpenURL("https://reshade.me");

	ImGui::Separator();

	ImGui::PushTextWrapPos();

	ImGui::TextUnformatted(_("Developed and maintained by crosire."));
	ImGui::TextUnformatted(_("This project makes use of several open source libraries, licenses of which are listed below:"));

	if (ImGui::CollapsingHeader("ReShade", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_RESHADE);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("MinHook"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_MINHOOK);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("Dear ImGui"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_IMGUI);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("ImGuiColorTextEdit"))
	{
		ImGui::TextUnformatted("Copyright (C) 2017 BalazsJako\
\
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\
\
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\
\
THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.");
	}
	if (ImGui::CollapsingHeader("gl3w"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_GL3W);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("UTF8-CPP"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_UTFCPP);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("stb_image, stb_image_write"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_STB);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("DDS loading from SOIL"))
	{
		ImGui::TextUnformatted("Jonathan \"lonesock\" Dummer");
	}
	if (ImGui::CollapsingHeader("fpng"))
	{
		ImGui::TextUnformatted("Public Domain (https://github.com/richgel999/fpng)");
	}
	if (ImGui::CollapsingHeader("SPIR-V"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_SPIRV);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("Vulkan & Vulkan-Loader"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_VULKAN);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("Vulkan Memory Allocator"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_VMA);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("OpenVR"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_OPENVR);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("OpenXR"))
	{
		const resources::data_resource resource = resources::load_data_resource(IDR_LICENSE_OPENXR);
		ImGui::TextUnformatted(static_cast<const char *>(resource.data), static_cast<const char *>(resource.data) + resource.data_size);
	}
	if (ImGui::CollapsingHeader("Solarized"))
	{
		ImGui::TextUnformatted("Copyright (C) 2011 Ethan Schoonover\
\
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\
\
The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\
\
THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.");
	}
	if (ImGui::CollapsingHeader("Fork Awesome"))
	{
		ImGui::TextUnformatted("Copyright (C) 2018 Fork Awesome (https://forkawesome.github.io)\
\
This Font Software is licensed under the SIL Open Font License, Version 1.1. (http://scripts.sil.org/OFL)");
	}

	ImGui::PopTextWrapPos();
}


// NFS DEBUG/TWEAK MENU
// NFS CODE START
struct bVector3 // same as UMath::Vector3 anyways...
{
	float x;
	float y;
	float z;
};

struct bVector4
{
	float x;
	float y;
	float z;
	float w;
};

struct bMatrix4
{
	bVector4 v0;
	bVector4 v1;
	bVector4 v2;
	bVector4 v3;
};

// math stuff for RigidBody rotations...
void SetZRot(bMatrix4* dest, float zangle)
{
	float v3;
	float v4;
	float v5;
	float v6;
	float v7;
	bMatrix4* v8;
	int v9;

	v3 = (zangle * 6.2831855);
	v4 = cos(v3);
	v5 = v3;
	v6 = v4;
	v7 = sin(v5);
	v8 = dest;
	v9 = 16;
	do
	{
		v8->v0.x = 0.0;
		v8 = (bMatrix4*)((char*)v8 + 4);
		--v9;
	} while (v9);
	dest->v1.y = v6;
	dest->v0.x = v6;
	dest->v0.y = v7;
	dest->v1.x = -(float)v7;
	dest->v2.z = 1.0;
	dest->v3.w = 1.0;
}

void Matrix4Multiply(bMatrix4* m1, bMatrix4* m2, bMatrix4* result)
{
	float matrix1[4][4] = { 0 };
	float matrix2[4][4] = { 0 };
	float resulta[4][4] = { 0 };

	memcpy(&matrix1, m1, sizeof(bMatrix4));
	memcpy(&matrix2, m2, sizeof(bMatrix4));

	for (int i = 0; i < 4; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			for (int k = 0; k < 4; ++k)
			{
				resulta[i][j] += matrix1[i][k] * matrix2[k][j];
			}
		}
	}
	memcpy(result, &resulta, sizeof(bMatrix4));
}

enum GameFlowState
{
	GAMEFLOW_STATE_NONE = 0,
	GAMEFLOW_STATE_LOADING_FRONTEND = 1,
	GAMEFLOW_STATE_UNLOADING_FRONTEND = 2,
	GAMEFLOW_STATE_IN_FRONTEND = 3,
	GAMEFLOW_STATE_LOADING_REGION = 4,
	GAMEFLOW_STATE_LOADING_TRACK = 5,
	GAMEFLOW_STATE_RACING = 6,
	GAMEFLOW_STATE_UNLOADING_TRACK = 7,
	GAMEFLOW_STATE_UNLOADING_REGION = 8,
	GAMEFLOW_STATE_EXIT_DEMO_DISC = 9,
};

enum GRace_Context // SkipFE Race Type
{
	kRaceContext_QuickRace = 0,
	kRaceContext_TimeTrial = 1,
	kRaceContext_Online = 2,
	kRaceContext_Career = 3,
	kRaceContext_Count = 4,
};

char GameFlowStateNames[10][35] = {
  "NONE",
  "LOADING FRONTEND",
  "UNLOADING FRONTEND",
  "IN FRONTEND",
  "LOADING REGION",
  "LOADING TRACK",
  "RACING",
  "UNLOADING TRACK",
  "UNLOADING REGION",
  "EXIT DEMO DISC",
};

char GRaceContextNames[kRaceContext_Count][24] // SkipFE Race Type
{
	"Quick Race",
	"Time Trial",
	"Online",
	"Career",
};
char SkipFERaceTypeDisplay[64];

char PrecullerModeNames[4][27] = {
	"Preculler Mode: Off",
	"Preculler Mode: On",
	"Preculler Mode: Blinking",
	"Preculler Mode: High Speed",
};

bVector3 TeleportPos = { 0 };

void(__thiscall* GameFlowManager_UnloadFrontend)(void* dis) = (void(__thiscall*)(void*))GAMEFLOWMGR_UNLOADFE_ADDR;
void(__thiscall* GameFlowManager_UnloadTrack)(void* dis) = (void(__thiscall*)(void*))GAMEFLOWMGR_UNLOADTRACK_ADDR;
void(__thiscall* GameFlowManager_LoadRegion)(void* dis) = (void(__thiscall*)(void*))GAMEFLOWMGR_LOADREGION_ADDR;
int SkipFETrackNum = DEFAULT_TRACK_NUM;

void(__stdcall* RaceStarter_StartSkipFERace)() = (void(__stdcall*)())STARTSKIPFERACE_ADDR;

#ifdef GAME_MW
void(__stdcall* BootFlowManager_Init)() = (void(__stdcall*)())BOOTFLOWMGR_INIT_ADDR;
#endif

bool OnlineEnabled_OldState;

#ifndef OLD_NFS
int ActiveHotPos = 0;
bool bTeleFloorSnap = false;
bool bTeleFloorSnap_OldState = false;

void(*Sim_SetStream)(bVector3* location, bool blocking) = (void(*)(bVector3*, bool))SIM_SETSTREAM_ADDR;
bool(__thiscall*WCollisionMgr_GetWorldHeightAtPointRigorous)(void* dis, bVector3* pt, float* height, bVector3* normal) = (bool(__thiscall*)(void*, bVector3*, float*, bVector3*))WCOLMGR_GETWORLDHEIGHT_ADDR;

char SkipFEPlayerCar[64] = { DEFAULT_PLAYERCAR };
char SkipFEPlayerCar2[64] = { DEFAULT_PLAYER2CAR };
#ifdef GAME_PS
char SkipFEPlayerCar3[64] = { DEFAULT_PLAYER3CAR };
char SkipFEPlayerCar4[64] = { DEFAULT_PLAYER4CAR };
char SkipFETurboSFX[64] = "default";
char SkipFEForceHubSelectionSet[64];
char SkipFEForceRaceSelectionSet[64];
char SkipFEForceNIS[64];
char SkipFEForceNISContext[64];
bool bCalledProStreetTele = false;
#endif
#ifdef GAME_MW
char SkipFEOpponentPresetRide[64];
#else
#ifdef GAME_CARBON
char SkipFEPlayerPresetRide[64];
char SkipFEWingmanPresetRide[64];
char SkipFEOpponentPresetRide0[64];
char SkipFEOpponentPresetRide1[64];
char SkipFEOpponentPresetRide2[64];
char SkipFEOpponentPresetRide3[64];
char SkipFEOpponentPresetRide4[64];
char SkipFEOpponentPresetRide5[64];
char SkipFEOpponentPresetRide6[64];
char SkipFEOpponentPresetRide7[64];
#endif
#endif

// camera stuff
void(__cdecl* CameraAI_SetAction)(int EVIEW_ID, const char* action) = (void(__cdecl*)(int, const char*))CAMERA_SETACTION_ADDR;

int(__thiscall* UTL_IList_Find)(void* dis, void* IList) = (int(__thiscall*)(void*, void*))UTL_ILIST_FIND_ADDR;
#ifdef HAS_COPS
bool bDebuggingHeat = false;
bool bSetFEDBHeat = false;
float DebugHeat = 1.0;
#ifndef GAME_UC
void(__thiscall* FECareerRecord_AdjustHeatOnEventWin)(void* dis) = (void(__thiscall*)(void*))HEATONEVENTWIN_ADDR;
void(__cdecl* AdjustStableHeat_EventWin)(int player) = (void(__cdecl*)(int))ADJUSTSTABLEHEAT_EVENTWIN_ADDR;
void __stdcall FECareerRecord_AdjustHeatOnEventWin_Hook()
{
	unsigned int TheThis = 0;
	_asm mov TheThis, ecx
	if (!bDebuggingHeat)
		return FECareerRecord_AdjustHeatOnEventWin((void*)TheThis);
	*(float*)(TheThis + CAREERHEAT_OFFSET) = DebugHeat;
}
#endif

void TriggerSetHeat()
{
	int FirstLocalPlayer = **(int**)PLAYER_LISTABLESET_ADDR;
	int LocalPlayerVtable = *(int*)(FirstLocalPlayer);
	int LocalPlayerSimable = 0;
	int PlayerInstance = 0;

	int(__thiscall * LocalPlayer_GetSimable)(void* dis);

	if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
	{
		LocalPlayer_GetSimable = (int(__thiscall*)(void*)) * (int*)(LocalPlayerVtable + GETSIMABLE_OFFSET);
		LocalPlayerSimable = LocalPlayer_GetSimable((void*)FirstLocalPlayer);

		PlayerInstance = UTL_IList_Find(*(void**)(LocalPlayerSimable + 4), (void*)IPERPETRATOR_HANDLE_ADDR);


		if (PlayerInstance)
		{
			int(__thiscall *AIPerpVehicle_SetHeat)(void* dis, float heat);
			AIPerpVehicle_SetHeat = (int(__thiscall*)(void*, float))*(int*)((*(int*)PlayerInstance) + PERP_SETHEAT_OFFSET);
			AIPerpVehicle_SetHeat((void*)PlayerInstance, DebugHeat);
		}

	}
#ifndef GAME_UC
	if (bSetFEDBHeat)
	{
		bDebuggingHeat = true;
		AdjustStableHeat_EventWin(0);
		bDebuggingHeat = false;
	}
#endif
}
#endif

// race finish stuff
void(__cdecl* Game_NotifyRaceFinished)(void* ISimable) = (void(__cdecl*)(void*))GAMENOTIFYRACEFINISHED_ADDR;
//void(__cdecl* Game_NotifyLapFinished)(void* ISimable, int unk) = (void(__cdecl*)(void*, int))GAMENOTIFYLAPFINISHED_ADDR;
void(__cdecl* Game_EnterPostRaceFlow)() = (void(__cdecl*)())GAMEENTERPOSTRACEFLOW_ADDR;

int ForceFinishRace() // TODO: fix this, it's broken, either crashes or half-works
{
	int FirstLocalPlayer = **(int**)PLAYER_LISTABLESET_ADDR;
	int LocalPlayerVtable = *(int*)(FirstLocalPlayer);
	int LocalPlayerSimable = 0;

	int(__thiscall * LocalPlayer_GetSimable)(void* dis);

	if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
	{
		LocalPlayer_GetSimable = (int(__thiscall*)(void*)) * (int*)(LocalPlayerVtable + GETSIMABLE_OFFSET);
		LocalPlayerSimable = LocalPlayer_GetSimable((void*)FirstLocalPlayer);
		//Game_NotifyLapFinished((void*)LocalPlayerSimable, FinishParam);
		Game_NotifyRaceFinished((void*)LocalPlayerSimable);
		Game_EnterPostRaceFlow();
	}
	return 0;
}
#ifdef GAME_UC
void FlipCar()
{
	int FirstVehicle = **(int**)VEHICLE_LISTABLESET_ADDR;

	int RigidBodyInstance = 0;
	int RigidBodyVtable = 0;

	bMatrix4 rotor = { 0 };
	bMatrix4 result = { 0 };
	bMatrix4* GetMatrixRes = NULL;

	RigidBodyInstance = UTL_IList_Find(*(void**)(FirstVehicle + 4), (void*)IRIGIDBODY_HANDLE_ADDR);


	if (RigidBodyInstance)
	{
		RigidBodyVtable = *(int*)(RigidBodyInstance);
		bMatrix4*(__thiscall * RigidBody_GetMatrix4)(void* dis);
		RigidBody_GetMatrix4 = (bMatrix4*(__thiscall*)(void*)) * (int*)(RigidBodyVtable + RB_GETMATRIX4_OFFSET);

		int(__thiscall * RigidBody_SetOrientation)(void* dis, bMatrix4 * input);
		RigidBody_SetOrientation = (int(__thiscall*)(void*, bMatrix4*)) * (int*)(RigidBodyVtable + RB_SETORIENTATION_OFFSET);

		GetMatrixRes = RigidBody_GetMatrix4((void*)RigidBodyInstance);
		memcpy(&result, GetMatrixRes, 0x40);
		SetZRot(&rotor, 0.5);
		Matrix4Multiply(&result, &rotor, &result);
		RigidBody_SetOrientation((void*)RigidBodyInstance, &result);
	}
}
#else
void FlipCar()
{
	int FirstVehicle = **(int**)VEHICLE_LISTABLESET_ADDR;

	int RigidBodyInstance = 0;
	int RigidBodyVtable = 0;

	bMatrix4 rotor = { 0 };
	bMatrix4 result = { 0 };

	RigidBodyInstance = UTL_IList_Find(*(void**)(FirstVehicle + 4), (void*)IRIGIDBODY_HANDLE_ADDR);


	if (RigidBodyInstance)
	{
		RigidBodyVtable = *(int*)(RigidBodyInstance);
		int(__thiscall * RigidBody_GetMatrix4)(void* dis, bMatrix4* dest);
		RigidBody_GetMatrix4 = (int(__thiscall*)(void*, bMatrix4*)) *(int*)(RigidBodyVtable + RB_GETMATRIX4_OFFSET);

		int(__thiscall * RigidBody_SetOrientation)(void* dis, bMatrix4 * input);
		RigidBody_SetOrientation = (int(__thiscall*)(void*, bMatrix4*)) *(int*)(RigidBodyVtable + RB_SETORIENTATION_OFFSET);

		RigidBody_GetMatrix4((void*)RigidBodyInstance, &result);
		SetZRot(&rotor, 0.5);
		Matrix4Multiply(&result, &rotor, &result);
		RigidBody_SetOrientation((void*)RigidBodyInstance, &result);
	}
}
#endif

// overlay switch stuff that is only found in newer NFS games...
#ifndef GAME_UC
void(__thiscall* cFEng_QueuePackagePop)(void* dis, int num_to_pop) = (void(__thiscall*)(void*, int))FENG_QUEUEPACKAGEPOP_ADDR;
#ifdef GAME_MW
void(__thiscall* cFEng_QueuePackageSwitch)(void* dis, const char* pPackageName, int unk1, unsigned int unk2, bool) = (void(__thiscall*)(void*, const char*, int, unsigned int, bool))FENG_QUEUEPACKAGESWITCH_ADDR;
#else
void(__thiscall* cFEng_QueuePackageSwitch)(void* dis, const char* pPackageName, int pArg) = (void(__thiscall*)(void*, const char*, int))FENG_QUEUEPACKAGESWITCH_ADDR;
#endif
char CurrentOverlay[128] = { "ScreenPrintf.fng" };

void SwitchOverlay(char* overlay_name)
{
	cFEng_QueuePackagePop(*(void**)FENG_MINSTANCE_ADDR, 1);
#ifdef GAME_MW
	cFEng_QueuePackageSwitch(*(void**)FENG_MINSTANCE_ADDR, overlay_name, 0, 0, false);
#else
	cFEng_QueuePackageSwitch(*(void**)FENG_MINSTANCE_ADDR, overlay_name, 0);
#endif
}

unsigned int PlayerBin = 16;
#endif
void TriggerWatchCar(int type)
{
	//*(bool*)CAMERADEBUGWATCHCAR_ADDR = true;

	*(int*)MTOGGLECAR_ADDR = 0;
	*(int*)MTOGGLECARLIST_ADDR = type;
	*(bool*)CAMERADEBUGWATCHCAR_ADDR = !*(bool*)CAMERADEBUGWATCHCAR_ADDR;
	if (*(bool*)CAMERADEBUGWATCHCAR_ADDR)
		CameraAI_SetAction(1, "CDActionDebugWatchCar");
	else
		CameraAI_SetAction(1, "CDActionDrive");
}

#ifdef GAME_MW
void(__thiscall* EEnterBin_EEnterBin)(void* dis, int bin) = (void(__thiscall*)(void*, int))EENTERBIN_ADDR;
void*(__cdecl* Event_Alloc)(int size) = (void*(__cdecl*)(int))EVENTALLOC_ADDR;
char JumpToBinOptionText[32];

void JumpToBin(int bin)
{
	void* EventThingy = NULL;
	if (!*(int*)GRACESTATUS_ADDR && *(unsigned char*)CURRENTBIN_POINTER >= 2)
	{
		EEnterBin_EEnterBin(Event_Alloc(0xC), bin);
	}
}

void PlayMovie()
{
	if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_IN_FRONTEND)
		SwitchOverlay("FEAnyMovie.fng");
	if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
		SwitchOverlay("InGameAnyMovie.fng");

}

bVector3 VisualFilterColourPicker = {0.88, 0.80, 0.44};
float VisualFilterColourMultiR = 1.0;
float VisualFilterColourMultiG = 1.0;
float VisualFilterColourMultiB = 1.0;
enum IVisualTreatment_eVisualLookState
{
	HEAT_LOOK = 0,
	COPCAM_LOOK = 1,
	FE_LOOK = 2,
};

#else
char SkipFERaceID[64];

#ifndef GAME_UC
char MovieFilename[64];
void(__cdecl* FEAnyMovieScreen_PushMovie)(const char* package) = (void(__cdecl*)(const char*))PUSHMOVIE_ADDR;

void PlayMovie()
{
	if (*(bool*)ISMOVIEPLAYING_ADDR)
	{
		cFEng_QueuePackagePop(*(void**)FENG_MINSTANCE_ADDR, 1);
	}
	FEAnyMovieScreen_PushMovie(MovieFilename);
}
#endif
#endif

#ifdef GAME_CARBON
// infinite NOS
bool bInfiniteNOS = false;
bool(__thiscall* EasterEggCheck)(void* dis, int cheat) = (bool(__thiscall*)(void*, int))EASTEREGG_CHECK_FUNC;

bool __stdcall EasterEggCheck_Hook(int cheat)
{
	int TheThis = 0;
	_asm mov TheThis, ecx
	if (cheat == 0xA && bInfiniteNOS)
		return true;
	return EasterEggCheck((void*)TheThis, cheat);
}

char BossNames[FAKEBOSS_COUNT][8] = {
	"None",
	"Angie",
	"Darius",
	"Wolf",
	"Kenji",
	"Neville"
};
char BossNames_DisplayStr[64] = "Force Fake Boss: Unknown";

char FeCarPosition_Names[CAR_FEPOSITION_COUNT][28] = {
	"CarPosition_Main",
	"CarPosition_Muscle",
	"CarPosition_Tuner",
	"CarPosition_Exotic",
	"CarPosition_Generic",
	"CarPosition_CarClass",
	"CarPosition_CarLot_Muscle",
	"CarPosition_CarLot_Tuner",
	"CarPosition_CarLot_Exotic",
	"CarPosition_CarLot_Mazda"
};

char FeCarPosition_DisplayStr[64] = "FeLocation: Unknown";

int FeCarPosition = 0;

#endif

#if defined(GAME_MW) || defined(GAME_CARBON)

void __stdcall JumpToNewPos(bVector3* pos)
{
	int FirstLocalPlayer = **(int**)PLAYER_LISTABLESET_ADDR;
	int LocalPlayerVtable;
	bVector3 ActualTeleportPos = { -(*pos).y, (*pos).z, (*pos).x }; // Simables have coordinates in this format...
	float Height = (*pos).z;
	char WCollisionMgrSpace[0x20] = { 0 };

	void(__thiscall*LocalPlayer_SetPosition)(void* dis, bVector3 *position);

	if (FirstLocalPlayer)
	{
		Sim_SetStream(&ActualTeleportPos, true);

		if (bTeleFloorSnap)
		{
			if (!WCollisionMgr_GetWorldHeightAtPointRigorous(WCollisionMgrSpace, &ActualTeleportPos, &Height, NULL))
			{
				Height += 1.0;
			}
			ActualTeleportPos.y = Height; // actually Z in Simables
		}

		LocalPlayerVtable = *(int*)(FirstLocalPlayer);
		LocalPlayer_SetPosition = (void(__thiscall*)(void*, bVector3*))*(int*)(LocalPlayerVtable+0x10);
		LocalPlayer_SetPosition((void*)FirstLocalPlayer, &ActualTeleportPos);
	}
}

#else
// Undercover & ProStreet are special beings. They're actually multi threaded.
// We have to do teleporting during EMainService / World::Service (or same at least in the same thread as World updates), otherwise we cause hanging bugs...

bool bDoTeleport = false;
bVector3 ServiceTeleportPos = { 0 };

// Track loading stuff
bool bDoTrackUnloading = false;
bool bDoFEUnloading= false;
bool bDoLoadRegion = false;

bool bDoTriggerWatchCar = false;
int CarTypeToWatch = CARLIST_TYPE_AIRACER;

bool bDoFlipCar = false;

bool bDoOverlaySwitch = false;
bool bDoPlayMovie = false;

#ifdef GAME_UC
bool bDoAwardCash = false;
float CashToAward = 0.0;
void(__thiscall* GMW2Game_AwardCash)(void* dis, float cash, float unk) = (void(__thiscall*)(void*, float, float))GMW2GAME_AWARDCASH_ADDR;
bool bDisableCops = false;
#endif

void __stdcall JumpToNewPosPropagator(bVector3* pos)
{
	int FirstLocalPlayer = **(int**)PLAYER_LISTABLESET_ADDR;
	int LocalPlayerVtable;
	bVector3 ActualTeleportPos = { -(*pos).y, (*pos).z, (*pos).x }; // Simables have coordinates in this format...
	float Height = (*pos).z;
	char WCollisionMgrSpace[0x20] = { 0 };

	void(__thiscall * LocalPlayer_SetPosition)(void* dis, bVector3 * position);

	if (FirstLocalPlayer)
	{
		Sim_SetStream(&ActualTeleportPos, true);
		if (bTeleFloorSnap)
		{
			if (!WCollisionMgr_GetWorldHeightAtPointRigorous(WCollisionMgrSpace, &ActualTeleportPos, &Height, NULL))
			{
				Height += 1.0;
			}
			ActualTeleportPos.y = Height; // actually Z in Simables
		}

		LocalPlayerVtable = *(int*)(FirstLocalPlayer);
		LocalPlayer_SetPosition = (void(__thiscall*)(void*, bVector3*)) * (int*)(LocalPlayerVtable + 0x10);
		LocalPlayer_SetPosition((void*)FirstLocalPlayer, &ActualTeleportPos);
	}
}

void __stdcall JumpToNewPos(bVector3* pos)
{
	memcpy(&ServiceTeleportPos, pos, sizeof(bVector3));
	bDoTeleport = true;
}

void __stdcall MainService_Hook()
{
	if (bDoTeleport)
	{
#ifdef GAME_PS
		if (bCalledProStreetTele)
		{
			bTeleFloorSnap_OldState = bTeleFloorSnap;
			bTeleFloorSnap = true;
		}
#endif
		JumpToNewPosPropagator(&ServiceTeleportPos);
		bDoTeleport = false;
#ifdef GAME_PS
		if (bCalledProStreetTele)
			bTeleFloorSnap = bTeleFloorSnap_OldState;
#endif
	}
	if (bDoTrackUnloading)
	{
		GameFlowManager_UnloadTrack((void*)GAMEFLOWMGR_ADDR);
		bDoTrackUnloading = false;
	}
	if (bDoFEUnloading)
	{
		*(int*)SKIPFE_ADDR = 1;
		*(int*)SKIPFETRACKNUM_ADDR = SkipFETrackNum;
		GameFlowManager_UnloadFrontend((void*)GAMEFLOWMGR_ADDR);
		bDoFEUnloading = false;
	}
	if (bDoLoadRegion)
	{
		*(int*)SKIPFE_ADDR = 1;
		*(int*)SKIPFETRACKNUM_ADDR = SkipFETrackNum;
		GameFlowManager_LoadRegion((void*)GAMEFLOWMGR_ADDR);
		bDoLoadRegion = false;
	}
	if (bDoTriggerWatchCar)
	{
		TriggerWatchCar(CarTypeToWatch);
		bDoTriggerWatchCar = false;
	}

	if (bDoFlipCar)
	{
		FlipCar();
		bDoFlipCar = false;
	}
#ifdef GAME_UC
	if (bDoAwardCash)
	{
		GMW2Game_AwardCash(*(void**)GMW2GAME_OBJ_ADDR, CashToAward, 0.0);
		bDoAwardCash = false;
	}
#else
	if (bDoOverlaySwitch)
	{
		SwitchOverlay(CurrentOverlay);
		bDoOverlaySwitch = false;
	}

	if (bDoPlayMovie)
	{
		PlayMovie();
		bDoPlayMovie = false;
	}
#endif

}

#endif
#else
#ifdef GAME_UG2
int GlobalRainType = DEFAULT_RAIN_TYPE;

void(__thiscall* Car_ResetToPosition)(unsigned int dis, bVector3* position, float unk, short int angle, bool unk2) = (void(__thiscall*)(unsigned int, bVector3*, float, short int, bool))CAR_RESETTOPOS_ADDR;
int(__cdecl* eGetView)(int view) = (int(__cdecl*)(int))EGETVIEW_ADDR;
void(__thiscall* Rain_Init)(void* dis, int RainType, float unk) = (void(__thiscall*)(void*, int, float))RAININIT_ADDR;


void __stdcall SetRainBase_Custom()
{
	int ViewResult;
	ViewResult = eGetView(1);
	ViewResult = *(int*)(ViewResult + 0x64);
	*(float*)(ViewResult + 0x509C) = 1.0;

	ViewResult = eGetView(2);
	ViewResult = *(int*)(ViewResult + 0x64);
	*(float*)(ViewResult + 0x509C) = 1.0;

	ViewResult = eGetView(1);
	ViewResult = *(int*)(ViewResult + 0x64);
	Rain_Init((void*)ViewResult, GlobalRainType, 1.0);

	ViewResult = eGetView(2);
	ViewResult = *(int*)(ViewResult + 0x64);
	Rain_Init((void*)ViewResult, GlobalRainType, 1.0);
}

void __stdcall JumpToNewPos(bVector3* pos)
{
	int FirstLocalPlayer = *(int*)PLAYERBYINDEX_ADDR;

	if (FirstLocalPlayer)
	{
		Car_ResetToPosition(*(unsigned int*)(FirstLocalPlayer + 4), pos, 0, 0, false);
	}
}

void(__cdecl* FEngPushPackage)(const char* pkg_name, int unk) = (void(__cdecl*)(const char*, int))FENG_PUSHPACKAGE_ADDR;
void(__cdecl* FEngPopPackage)(const char* pkg_name) = (void(__cdecl*)(const char*))FENG_POPPACKAGE_ADDR;
//void(__cdecl* FEngSwitchPackage)(const char* pkg_name, const char* pkg_name2 ,int unk) = (void(__cdecl*)(const char*, const char*, int))FENG_SWITCHPACKAGE_ADDR;
char CurrentOverlay[64] = { "ScreenPrintf.fng" };
char OverlayToPush[64] = { "ScreenPrintf.fng" };

char* cFEng_FindPackageWithControl_Name()
{
	return *(char**)(*(int*)((*(int*)FENG_PINSTANCE_ADDR) + 0x10) + 0x8);
}

void SwitchOverlay(char* overlay_name)
{
	FEngPopPackage(cFEng_FindPackageWithControl_Name());
	strcpy(OverlayToPush, overlay_name);
	FEngPushPackage(OverlayToPush, 0);
	//FEngSwitchPackage(cFEng_FindPackageWithControl_Name(), overlay_name, 0);
}

#else
// Since Undergound 1 on PC was compiled with GOD AWFUL OPTIMIZATIONS, the actual function symbol definition does not match (like it does in *gasp* other platforms and Underground 2)
// Arguments are passed through registers... REGISTERS. ON x86!
// THIS ISN'T MIPS OR PPC FOR CRYING OUT LOUD
// What devilish compiler setting even is this?
// Anyhow, it's still a thiscall, except, get this, POSITION and ANGLE are passed through EDX and EAX respectively
// So it's not even in ORDER, it's ARGUMENT 1 and ARGUMENT 3
void(__thiscall* Car_ResetToPosition)(unsigned int dis, float unk, bool unk2) = (void(__thiscall*)(unsigned int, float, bool))CAR_RESETTOPOS_ADDR;

void __stdcall JumpToNewPos(bVector3* pos)
{
	int FirstLocalPlayer = *(int*)PLAYERBYINDEX_ADDR;

	if (FirstLocalPlayer)
	{
		_asm
		{
			mov edx, pos
			xor eax, eax
		}
		Car_ResetToPosition(*(unsigned int*)(FirstLocalPlayer + 4), 0, false);
	}
}
#endif

#endif

#ifdef GAME_PS
// ProStreet Caves, because a lot of code is either optimized/missing or SecuRom'd, this should in theory work with UC if it is necessary
// A necessary evil to bring some features back.
// Everything else is hooked cleanly, no caves.

bool bToggleAiControl = false;
//int AIControlCaveExit = 0x41D296;
int AIControlCaveExit2 = AICONTROL_CAVE_EXIT;
int UpdateWrongWay = UPDATEWRONGWAY_ADDR;
bool bAppliedSpeedLimiterPatches = false;

void __declspec(naked) ToggleAIControlCave()
{
	_asm
	{
		mov al, bToggleAiControl
		test al, al
		jz AIControlCaveExit
		mov eax, [edi + 0x1A38]
		lea ecx, [edi + 0x1A38]
		call dword ptr [eax+8]
		neg al
		sbb al, al
		inc al
		lea ecx, [edi + 0x1A38]
		push eax
		mov eax, [edi + 0x1A38]
		call dword ptr [eax+0xC]
		mov bToggleAiControl, 0
	AIControlCaveExit:
		push edi
		call UpdateWrongWay
		jmp AIControlCaveExit2
	}
}

bool bInfiniteNOS = false;
void __declspec(naked) InfiniteNOSCave()
{
	if (bInfiniteNOS)
		_asm mov eax, 0xFF

	_asm
	{
		mov[esi + 0x10C], eax
		pop esi
		add esp, 8
		retn 8
	}
}

bool bDrawWorld = true;
int DrawWorldCaveTrueExit = DRAWWORLD_CAVE_TRUE_EXIT;
int DrawWorldCaveFalseExit = DRAWWORLD_CAVE_FALSE_EXIT;
void __declspec(naked) DrawWorldCave()
{
	if (!bDrawWorld)
		_asm jmp DrawWorldCaveFalseExit
	_asm
	{
		mov eax, dword ptr [GAMEFLOWMGR_STATUS_ADDR]
		cmp eax, 4
		jz DWCF_label
		jmp DrawWorldCaveTrueExit
		DWCF_label:
		jmp DrawWorldCaveFalseExit
	}
}

float GameSpeed = 1.0;
float GameSpeedConstant = 1.0;

int GameSpeedCaveExit = GAMESPEED_CAVE_EXIT;
void __declspec(naked) GameSpeedCave()
{
	_asm
	{
		fld GameSpeedConstant
		fld GameSpeed
		mov esi, ecx
		fucompp
		fnstsw ax
		test ah, 0x44
		jnp GSCE_LABEL
		fld GameSpeed
		pop esi
		pop ebx
		add esp, 8
		retn 8
		GSCE_LABEL:
		jmp GameSpeedCaveExit
	}
}

int __stdcall RetZero()
{
	return 0;
}

void ApplySpeedLimiterPatches()
{
	injector::WriteMemory<unsigned int>(0x0040BE15, 0, true);
	injector::WriteMemory<unsigned int>(0x004887A3, 0, true);
	injector::WriteMemory<unsigned int>(0x00488AA9, 0, true);
	injector::WriteMemory<unsigned int>(0x00488AE3, 0, true);
	injector::WriteMemory<unsigned int>(0x00718B3F, 0, true);
	injector::WriteMemory<unsigned int>(0x0071E4E8, 0, true);
	injector::MakeJMP(0x00402820, RetZero, true);
}

void UndoSpeedLimiterPatches()
{
	injector::WriteMemory<unsigned int>(0x0040BE15, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x004887A3, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x00488AA9, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x00488AE3, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x00718B3F, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x0071E4E8, 0x0A2D9ECB4, true);
	injector::WriteMemory<unsigned int>(0x00402820, 0x66E9FF6A, true);
	injector::WriteMemory<unsigned int>(0x00402822, 0x0DCA66E9, true);
}

#endif
#ifdef GAME_UC
bool bInfiniteNOS = false;
int InfiniteNOSExitTrue = INFINITENOS_CAVE_EXIT_TRUE;
int InfiniteNOSExitFalse = INFINITENOS_CAVE_EXIT_FALSE;
int InfiniteNOSExitBE = INFINITENOS_CAVE_EXIT_BE;
void __declspec(naked) InfiniteNOSCave()
{
	_asm
	{
		jbe InfNosExitBE
		cmp bInfiniteNOS, 0
		jne InfNosExitTrue
		jmp InfiniteNOSExitFalse
	InfNosExitTrue:
		jmp InfiniteNOSExitTrue
	InfNosExitBE:
		jmp InfiniteNOSExitBE
	}

}

bool bToggleAiControl = false;
bool bBeTrafficCar = false;
//int AIControlCaveExit = AICONTROL_CAVE_EXIT;
int AIControlCaveExit2 = AICONTROL_CAVE_EXIT2;
int UpdateWrongWay = 0x0041B490;
int unk_sub_aiupdate = 0x0040EF30;
bool bAppliedSpeedLimiterPatches = false;


void __declspec(naked) ToggleAIControlCave()
{
	_asm
	{
		test bl, bl
		movss dword ptr[edi + 0x1510], xmm0
		jnz AIControlCaveExit2Label

		mov ecx, edi
		call UpdateWrongWay
		mov ecx, edi
		call unk_sub_aiupdate

	AIControlCaveExit2Label:
		mov al, bToggleAiControl
		test al, al
		jz AIControlCaveExitLabel
		mov eax, [edi + 0x14AC]
		lea ecx, [edi + 0x14AC]
		call dword ptr[eax + 8]
		neg al
		sbb al, al
		inc al
		lea ecx, [edi + 0x14AC]
		push bBeTrafficCar
		push eax
		mov eax, [edi + 0x14AC]
		call dword ptr[eax + 0xC]
		mov bToggleAiControl, 0
		lea ebp, [edi + 0x14AC]

	AIControlCaveExitLabel:
		jmp AIControlCaveExit2

	}
}



#endif

#ifdef HAS_FOG_CTRL
bVector3 FogColourPicker = {0.43, 0.41, 0.29};
#endif

void reshade::runtime::draw_gui_nfs()
{
	bool modified = false;

	ImGui::TextUnformatted("NFS Tweak Menu");
	ImGui::Separator();

	drawFrontEnd = *(bool*)DRAW_FENG_BOOL_ADDR;
	if (ImGui::Checkbox("Draw FrontEnd", &drawFrontEnd))
	{
		*(bool*)DRAW_FENG_BOOL_ADDR = drawFrontEnd;
	}

	ImGui::Separator();
	if (ImGui::CollapsingHeader("Front End", ImGuiTreeNodeFlags_None))
	{
#ifndef GAME_UG
		if (ImGui::CollapsingHeader("Safe House", ImGuiTreeNodeFlags_None))
		{
			ImGui::Checkbox("Unlock All Things", (bool*)UNLOCKALLTHINGS_ADDR);
#ifndef OLD_NFS
			ImGui::Checkbox("Car Guys Camera", (bool*)CARGUYSCAMERA_ADDR);
#ifdef GAME_MW
			if (*(int*)FEDATABASE_ADDR)
			{
				ImGui::InputInt("Player Cash", (int*)PLAYERCASH_POINTER, 1, 100, ImGuiInputTextFlags_None);
				PlayerBin = *(unsigned char*)CURRENTBIN_POINTER;
				if (ImGui::InputInt("Current Bin", (int*)&PlayerBin, 1, 100, ImGuiInputTextFlags_None))
				{
					*(unsigned char*)CURRENTBIN_POINTER = (PlayerBin & 0xFF);
				}
			}
#else
#ifndef GAME_UC
			if (*(int*)FEMANAGER_INSTANCE_ADDR)
			{
				ImGui::InputInt("Player Cash", (int*)PLAYERCASH_POINTER, 1, 100, ImGuiInputTextFlags_None);
				PlayerBin = *(unsigned char*)CURRENTBIN_POINTER;
				if (ImGui::InputInt("Current Bin", (int*)&PlayerBin, 1, 100, ImGuiInputTextFlags_None))
				{
					*(unsigned char*)CURRENTBIN_POINTER = (PlayerBin & 0xFF);
				}
#ifdef GAME_CARBON
				ImGui::InputInt("Player Rep", (int*)PLAYERREP_POINTER, 1, 100, ImGuiInputTextFlags_None);
				ImGui::InputText("Profile Name", (char*)PROFILENAME_POINTER, 31); // figure out the actual size, I assume 31 due to the memory layout

				ImGui::InputText("Crew Name", (char*)CREWNAME_POINTER, 15); // figure out the actual size, I assume 15 due to the memory layout
				FeCarPosition = *(unsigned char*)CAR_FEPOSITION_POINTER;
				sprintf(FeCarPosition_DisplayStr, "FeLocation: %s", FeCarPosition_Names[FeCarPosition]);
				if (ImGui::InputInt(FeCarPosition_DisplayStr, (int*)&FeCarPosition, 1, 100, ImGuiInputTextFlags_None))
				{
					FeCarPosition %= CAR_FEPOSITION_COUNT;
					if (FeCarPosition < 0)
						FeCarPosition = CAR_FEPOSITION_COUNT - 1;
					*(unsigned char*)CAR_FEPOSITION_POINTER = (FeCarPosition & 0xFF);
				}
#endif
				ImGui::Separator();
				ImGui::Text("User Profile pointer: 0x%X", USERPROFILE_POINTER);
#ifdef GAME_PS
				ImGui::Text("FECareer pointer: 0x%X", FECAREER_POINTER);
#endif
			}
#else
			if (*(int*)GMW2GAME_OBJ_ADDR)
			{
				ImGui::InputFloat("Player Cash Adjust", &CashToAward, 1.0, 1000.0, "%.1f", ImGuiInputTextFlags_CharsScientific);
				if (ImGui::Button("Adjust Cash", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bDoAwardCash = true;
				}
			}
#endif
#endif
#endif
#ifdef GAME_UG2
			if (ImGui::Button("Unlimited Casholaz", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				*(bool*)UNLIMITEDCASHOLAZ_ADDR = true;
			}
#endif
	}
#endif
		if (ImGui::CollapsingHeader("Printfs", ImGuiTreeNodeFlags_None))
		{
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted("NOTE: This doesn't actually do anything in the release builds yet. Only controls the variable.");
			ImGui::PopTextWrapPos();
			ImGui::Checkbox("Screen Printf", (bool*)DOSCREENPRINTF_ADDR);
			ImGui::Separator();
		}
#ifdef GAME_MW
		ImGui::Checkbox("Test Career Customization", (bool*)TESTCAREERCUSTOMIZATION_ADDR);
#endif
		ImGui::Checkbox("Show All Cars in FE", (bool*)SHOWALLCARSINFE_ADDR);
#ifndef OLD_NFS
#ifdef GAME_CARBON
		ImGui::Checkbox("Enable Debug Car Customize", (bool*)ENABLEDCC_ADDR);
#endif
#endif
#ifndef GAME_UG
#ifndef GAME_UC
		if (ImGui::CollapsingHeader("Overlays", ImGuiTreeNodeFlags_None))
		{
			ImGui::InputText("Manual overlay", CurrentOverlay, 128);
			if (ImGui::Button("Switch to the manual", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
#ifdef NFS_MULTITHREAD
				bDoOverlaySwitch = true;
#else
				SwitchOverlay(CurrentOverlay);
#endif
			}
#ifdef GAME_MW
			ImGui::Separator();
			if (ImGui::CollapsingHeader("Loading Screen", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Loading", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Loading.fng");
				if (ImGui::Button("Loading Controller", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Loading_Controller.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Main Menu", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("Main Menu", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("MainMenu.fng");
				if (ImGui::Button("Main Menu Sub", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("MainMenu_Sub.fng");
				if (ImGui::Button("Options", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Options.fng");
				if (ImGui::Button("Quick Race Brief", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Quick_Race_Brief.fng");
				if (ImGui::Button("Track Select", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Track_Select.fng");
				ImGui::Separator();
			}

			if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_RACING)
					ImGui::TextUnformatted("WARNING: You're not in race mode. The game might crash if you use these.");
				if (ImGui::Button("Pause Main", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Pause_Main.fng");
				if (ImGui::Button("Pause Performance Tuning", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Pause_Performance_Tuning.fng");
				if (ImGui::Button("World Map Main", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("WorldMapMain.fng");
				if (ImGui::Button("Pause Options", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Pause_Options.fng");
				if (ImGui::Button("InGame Reputation Overview", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("InGameReputationOverview.fng");
				if (ImGui::Button("InGame Milestones", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("InGameMilestones.fng");
				if (ImGui::Button("InGame Rival Challenge", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("InGameRivalChallenge.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Career", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("Safehouse Reputation Overview", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("SafehouseReputationOverview.fng");
				if (ImGui::Button("Rap Sheet Overview", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("RapSheetOverview.fng");
				if (ImGui::Button("Rap Sheet Rankings", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("RapSheetRankings.fng");
				if (ImGui::Button("Rap Sheet Infractions", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("RapSheetInfractions.fng");
				if (ImGui::Button("Rap Sheet Cost To State", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("RapSheetCostToState.fng");
				if (ImGui::Button("Safehouse Rival Challenge", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("SafehouseRivalChallenge.fng");
				if (ImGui::Button("Safehouse Milestones", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("SafehouseMilestones.fng");
				if (ImGui::Button("BlackList", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("BlackList.fng");
				if (ImGui::Button("Controller Unplugged", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("ControllerUnplugged.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Customization (Must be in Customized Car Screen)", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("My Cars Manager", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("MyCarsManager.fng");
				if (ImGui::Button("Debug Car Customize", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_DebugCarCustomize.fng");
				if (ImGui::Button("Customize Parts", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("CustomizeParts.fng");
				if (ImGui::Button("Customize Parts BACKROOM", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("CustomizeParts_BACKROOM.fng");
				if (ImGui::Button("Customize Category", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("CustomHUD.fng");
				if (ImGui::Button("Custom HUD BACKROOM", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("CustomHUD_BACKROOM.fng");
				if (ImGui::Button("Decals", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Decals.fng");
				if (ImGui::Button("Decals BACKROOM", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Decals_BACKROOM.fng");
				if (ImGui::Button("Numbers", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Numbers.fng");
				if (ImGui::Button("Rims BACKROOM", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Rims_BACKROOM.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Misc", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("Keyboard", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Keyboard.fng");
				if (ImGui::Button("LS LangSelect", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("LS_LangSelect.fng");
				if (ImGui::Button("Loading Controller", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("Loading_Controller.fng");
				if (ImGui::Button("UI_OptionsController", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OptionsController.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Online (Must be in ONLINE connected)", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("News and Terms", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_News_and_Terms.fng");
				if (ImGui::Button("Lobby Room", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_LobbyRoom.fng");
				if (ImGui::Button("Game Room", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLGameRoom.fng");
				if (ImGui::Button("Game Room host", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_GameRoom_Dialog.fng");
				if (ImGui::Button("Game Room client", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLGameRoom_client.fng");
				if (ImGui::Button("Mode Select List", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_ModeSelectList.fng");
				if (ImGui::Button("Online Main", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_MAIN.fng");
				if (ImGui::Button("Quick Race Main", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_Quickrace_Main.fng");
				if (ImGui::Button("Filters", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLFilters.fng");
				if (ImGui::Button("OptiMatch Available", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLX_OptiMatch_Available.fng");
				if (ImGui::Button("OptiMatch Filters", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLX_OptiMatch_Filters.fng");
				if (ImGui::Button("Rankings Personal", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLRankings_Personal.fng");
				if (ImGui::Button("Rankings Overall", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLRankings_Overall.fng");
				if (ImGui::Button("Rankings Monthly", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLRankings_Monthly.fng");
				if (ImGui::Button("Friend Dialogue", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_FriendDialogue.fng");
				if (ImGui::Button("Feedback", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_Feedback.fng");
				if (ImGui::Button("Voice Chat", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_VoiceChat.fng");
				if (ImGui::Button("Auth DNAS", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLAuthDNAS.fng");
				if (ImGui::Button("ISP Connect", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLISPConnect.fng");
				if (ImGui::Button("Select Persona", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_SelectPersona.fng");
				if (ImGui::Button("Create Account", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_Create_Account.fng");
				if (ImGui::Button("Age Verification", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLAgeVerif.fng");
				if (ImGui::Button("Age Too Young", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_OLAgeTooYoung.fng");
				if (ImGui::Button("Use Existing", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("OL_UseExisting.fng");
				if (ImGui::Button("Date Entry", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("UI_DateEntry.fng");
				ImGui::Separator();
			}
			if (ImGui::CollapsingHeader("Memory Card", ImGuiTreeNodeFlags_None))
			{
				if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_IN_FRONTEND)
					ImGui::TextUnformatted("WARNING: You're not in Front End. The game might crash if you use these.");
				if (ImGui::Button("Profile Manager", ImVec2(ImGui::CalcItemWidth(), 0)))
					SwitchOverlay("MC_ProfileManager.fng");
				ImGui::Separator();
			}
#endif
		}
#endif
#endif
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Career", ImGuiTreeNodeFlags_None))
	{
#ifdef GAME_UG2
		ImGui::Checkbox("Shut Up Rachel", (bool*)SHUTUPRACHEL_ADDR);
#endif
#ifndef OLD_NFS
		ImGui::Checkbox("Skip bin 15 intro", (bool*)SKIPCAREERINTRO_ADDR);
#ifndef NFS_MULTITHREAD
		ImGui::Checkbox("Skip DDay Races", (bool*)SKIPDDAYRACES_ADDR);
#endif
#endif
#ifdef GAME_MW
		if (!*(int*)GRACESTATUS_ADDR && *(unsigned char*)CURRENTBIN_POINTER >= 2)
		{
			sprintf(JumpToBinOptionText, "Jump to bin %d", *(unsigned char*)CURRENTBIN_POINTER - 1);
			if (ImGui::Button(JumpToBinOptionText, ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				JumpToBin(*(unsigned char*)CURRENTBIN_POINTER - 1);
			}
		}
		else
		{
			if (*(int*)GRACESTATUS_ADDR)
			{
				ImGui::Text("Can't jump to bin %d while racing. Go to FE to jump bins.", *(unsigned char*)CURRENTBIN_POINTER - 1);
			}
			if (*(unsigned char*)CURRENTBIN_POINTER < 2)
			{
				ImGui::Text("No more bins left to jump! You're at bin %d!", *(unsigned char*)CURRENTBIN_POINTER);
			}
		}
#endif
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Teleport", ImGuiTreeNodeFlags_None))
	{
		ImGui::InputFloat("X", &TeleportPos.x, 0, 0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		ImGui::InputFloat("Y", &TeleportPos.y, 0, 0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		ImGui::InputFloat("Z", &TeleportPos.z, 0, 0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		if (ImGui::Button("Engage!", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			JumpToNewPos(&TeleportPos);
		}
		ImGui::Separator();

#ifdef OLD_NFS
#ifdef GAME_UG2
		ImGui::TextUnformatted("Hot Position"); // TODO: maybe port over Hot Position from UG2 to UG1?
		if (ImGui::Button("Save", ImVec2(10 * _font_size - _imgui_context->Style.ItemSpacing.x, 0)))
		{
			*(bool*)SAVEHOTPOS_ADDR = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Load", ImVec2(10 * _font_size - _imgui_context->Style.ItemSpacing.x, 0)))
		{
			*(bool*)LOADHOTPOS_ADDR = true;
		}
		ImGui::Separator();
#endif
#else
		ImGui::InputInt("Hot Position", &ActiveHotPos, 1, 1, ImGuiInputTextFlags_CharsDecimal);
		if (ActiveHotPos <= 0)
			ActiveHotPos = 1;
		ActiveHotPos %= 6;
		if (ImGui::Button("Save", ImVec2(10 * _font_size - _imgui_context->Style.ItemSpacing.x, 0)))
		{
			*(int*)SAVEHOTPOS_ADDR = ActiveHotPos;
		}

		ImGui::SameLine();
		if (ImGui::Button("Load", ImVec2(10 * _font_size - _imgui_context->Style.ItemSpacing.x, 0)))
		{
			*(int*)LOADHOTPOS_ADDR = ActiveHotPos;
		}
		ImGui::Checkbox("Floor Snapping", &bTeleFloorSnap);
		ImGui::Separator();
#endif

#ifdef GAME_UG
		if (ImGui::CollapsingHeader("Landmarks (Underground 1 World) (L1RA)", ImGuiTreeNodeFlags_None))
		{
			ImGui::TextUnformatted("Sorry, no landmarks for Underground 1 yet :("); // TODO: make landmarks for UG1 Free Roam (it's such a small map anyway...)
		}
		ImGui::Separator();
#endif

#ifdef GAME_UG2
		if (ImGui::CollapsingHeader("Landmarks (Underground 2 World) (L4RA)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Garage", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bVector3 pos = { 654.59, -102.02,   15.75 };
				JumpToNewPos(&pos);
			}
			if (ImGui::CollapsingHeader("Airport", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Terminal Station", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1871.38, -829.58,   34.08 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Parking Lot Front", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1831.13, -818.90,   24.08 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Parking Lot Behind", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1722.82, -795.87,   23.96 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("City Core", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Stadium Entrance", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1183.45, -646.81, 18.03 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Stadium Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1016.02, -849.46, 17.99 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("South Market - Casino Fountain", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -326.62, -559.63, 19.95 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fort Union Square", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 212.86, -438.44, 18.03 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Best Buy", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 485.37, -617.05, 18.08 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hotel Plaza", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -282.70, 81.08, 8.39 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hotel Plaza Fountain", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -433.37, -170.49, 25.98 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Convention Center", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -632.79, 59.24, 10.59 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Main Street", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -758.48, -176.86, 29.38 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Construction Road 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 148.85, -830.72, 17.52 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Construction Road 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 36.05, -27.53, 16.36 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Parking Garage", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 154.32, 404.37, 4.57 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Basketball Court", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 523.90, -783.58, 8.37 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hotel Plaza Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -225.51, 315.23, 1.08 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Beacon Hill", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -649.46, 585.84, 25.21 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Burger King", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1390.40, 199.50, 11.03 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Park & Boardwalk", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1638.55, 433.51, 4.35 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1170.94, 564.39, 30.19 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Back Alley Shortcut", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1260.51, 778.16, 48.05 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Restaurant Back Alley", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1004.35, 705.92, 35.82 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Zigzag Bottom", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1643.70, 690.86, 2.73 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Zigzag Top", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1463.39, 859.80, 42.42 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Bridge", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -381.70, 709.03, 26.12 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Pigeon Park", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Glass Garden", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -10.06, 827.62, 33.57 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Mansion", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 534.27, 988.52, 33.41 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Pavilion 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 376.73, 1170.13, 35.19 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Pavilion 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 207.75, 1335.30, 49.13 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("BBQ Restaurant", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -185.32, 1599.37, 43.84 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fountain", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -124.09, 1519.45, 39.65 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Brad Lawless Memorial Statue", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -505.99, 1190.16, 47.18 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Jackson Heights", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Entrance Gate", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1494.47, 1047.15, 52.78 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Waterfall Bridge 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2126.07, 1651.40, 130.54 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Waterfall Bridge 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2648.50, 2240.41, 229.08 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Large Mansion", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -3085.82, 2334.12, 262.17 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Mansion 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -720.69, 1853.10, 154.16 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Observatory", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2067.00, 2794.99, 318.93 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Observatory Tunnel", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1499.79, 2219.02, 208.01 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2024.93, 2587.49, 325.54 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Radio Tower", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1205.87, 3119.98, 375.72 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("City Vista", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2155.04, 2016.00, 218.18 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Coal Harbor", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Shipyard", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -687.03, -1433.25, 18.63 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Trainyard", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1031.52, -1782.72, 16.20 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -243.80, -1537.74, 13.95 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Trashyard", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1397.31, -1544.86, 13.85 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Refinery", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 253.64, -1886.44, 14.01 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("East Hwy Entrance", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 926.80, -1231.56, 14.07 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Highway", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Hwy 7 North ", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 305.70, -1681.59, 21.09 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hwy 7 South", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 859.69, -369.76, 18.73 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hwy 27 North", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 208.38, -1013.74, 25.98 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hwy 27 East", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1147.26, -270.31, 25.68 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hwy 27 South", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 69.73, 317.97, 11.17 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hwy 27 West", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1194.52, -212.97, 24.94 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Shops", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::CollapsingHeader("City Center Shops", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Car Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -728.10, -881.46,   18.11 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Performance", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1051.87, -147.84,   17.19 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("El Norte Performance", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 337.01, -756.14,   13.39 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -387.00,   37.17,   14.15 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("El Norte Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 479.72,  122.67,   10.16 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Graphics", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -278.93, -464.33,   20.14 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Specialty", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1267.26, -616.56,   19.93 };
						JumpToNewPos(&pos);
					}

				}
				if (ImGui::CollapsingHeader("Beacon Hill Shops", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Car Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -974.43,  413.24,   11.80 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Performance", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1306.92,  355.72,    9.27 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -994.29,  818.47,   39.12 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Graphics", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -491.38,  714.42,   29.13 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Specialty", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1456.41,  688.46,   24.16 };
						JumpToNewPos(&pos);
					}

				}
				if (ImGui::CollapsingHeader("Jackson Heights Shops", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1928.18, 1130.85,   61.72 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Graphics", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1886.35, 1157.29,   61.63 };
						JumpToNewPos(&pos);
					}

				}
				if (ImGui::CollapsingHeader("Coal Harbor Shops", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Car Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 37.30,-1718.94,   14.15 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("East Performance", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 840.05,-1404.39,    9.50 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("East Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -69.84,-1474.95,   13.92 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Graphics", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 456.23,-1553.53,   13.92 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("East Specialty", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 731.82,-1174.17,   13.57 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("West Performance", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -527.15,-1566.07,   13.93 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("West Body", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -546.54,-1893.46,    4.12 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("West Specialty", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1125.19,-1872.93,   13.92 };
						JumpToNewPos(&pos);
					}
				}
			}
		}
		ImGui::Separator();
#endif
#if defined(GAME_MW) || defined(GAME_CARBON)
		if (ImGui::CollapsingHeader("Landmarks (Most Wanted World)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Jump to Memory High Watermark", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bVector3 pos = { 1113.0, 3221.0, 394.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::CollapsingHeader("City Landmarks", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("East Park", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2085.00,  141.00,   98.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("West Park", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 709.00,  155.00,  115.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Stadium", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 934.00, -649.00,  116.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Time Square", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1452.05,  360.00,  101.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Little Italy", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2113.00,  322.00,  100.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Subway Entrance 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2147.00, 40.00, 94.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Subway Entrance 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 926.00, -556.00, 115.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Subway Entrance 3", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1819.00, 512.00, 94.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway North", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1819.00, 916.00, 124.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway South", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1972.00, -581.00, 105.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway East", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2515.00, 291.00, 93.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway West", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 499.00, 176.00, 108.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Safehouse", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2313.00, -70.00, 94.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Museum", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 885.00, 455.00, 113.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Amphitheatre", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 850.00, 246.00, 114.00 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Coastal Landmarks", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Coastal Safe House", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4254.00, 75.00, 11.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Coney Island", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4094.00, 166.00, 23.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fish Market", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4629.00, 149.00, 7.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Oil Refinery", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3993.00, 2100.00, 28.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fishing Village", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3693.00, 3516.00, 26.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Shipyard", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3107.00, 490.00, 18.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Trainyard", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2920.00, 175.00, 28.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Lighthouse", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4092.00, -173.00, 17.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Drive-in Theatre", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3144.00, 1661.00, 110.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3713.00, 3490.00, 27.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2749.00, 2159.00, 108.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 3", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3071.00, 1030.00, 67.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 4", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3836.00, 605.00, 25.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Trailer Park", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2871.00, 3004.00, 74.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Cannery", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3401.00, 2853.00, 12.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fire Hall", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3878.00, 459.00, 23.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Amusement Park", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3944.00, 273.00, 17.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Boardwalk", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4542.00, 118.00, 6.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Prison", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4093.00, 1302.00, 45.00 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("College Landmarks", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("College Safe House", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1773.00, 2499.00, 150.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Rosewood Park", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 663.00, 4084.00, 210.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Golf Course", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2180.00, 3591.00, 165.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Campus", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1087.00, 3187.00, 202.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Main Street", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1606.00, 2210.00, 145.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Baseball Stadium", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 24.00, 3239.00, 195.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Club House", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2168.00, 3566.00, 162.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway North", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1830.00, 4288.00, 233.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway South", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1798.00, 2003.00, 152.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway East", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2259.00, 2616.00, 141.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Highway West", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -293.00, 3633.00, 207.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Toll Booth 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 378.00, 4502.00, 236.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Toll Booth 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1580.00, 1800.00, 168.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 1", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 822.00, 4500.00, 205.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 2", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1216.00, 3667.00, 201.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 3", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 549.00, 2622.00, 167.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 4", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1641.00, 2486.00, 152.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gas Station 5", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1990.00, 1640.00, 152.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Small Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1057.00, 3826.00, 201.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Large Parking Lot", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1227.00, 3259.00, 204.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Tennis Court", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 729.00, 3540.00, 201.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Stadium", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 43.00, 3154.00, 189.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Cemetary", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 846.00, 2439.00, 151.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Hospital", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1419.00, 2613.00, 164.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Fire Hall", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1510.00, 2144.00, 146.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Donut Shop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1825.00, 1860.00, 151.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Clock Tower", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1683.00, 2114.00, 146.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Strip Mall", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2633.00, 2199.00, 108.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Overpass", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2155.00, 2642.00, 148.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Bus Station", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 2094.00, 1427.00, 152.00 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Shops", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("North College Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 713.00, 4507.00, 214.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("College Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1513.00, 2550.00, 158.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("College Car", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 990.00, 2164.00, 154.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("North City Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1863.00, 1193.00, 146.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("City Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1086.00, 54.00, 101.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("City Car", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1762.00, 529.00, 93.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("South City Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3410.00, -203.00, 14.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("North Coastal Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3611.00, 3636.00, 31.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Refinery Coastal Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3467.00, 2019.00, 77.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Coastal Car", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4200.00, 1276.00, 48.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Coastal Chop", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4234.00, 714.00, 56.00 };
					JumpToNewPos(&pos);
				}
			}
		}
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Landmarks (Carbon World)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::CollapsingHeader("Landmarks", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Tuner", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1912.00, 1363.00, 109.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Exotic", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 1175.00, 472.00, 60.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Casino", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 4794.00, 2040.00, 101.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Muscle", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3776.00, -1341.00, 12.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Santa Fe", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2088.00, 1942.00, -6.00 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Palmont Motor Speedway (drift track)", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -1131.29, 7754.64, 1.14 };
					JumpToNewPos(&pos);
				}
			}
			if (ImGui::CollapsingHeader("Pursuit Breakers", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::CollapsingHeader("Casino", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Casino Archway", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 3993.00, 3459.00, 208.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Gas Homes", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 3909.00, 2844.00, 150.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Donut", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 5271.00, 2119.00, 102.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Motel", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4840.00, 3364.00, 140.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Petro", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 5351.00, 3426.00, 117.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Scaffold", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4041.00, 2419.00, 130.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Casino Gas Commercial", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4281.00, 2152.00, 112.00 };
						JumpToNewPos(&pos);
					}
				}
				if (ImGui::CollapsingHeader("Muscle", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Muscle Facade", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 2763.00, -602.00, 17.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Muscle Ice Cream", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 2889.00, -1361.00, 4.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Muscle Tire", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4403.00, -32.00, 37.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Muscle Dock Crane", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 2770.00, -612.00, 17.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Muscle Gas", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4773.00, -780.00, 27.00 };
						JumpToNewPos(&pos);
					}
				}
				if (ImGui::CollapsingHeader("Santa Fe", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Santa Fe Scaffold", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -2416.00, 1997.00, 11.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Santa Fe Sculpture", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1854.00, 1891.00, 7.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Santa Fe Gas", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -2374.00, 1797.00, -5.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Santa Fe Thunderbird", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { -1967.00, 1695.00, 2.00 };
						JumpToNewPos(&pos);
					}
				}
				if (ImGui::CollapsingHeader("Tuner", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::Button("Tuner Gate A", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 4180.00, 588.00, 48.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Tuner Gate B", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 3883.00, 689.00, 32.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Tuner Gas Park", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 5286.00, 1524.00, 68.00 };
						JumpToNewPos(&pos);
					}
					if (ImGui::Button("Tuner Gas Financial", ImVec2(ImGui::CalcItemWidth(), 0)))
					{
						bVector3 pos = { 5563.00, 683.00, 57.00 };
						JumpToNewPos(&pos);
					}
				}
			}
			if (ImGui::CollapsingHeader("Canyons", ImGuiTreeNodeFlags_None))
			{
				if (ImGui::Button("Eternity Pass", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -6328.50, 12952.43, 942.69 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Journeyman's Bane", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -3256.09, 9614.98, 721.28 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Knife's Edge", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -2498.47, 6215.55, 787.85 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Lookout Point", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 3471.10, 10693.66, 602.65 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Devil's Creek Pass", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 7381.51, 11080.98, 598.47 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Lofty Heights Downhill", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -3253.87, 12843.19, 723.47 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Desparation Ridge", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -703.21, 12999.49, 888.32 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Deadfall Junction", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 8139.83, 9541.43, 889.62 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Copper Ridge", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { -7013.55, 6217.82,  787.86 };
					JumpToNewPos(&pos);
				}
				if (ImGui::Button("Gold Valley Run", ImVec2(ImGui::CalcItemWidth(), 0)))
				{
					bVector3 pos = { 5224.52, 8066.35,  496.12 };
					JumpToNewPos(&pos);
				}
			}
		}
#endif
#ifdef GAME_PS
		if (ImGui::Button("Speed Challenge", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			bCalledProStreetTele = true;
			bVector3 pos = { 0.0,   5000.0,  5.0 };
			JumpToNewPos(&pos);
		}
		if (ImGui::CollapsingHeader("Landmarks (Ebisu)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Ebisu West", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { -54.0,   -5.0,  5.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::Button("Ebisu South", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { -34.0,   240.0,  5.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::Button("Ebisu Touge", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { -255.0,   728.0,  5.0 };
				JumpToNewPos(&pos);
			}
		}
		if (ImGui::CollapsingHeader("Landmarks (Autopolis)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Autopolis Main", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { 40.0, 0,  2.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::Button("Autopolis Lakeside", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { 300.0, -150.0,  5.0 };
				JumpToNewPos(&pos);
			}
		}
		if (ImGui::CollapsingHeader("Landmarks (Willow Springs)", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Willow Springs HorseThief", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { -332.0, 731.0,  5.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::Button("Willow Springs GP", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { -73.0, 6.0,  5.0 };
				JumpToNewPos(&pos);
			}
			if (ImGui::Button("Willow Springs Street", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				bCalledProStreetTele = true;
				bVector3 pos = { 236.0, 569.0,  5.0 };
				JumpToNewPos(&pos);
			}
		}
#endif
#ifdef GAME_UC
		if (ImGui::CollapsingHeader("Landmarks (Undercover / MW2 World)", ImGuiTreeNodeFlags_None))
		{
			ImGui::TextUnformatted("Sorry, no landmarks for Undercover / MW2 yet :("); // TODO: make / rip landmarks for MW2 Free Roam
		}
#endif
	}
	ImGui::Separator();
#ifndef OLD_NFS
	if (ImGui::CollapsingHeader("Race", ImGuiTreeNodeFlags_None))
	{
#ifdef GAME_CARBON
		if (*(int*)FORCEFAKEBOSS_ADDR >= FAKEBOSS_COUNT || *(int*)FORCEFAKEBOSS_ADDR < 0)
			sprintf(BossNames_DisplayStr, "Force Fake Boss: %s", "Unknown");
		else
			sprintf(BossNames_DisplayStr, "Force Fake Boss: %s", BossNames[*(int*)FORCEFAKEBOSS_ADDR]);
		ImGui::InputInt(BossNames_DisplayStr, (int*)FORCEFAKEBOSS_ADDR, 1, 100, ImGuiInputTextFlags_None);
#endif
		if (ImGui::Button("Force Finish", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			ForceFinishRace();
		}
	}
	ImGui::Separator();
#endif
	if (ImGui::CollapsingHeader("AI", ImGuiTreeNodeFlags_None))
	{
#ifndef OLD_NFS
		if (ImGui::CollapsingHeader("Car Watches", ImGuiTreeNodeFlags_None))
		{
			ImGui::InputInt("Current car", (int*)MTOGGLECAR_ADDR, 1, 1, ImGuiInputTextFlags_None);
#ifdef NFS_MULTITHREAD
#ifdef HAS_COPS
			if (ImGui::Button("Watch Cop Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				CarTypeToWatch = CARLIST_TYPE_COP;
				bDoTriggerWatchCar = true;
			}
			if (ImGui::Button("Watch Traffic Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				CarTypeToWatch = CARLIST_TYPE_TRAFFIC;
				bDoTriggerWatchCar = true;
			}
#endif
			if (ImGui::Button("Watch Racer Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				CarTypeToWatch = CARLIST_TYPE_AIRACER;
				bDoTriggerWatchCar = true;
			}
#else
#ifdef HAS_COPS
			if (ImGui::Button("Watch Cop Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				TriggerWatchCar(CARLIST_TYPE_COP);
			}
			if (ImGui::Button("Watch Traffic Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				TriggerWatchCar(CARLIST_TYPE_TRAFFIC);
			}
#endif
			if (ImGui::Button("Watch Racer Car", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				TriggerWatchCar(CARLIST_TYPE_AIRACER);
			}
#endif
			ImGui::Separator();
		}
		if (ImGui::Button("Toggle AI Control", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
#ifdef NFS_MULTITHREAD
			bToggleAiControl = !bToggleAiControl;
#else
			* (bool*)TOGGLEAICONTROL_ADDR = !*(bool*)TOGGLEAICONTROL_ADDR;
#endif
		}
#ifdef GAME_UC
		ImGui::Checkbox("Be a traffic car (after toggle)", &bBeTrafficCar);
#endif
#endif
#ifdef HAS_COPS
#ifdef GAME_MW
		ImGui::Checkbox("Show Non-Pursuit Cops (Minimap)", (bool*)MINIMAP_SHOWNONPURSUITCOPS_ADDR);
		ImGui::Checkbox("Show Pursuit Cops (Minimap)", (bool*)MINIMAP_SHOWPURSUITCOPS_ADDR);
#endif
		if (ImGui::InputFloat("Set Heat", &DebugHeat, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific))
		{
			TriggerSetHeat();
		}
		ImGui::Checkbox("Also set heat to save file", &bSetFEDBHeat);
#ifdef GAME_UC
		bDisableCops = !*(bool*)ENABLECOPS_ADDR;
		if (ImGui::Checkbox("No Cops", &bDisableCops))
			*(bool*)ENABLECOPS_ADDR = !bDisableCops;
#else
		ImGui::Checkbox("No Cops", (bool*)DISABLECOPS_ADDR);
#endif
		ImGui::Checkbox("AI Random Turns", (bool*)AI_RANDOMTURNS_ADDR);
#endif
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_None))
	{
#ifndef OLD_NFS
#ifndef GAME_MW
		ImGui::Checkbox("SmartLookAheadCamera", (bool*)SMARTLOOKAHEADCAMERA_ADDR);
#endif
#endif
#ifdef OLD_NFS
		ImGui::Checkbox("Debug Cameras Enabled", (bool*)DEBUGCAMERASENABLED_ADDR);
#endif
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Car", ImGuiTreeNodeFlags_None))
	{
#ifndef OLD_NFS
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("WARNING: Car changing is unstable and may cause the game to crash!");
		if (*(int*)GAMEFLOWMGR_STATUS_ADDR != GAMEFLOW_STATE_RACING)
			ImGui::TextUnformatted("WARNING: You're not in race mode. The game might crash if you use these.");
		ImGui::PopTextWrapPos();
		ImGui::Separator();
		if (ImGui::Button("Change Car", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			*(bool*)CHANGEPLAYERVEHICLE_ADDR = true;
		}
		if (ImGui::Button("Flip Car", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
#ifdef NFS_MULTITHREAD
			bDoFlipCar = true;
#else
			FlipCar();
#endif
		}
#ifdef GAME_CARBON
		ImGui::Checkbox("Augmented Drift With EBrake", (bool*)AUGMENTEDDRIFT_ADDR);
#endif
#endif
#ifndef OLD_NFS
#ifdef GAME_MW
		ImGui::Checkbox("Infinite NOS", (bool*)INFINITENOS_ADDR);
#else
		ImGui::Checkbox("Infinite NOS", &bInfiniteNOS);
#endif
#ifndef GAME_PS
		ImGui::Checkbox("Infinite RaceBreaker", (bool*)INFINITERACEBREAKER_ADDR);
#else
		if (*(int*)0x0040BE15 == 0)
			bAppliedSpeedLimiterPatches = true;
		if (bAppliedSpeedLimiterPatches)
		{
			if (ImGui::Button("Revive Top Speed Limiter", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				UndoSpeedLimiterPatches();
				bAppliedSpeedLimiterPatches = false;
			}
		}
		else
		{
			if (ImGui::Button("Kill Top Speed Limiter", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				ApplySpeedLimiterPatches();
				bAppliedSpeedLimiterPatches = true;
			}
		}
		if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
		{
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted("NOTE: Top speed patches will take effect after reloading the track! (Go to FrontEnd and back)");
			ImGui::PopTextWrapPos();
		}
#endif
#endif

	}

	ImGui::Separator();
	if (ImGui::CollapsingHeader("GameFlow", ImGuiTreeNodeFlags_None))
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("WARNING: This feature is still experimental! The game may crash unexpectedly!\nLoad a save profile to avoid any bugs.");
#ifdef OLD_NFS
		ImGui::TextUnformatted("WARNING: You are running NFSU or NFSU2. Please use SkipFE features instead.");
#endif
		ImGui::PopTextWrapPos();
		ImGui::Separator();
		ImGui::Text("Current Track: %d", *(int*)SKIPFETRACKNUM_ADDR);
		ImGui::InputInt("Track Number", &SkipFETrackNum, 1, 100, ImGuiInputTextFlags_None);
		ImGui::Separator();

		if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_IN_FRONTEND)
		{
			if (ImGui::Button("Start Track", ImVec2(ImGui::CalcItemWidth(), 0)))
			{

#if defined GAME_PS || defined GAME_UC
				bDoFEUnloading = true;
#else
				*(int*)SKIPFETRACKNUM_ADDR = SkipFETrackNum;
				GameFlowManager_UnloadFrontend((void*)GAMEFLOWMGR_ADDR);
#endif
			}
		}
		if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
		{
			if (ImGui::Button("Start Track (in game - it may not work, goto FE first)", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
#ifdef NFS_MULTITHREAD
				bDoLoadRegion = true;
#else
				* (int*)SKIPFETRACKNUM_ADDR = SkipFETrackNum;
				GameFlowManager_LoadRegion((void*)GAMEFLOWMGR_ADDR);
#endif
			}
#ifndef GAME_UC
			if (ImGui::Button("Unload Track (Go to FrontEnd)", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
#ifdef NFS_MULTITHREAD
				bDoTrackUnloading = true;
#else
#ifdef GAME_MW
				BootFlowManager_Init(); // otherwise crashes without it in MW...
#endif
				GameFlowManager_UnloadTrack((void*)GAMEFLOWMGR_ADDR);
#endif
			}
#endif
		}
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("SkipFE", ImGuiTreeNodeFlags_None))
	{
		ImGui::Checkbox("SkipFE Status", (bool*)SKIPFE_ADDR);
		ImGui::Text("Current Track: %d", *(int*)SKIPFETRACKNUM_ADDR);
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Track Settings", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::InputInt("Track Number", &SkipFETrackNum, 1, 100, ImGuiInputTextFlags_None);
			ImGui::Checkbox("Track Reverse Direction", (bool*)SKIPFE_TRACKDIRECTION_ADDR);
			ImGui::InputInt("Num Laps", (int*)SKIPFE_NUMLAPS_ADDR, 1, 100, ImGuiInputTextFlags_None);
#ifndef OLD_NFS
#ifdef GAME_MW
			ImGui::InputText("Race ID", (char*)SKIPFE_RACEID_ADDR, 15);
#else
			if (ImGui::InputText("Race ID", SkipFERaceID, 64))
				*(char**)SKIPFE_RACEID_ADDR = SkipFERaceID;
#endif
#ifdef GAME_PS
			if (ImGui::InputText("Force Hub Selection Set", SkipFEForceHubSelectionSet, 64))
				*(char**)SKIPFE_FORCEHUBSELECTIONSET_ADDR = SkipFEForceHubSelectionSet;
			if (ImGui::InputText("Force Race Selection Set", SkipFEForceRaceSelectionSet, 64))
				*(char**)SKIPFE_FORCERACESELECTIONSET_ADDR = SkipFEForceRaceSelectionSet;
#endif
#endif
		}
		if (ImGui::CollapsingHeader("AI Settings", ImGuiTreeNodeFlags_None))
		{
			ImGui::InputInt("Num AI Cars", (int*)SKIPFE_NUMAICARS_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::SliderInt("Difficulty", (int*)SKIPFE_DIFFICULTY_ADDR, 0, 2);
#ifdef OLD_NFS
			ImGui::InputInt("Force All AI Cars To Type", (int*)SKIPFE_FORCEALLAICARSTOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Force AI Car 1 To Type", (int*)SKIPFE_FORCEAICAR1TOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Force AI Car 2 To Type", (int*)SKIPFE_FORCEAICAR2TOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Force AI Car 3 To Type", (int*)SKIPFE_FORCEAICAR3TOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
#ifdef GAME_UG2
			ImGui::InputInt("Force AI Car 4 To Type", (int*)SKIPFE_FORCEAICAR4TOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Force AI Car 5 To Type", (int*)SKIPFE_FORCEAICAR5TOBETHISTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
#endif
			ImGui::SliderFloat("Force All AI Cars to Perf Rating", (float*)SKIPFE_FORCEALLAICARSTOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
			ImGui::SliderFloat("Force AI Car 1 to Perf Rating", (float*)SKIPFE_FORCEAICAR1TOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
			ImGui::SliderFloat("Force AI Car 2 to Perf Rating", (float*)SKIPFE_FORCEAICAR2TOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
			ImGui::SliderFloat("Force AI Car 3 to Perf Rating", (float*)SKIPFE_FORCEAICAR3TOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
#ifdef GAME_UG2
			ImGui::SliderFloat("Force AI Car 4 to Perf Rating", (float*)SKIPFE_FORCEAICAR4TOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
			ImGui::SliderFloat("Force AI Car 5 to Perf Rating", (float*)SKIPFE_FORCEAICAR5TOPERFRATING_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
#endif
#else
#ifdef GAME_MW
			if (ImGui::InputText("Opponent Preset Ride", SkipFEOpponentPresetRide, 64))
			{
				*(char**)SKIPFE_OPPONENTPRESETRIDE_ADDR = SkipFEOpponentPresetRide;
			}
#elif GAME_CARBON
			ImGui::Checkbox("No Wingman", (bool*)SKIPFE_NOWINGMAN_ADDR);
			if (ImGui::InputText("Wingman Preset Ride", SkipFEWingmanPresetRide, 64))
				*(char**)SKIPFE_WINGMANPRESETRIDE_ADDR = SkipFEWingmanPresetRide;
			if (ImGui::InputText("Opponent 1 Preset Ride", SkipFEOpponentPresetRide0, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE0_ADDR = SkipFEOpponentPresetRide0;
			if (ImGui::InputText("Opponent 2 Preset Ride", SkipFEOpponentPresetRide1, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE1_ADDR = SkipFEOpponentPresetRide1;
			if (ImGui::InputText("Opponent 3 Preset Ride", SkipFEOpponentPresetRide2, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE2_ADDR = SkipFEOpponentPresetRide2;
			if (ImGui::InputText("Opponent 4 Preset Ride", SkipFEOpponentPresetRide3, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE3_ADDR = SkipFEOpponentPresetRide3;
			if (ImGui::InputText("Opponent 5 Preset Ride", SkipFEOpponentPresetRide4, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE4_ADDR = SkipFEOpponentPresetRide4;
			if (ImGui::InputText("Opponent 6 Preset Ride", SkipFEOpponentPresetRide5, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE5_ADDR = SkipFEOpponentPresetRide5;
			if (ImGui::InputText("Opponent 7 Preset Ride", SkipFEOpponentPresetRide6, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE6_ADDR = SkipFEOpponentPresetRide6;
			if (ImGui::InputText("Opponent 8 Preset Ride", SkipFEOpponentPresetRide7, 64))
				*(char**)SKIPFE_OPPONENTPRESETRIDE7_ADDR = SkipFEOpponentPresetRide7;
#endif
#endif
		}
#ifdef HAS_COPS
		if (ImGui::CollapsingHeader("Cop Settings", ImGuiTreeNodeFlags_None))
		{
			ImGui::InputInt("Max Cops", (int*)SKIPFE_MAXCOPS_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::Checkbox("Helicopter", (bool*)SKIPFE_HELICOPTER_ADDR);
			ImGui::Checkbox("Disable Cops", (bool*)SKIPFE_DISABLECOPS_ADDR);
		}
#endif
		if (ImGui::CollapsingHeader("Traffic Settings", ImGuiTreeNodeFlags_None))
		{
#ifndef GAME_UC
#ifdef OLD_NFS
			ImGui::SliderInt("Traffic Density", (int*)SKIPFE_TRAFFICDENSITY_ADDR, 0, 3);
#else
			ImGui::SliderFloat("Traffic Density", (float*)SKIPFE_TRAFFICDENSITY_ADDR, 0.0, 100.0, "%.3f", ImGuiSliderFlags_None);
#endif
#ifndef GAME_UG2
			ImGui::SliderFloat("Traffic Oncoming", (float*)SKIPFE_TRAFFICONCOMING_ADDR, 0.0, 10.0, "%.3f", ImGuiSliderFlags_None);
#endif
#endif
#ifndef OLD_NFS
			ImGui::Checkbox("Disable Traffic", (bool*)SKIPFE_DISABLETRAFFIC_ADDR);
#endif
		}
		if (ImGui::CollapsingHeader("Game Settings", ImGuiTreeNodeFlags_None))
		{
			ImGui::Checkbox("Force Point2Point Mode", (bool*)SKIPFE_P2P_ADDR);
#ifdef GAME_PS
			ImGui::Checkbox("Practice Mode", (bool*)SKIPFE_PRACTICEMODE_ADDR);
#endif
#ifdef OLD_NFS
			ImGui::Checkbox("Force Drag Mode", (bool*)SKIPFE_DRAGRACE_ADDR);
			ImGui::Checkbox("Force Drift Mode", (bool*)SKIPFE_DRIFTRACE_ADDR);
#ifdef GAME_UG2
			ImGui::Checkbox("Force Team Drift Mode", (bool*)SKIPFE_DRIFTRACETEAMED_ADDR);
			ImGui::Checkbox("Force Burnout Mode", (bool*)SKIPFE_BURNOUTRACE_ADDR);
			ImGui::Checkbox("Force Short Track Mode", (bool*)SKIPFE_SHORTTRACK_ADDR);
			ImGui::InputFloat("Rolling Start Speed", (float*)SKIPFE_ROLLINGSTART_SPEED, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#else
			ImGui::Checkbox("Force Be A Cop Mode", (bool*)SKIPFE_BEACOP_ADDR);
#endif
#endif
			if (*(int*)SKIPFE_RACETYPE_ADDR >= kRaceContext_Count || *(int*)SKIPFE_RACETYPE_ADDR < 0)
				sprintf(SkipFERaceTypeDisplay, "Race Type: %s", "Unknown");
			else
				sprintf(SkipFERaceTypeDisplay, "Race Type: %s", GRaceContextNames[*(int*)SKIPFE_RACETYPE_ADDR]);
			ImGui::InputInt(SkipFERaceTypeDisplay, (int*)SKIPFE_RACETYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Num Player Cars", (int*)SKIPFE_NUMPLAYERCARS_ADDR, 1, 100, ImGuiInputTextFlags_None);
#ifndef OLD_NFS
#ifdef GAME_PS
			ImGui::InputInt("Num Player Screens", (int*)SKIPFE_NUMPLAYERSCREENS_ADDR, 1, 100, ImGuiInputTextFlags_None);
			if (ImGui::InputText("Force NIS", SkipFEForceNIS, 64))
				*(char**)SKIPFE_FORCENIS_ADDR = SkipFEForceNIS;
			if (ImGui::InputText("Force NIS Context", SkipFEForceNISContext, 64))
				*(char**)SKIPFE_FORCENISCONTEXT_ADDR = SkipFEForceNISContext;
			ImGui::Checkbox("Enable Debug Activity", (bool*)SKIPFE_ENABLEDEBUGACTIVITY_ADDR);
			ImGui::Checkbox("Disable Smoke", (bool*)SKIPFE_DISABLESMOKE_ADDR);
			ImGui::Checkbox("Slot Car Race", (bool*)SKIPFE_SLOTCARRACE_ADDR);
#else
#ifndef GAME_UC
			ImGui::Checkbox("Split screen mode", (bool*)SKIPFE_SPLITSCREEN_ADDR);
#endif
#endif
#endif
		}
		if (ImGui::CollapsingHeader("Car Settings", ImGuiTreeNodeFlags_None))
		{
#ifdef OLD_NFS
			ImGui::InputInt("Default Player 1 Car Type", (int*)SKIPFE_DEFAULTPLAYER1CARTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Default Player 2 Car Type", (int*)SKIPFE_DEFAULTPLAYER2CARTYPE_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Default Player 1 Skin Index", (int*)SKIPFE_DEFAULTPLAYER1SKININDEX_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Default Player 2 Skin Index", (int*)SKIPFE_DEFAULTPLAYER2SKININDEX_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::SliderInt("Player Car Upgrade Level", (int*)SKIPFE_PLAYERCARUPGRADEALL_ADDR, -1, 3);
#ifdef GAME_UG2
			ImGui::InputInt("Force Player 1 Start Position", (int*)SKIPFE_FORCEPLAYER1STARTPOS_ADDR, 1, 100, ImGuiInputTextFlags_None);
			ImGui::InputInt("Force Player 2 Start Position", (int*)SKIPFE_FORCEPLAYER2STARTPOS_ADDR, 1, 100, ImGuiInputTextFlags_None);
#endif
#else
			if (ImGui::InputText("Player Car", SkipFEPlayerCar, 128))
			{
				*(char**)SKIPFE_PLAYERCAR_ADDR = SkipFEPlayerCar;
			}
			if (ImGui::InputText("Player Car 2", SkipFEPlayerCar2, 128))
			{
				*(char**)SKIPFE_PLAYERCAR2_ADDR = SkipFEPlayerCar2;
			}
#ifdef GAME_PS
			if (ImGui::InputText("Player Car 3", SkipFEPlayerCar3, 128))
			{
				*(char**)SKIPFE_PLAYERCAR3_ADDR = SkipFEPlayerCar3;
			}
			if (ImGui::InputText("Player Car 4", SkipFEPlayerCar4, 128))
			{
				*(char**)SKIPFE_PLAYERCAR4_ADDR = SkipFEPlayerCar4;
			}
			if (ImGui::InputText("Turbo SFX", SkipFETurboSFX, 128))
			{
				*(char**)SKIPFE_TURBOSFX_ADDR = SkipFETurboSFX;
			}
			ImGui::InputInt("Transmission Setup", (int*)SKIPFE_TRANSMISSIONSETUP_ADDR, 1, 100);
			if (ImGui::CollapsingHeader("Driver Aids", ImGuiTreeNodeFlags_None))
			{
				ImGui::SliderInt("Traction Control Level", (int*)SKIPFE_TRACTIONCONTROLLEVEL_ADDR, -1, 4);
				ImGui::SliderInt("Stability Control Level", (int*)SKIPFE_STABILITYCONTROLLEVEL_ADDR, -1, 3);
				ImGui::SliderInt("ABS Level", (int*)SKIPFE_ANTILOCKBRAKESLEVEL_ADDR, -1, 3);
				ImGui::SliderInt("Drift Steering", (int*)SKIPFE_DRIFTASSISTLEVEL_ADDR, -1, 10);
				ImGui::SliderInt("Raceline Assist", (int*)SKIPFE_RACELINEASSISTLEVEL_ADDR, -1, 20);
				ImGui::SliderInt("Braking Assist", (int*)SKIPFE_BRAKINGASSISTLEVEL_ADDR, -1, 20);
			}

#endif
#ifdef GAME_CARBON
			if (ImGui::InputText("Player Preset Ride", SkipFEPlayerPresetRide, 64))
				*(char**)SKIPFE_PLAYERPRESETRIDE_ADDR = SkipFEPlayerPresetRide;
#endif
			ImGui::SliderFloat("Player Car Performance", (float*)SKIPFE_PLAYERPERFORMANCE_ADDR, -1.0, 10.0, "%.3f", ImGuiSliderFlags_None);
#endif
		}
		ImGui::Separator();
		if (ImGui::Button("Start SkipFE Race", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			*(int*)SKIPFETRACKNUM_ADDR2 = SkipFETrackNum;
			*(int*)SKIPFE_ADDR = 1;
#if defined(GAME_MW) || defined(OLD_NFS)
			OnlineEnabled_OldState = *(bool*)ONLINENABLED_ADDR;
			*(int*)ONLINENABLED_ADDR = 0;
#endif
			RaceStarter_StartSkipFERace();
#if defined(GAME_MW) || defined(OLD_NFS)
			*(int*)ONLINENABLED_ADDR = OnlineEnabled_OldState;
#endif
		}
	}
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_None))
	{
#ifdef GAME_UC
		if (ImGui::Checkbox("Motion Blur", &bMotionBlur))
		{
			modified = true;
		}
		ImGui::SliderFloat("Bloom Scale", (float*)0x00D5E154, -10.0, 10.0, "%.3f", ImGuiSliderFlags_None);
#endif
#ifndef OLD_NFS
#ifdef GAME_PS
		ImGui::InputFloat("Game Speed", &GameSpeed, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#else
		ImGui::InputFloat("Game Speed", (float*)GAMESPEED_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		ImGui::Checkbox("Visual Look Filter", (bool*)APPLYVISUALLOOK_ADDR);
#endif
		ImGui::InputInt(PrecullerModeNames[*(int*)PRECULLERMODE_ADDR], (int*)PRECULLERMODE_ADDR, 1, 100, ImGuiInputTextFlags_None);
		*(int*)PRECULLERMODE_ADDR %= 4;
		if (*(int*)PRECULLERMODE_ADDR < 0)
			*(int*)PRECULLERMODE_ADDR = 3;
#endif
#ifdef GAME_UG2
		ImGui::Checkbox("Draw Cars", (bool*)DRAWCARS_ADDR);
		ImGui::Checkbox("Draw Car Reflections", (bool*)DRAWCARSREFLECTIONS_ADDR);
		ImGui::Checkbox("Draw Light Flares", (bool*)DRAWLIGHTFLARES_ADDR);
		ImGui::Checkbox("Draw Fancy Car Shadows", (bool*)DRAWFANCYCARSHADOW_ADDR);
		ImGui::InputFloat("Fancy Car Shadow Edge Mult.", (float*)FANCYCARSHADOWEDGEMULT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		ImGui::InputFloat("Wheel Pivot Translation Amount", (float*)WHEELPIVOTTRANSLATIONAMOUNT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
		ImGui::InputFloat("Wheel Standard Width", (float*)WHEELSTANDARDWIDTH_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);

		if (ImGui::CollapsingHeader("Precipitation & Weather", ImGuiTreeNodeFlags_None))
		{
			ImGui::Checkbox("Precipitation Enable", (bool*)PRECIPITATION_ENABLE_ADDR);
			ImGui::Checkbox("Precipitation Render", (bool*)PRECIPITATION_RENDER_ADDR);
			ImGui::Checkbox("Precipitation Debug Enable", (bool*)PRECIPITATION_DEBUG_ADDR);
			ImGui::Separator();
			ImGui::TextUnformatted("Values");
			if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Precipitation Percentage", (float*)PRECIPITATION_PERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound X", (float*)PRECIP_BOUNDX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound Y", (float*)PRECIP_BOUNDY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound Z", (float*)PRECIP_BOUNDZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead X", (float*)PRECIP_AHEADX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead Y", (float*)PRECIP_AHEADY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead Z", (float*)PRECIP_AHEADZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Weather Change", (float*)PRECIP_WEATHERCHANGE_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Drive Factor", (float*)PRECIP_DRIVEFACTOR_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Prevailing Multiplier", (float*)PRECIP_PREVAILINGMULT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::Checkbox("Always Raining", (bool*)PRECIP_CHANCE100_ADDR);
				ImGui::InputInt("Rain Type (restart world to see diff.)", &GlobalRainType, 1, 100, ImGuiInputTextFlags_None);
			}
			if (ImGui::CollapsingHeader("Wind", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Wind Angle", (float*)PRECIP_WINDANG_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Max Sway", (float*)PRECIP_SWAYMAX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Max Wind Effect", (float*)PRECIP_MAXWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Road Dampness", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Base Dampness", (float*)PRECIP_BASEDAMPNESS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Uber Dampness", (float*)PRECIP_UBERDAMPNESS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("On-screen FX", ImGuiTreeNodeFlags_None))
			{
				ImGui::Checkbox("OverRide Enable", (bool*)PRECIP_ONSCREEN_OVERRIDE_ADDR);
				ImGui::InputFloat("Drip Speed", (float*)PRECIP_ONSCREEN_DRIPSPEED_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Speed Mod", (float*)PRECIP_ONSCREEN_SPEEDMOD_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_None))
			{
				ImGui::Checkbox("Fog Control OverRide", (bool*)FOG_CTRLOVERRIDE_ADDR);
				ImGui::InputFloat("Precip. Fog Percentage", (float*)PRECIP_FOGPERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff", (float*)BASEFOG_FALLOFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff X", (float*)BASEFOG_FALLOFFX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff Y", (float*)BASEFOG_FALLOFFY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Weather Fog", (float*)BASEWEATHER_FOG_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Weather Fog Start", (float*)BASEWEATHER_FOG_START_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				if (ImGui::CollapsingHeader("Base Weather Fog Colour", ImGuiTreeNodeFlags_None))
				{
					if (ImGui::ColorPicker3("", (float*)&(FogColourPicker.x), ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueWheel))
					{
						*(int*)BASEWEATHER_FOG_COLOUR_R_ADDR = (int)(FogColourPicker.x * 255);
						*(int*)BASEWEATHER_FOG_COLOUR_G_ADDR = (int)(FogColourPicker.y * 255);
						*(int*)BASEWEATHER_FOG_COLOUR_B_ADDR = (int)(FogColourPicker.z * 255);
					}
				}
			}
			if (ImGui::CollapsingHeader("Rain", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Rain X", (float*)PRECIP_RAINX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Y", (float*)PRECIP_RAINY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Z", (float*)PRECIP_RAINZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Z Constant", (float*)PRECIP_RAINZCONSTANT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius X", (float*)PRECIP_RAINRADIUSX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius Y", (float*)PRECIP_RAINRADIUSY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius Z", (float*)PRECIP_RAINRADIUSZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Wind Effect", (float*)PRECIP_RAINWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Percentage", (float*)PRECIP_RAINPERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain in the headlights", (float*)PRECIP_RAININTHEHEADLIGHTS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Snow", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Snow X", (float*)PRECIP_SNOWX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Y", (float*)PRECIP_SNOWY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Z", (float*)PRECIP_SNOWZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Z Constant", (float*)PRECIP_SNOWZCONSTANT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Radius X", (float*)PRECIP_SNOWRADIUSX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Radius Y", (float*)PRECIP_SNOWRADIUSY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Radius Z", (float*)PRECIP_SNOWRADIUSZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Wind Effect", (float*)PRECIP_SNOWWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Snow Percentage", (float*)PRECIP_SNOWPERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Sleet", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Sleet X", (float*)PRECIP_SLEETX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Y", (float*)PRECIP_SLEETY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Z", (float*)PRECIP_SLEETZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Z Constant", (float*)PRECIP_SLEETZCONSTANT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Radius X", (float*)PRECIP_SLEETRADIUSX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Radius Y", (float*)PRECIP_SLEETRADIUSY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Radius Z", (float*)PRECIP_SLEETRADIUSZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Sleet Wind Effect", (float*)PRECIP_SLEETWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Hail", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Hail X", (float*)PRECIP_HAILX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Y", (float*)PRECIP_HAILY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Z", (float*)PRECIP_HAILZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Z Constant", (float*)PRECIP_HAILZCONSTANT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Radius X", (float*)PRECIP_HAILRADIUSX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Radius Y", (float*)PRECIP_HAILRADIUSY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Radius Z", (float*)PRECIP_HAILRADIUSZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Hail Wind Effect", (float*)PRECIP_HAILWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}

		}
#endif
#ifdef HAS_FOG_CTRL
#ifndef OLD_NFS
		if (ImGui::CollapsingHeader("Precipitation & Weather", ImGuiTreeNodeFlags_None))
		{
			ImGui::Checkbox("Precipitation Enable", (bool*)PRECIPITATION_ENABLE_ADDR);
			ImGui::Checkbox("Precipitation Render", (bool*)PRECIPITATION_RENDER_ADDR);
			ImGui::Checkbox("Precipitation Debug Enable", (bool*)PRECIPITATION_DEBUG_ADDR);
			ImGui::Separator();
			ImGui::TextUnformatted("Values");
			if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Precipitation Percentage", (float*)PRECIPITATION_PERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound X", (float*)PRECIP_BOUNDX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound Y", (float*)PRECIP_BOUNDY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Bound Z", (float*)PRECIP_BOUNDZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead X", (float*)PRECIP_AHEADX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead Y", (float*)PRECIP_AHEADY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Ahead Z", (float*)PRECIP_AHEADZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Drive Factor", (float*)PRECIP_DRIVEFACTOR_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Prevailing Multiplier", (float*)PRECIP_PREVAILINGMULT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Camera Mod", (float*)PRECIP_CAMERAMOD_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Wind", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Wind Angle", (float*)PRECIP_WINDANG_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Max Sway", (float*)PRECIP_SWAYMAX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Max Wind Effect", (float*)PRECIP_MAXWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Road Dampness", ImGuiTreeNodeFlags_None))
			{
#ifdef GAME_MW
				ImGui::InputFloat("Base Dampness", (float*)PRECIP_BASEDAMPNESS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#endif
#ifdef GAME_CARBON
				ImGui::InputFloat("Wet Dampness", (float*)PRECIP_WETDAMPNESS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Dry Dampness", (float*)PRECIP_DRYDAMPNESS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);

#endif
			}
			if (ImGui::CollapsingHeader("On-screen FX", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Drip Speed", (float*)PRECIP_ONSCREEN_DRIPSPEED_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Speed Mod", (float*)PRECIP_ONSCREEN_SPEEDMOD_ADDR, 0.0001, 0.001, "%.6f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Drop Shape Speed Change", (float*)PRECIP_ONSCREEN_DROPSHAPESPEEDCHANGE_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_None))
			{
				ImGui::Checkbox("Fog Control OverRide", (bool*)FOG_CTRLOVERRIDE_ADDR);
				ImGui::InputFloat("Precip. Fog Percentage", (float*)PRECIP_FOGPERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff", (float*)BASEFOG_FALLOFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff X", (float*)BASEFOG_FALLOFFX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Falloff Y", (float*)BASEFOG_FALLOFFY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#ifdef GAME_CARBON
				ImGui::InputFloat("Base Fog End", (float*)BASEFOGEND_NONPS2_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Fog Exponent", (float*)BASEFOGEXPONENT_NONPS2_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#endif
#ifdef GAME_CARBON
				ImGui::InputFloat("Base Weather Fog", (float*)BASEWEATHERFOG_NONPS2_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Base Weather Fog (PS2 value)", (float*)BASEWEATHER_FOG_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#else
				ImGui::InputFloat("Base Weather Fog)", (float*)BASEWEATHER_FOG_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#endif
				ImGui::InputFloat("Base Weather Fog Start", (float*)BASEWEATHER_FOG_START_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				if (ImGui::CollapsingHeader("Base Weather Fog Colour", ImGuiTreeNodeFlags_None))
				{
					//ImGui::InputInt("R", (int*)BASEWEATHER_FOG_COLOUR_R_ADDR, 1, 100, ImGuiInputTextFlags_None);
					//ImGui::InputInt("G", (int*)BASEWEATHER_FOG_COLOUR_G_ADDR, 1, 100, ImGuiInputTextFlags_None);
					//ImGui::InputInt("B", (int*)BASEWEATHER_FOG_COLOUR_B_ADDR, 1, 100, ImGuiInputTextFlags_None);
					if (ImGui::ColorPicker3("", (float*)&(FogColourPicker.x), ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueWheel))
					{
						*(int*)BASEWEATHER_FOG_COLOUR_R_ADDR = (int)(FogColourPicker.x * 255);
						*(int*)BASEWEATHER_FOG_COLOUR_G_ADDR = (int)(FogColourPicker.y * 255);
						*(int*)BASEWEATHER_FOG_COLOUR_B_ADDR = (int)(FogColourPicker.z * 255);
					}
#ifdef GAME_CARBON
					ImGui::Separator();
#endif
				}
#ifdef GAME_CARBON
				//ImGui::InputFloat("Base Sky Fog Falloff", (float*)BASESKYFOGFALLOFF_ADDR, 0.001, 0.01, "%.6f", ImGuiInputTextFlags_CharsScientific);
				ImGui::SliderFloat("Base Sky Fog Falloff", (float*)BASESKYFOGFALLOFF_ADDR, -0.005, 0.005, "%.6f");
				ImGui::InputFloat("Base Sky Fog Offset", (float*)BASESKYFOGOFFSET_ADDR, 0.1, 1.00, "%.3f", ImGuiInputTextFlags_CharsScientific);
#endif
			}
			if (ImGui::CollapsingHeader("Clouds", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Rate of change", (float*)PRECIP_CLOUDSRATEOFCHANGE_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
			if (ImGui::CollapsingHeader("Rain", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Rain X", (float*)PRECIP_RAINX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Y", (float*)PRECIP_RAINY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Z", (float*)PRECIP_RAINZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Z Constant", (float*)PRECIP_RAINZCONSTANT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius X", (float*)PRECIP_RAINRADIUSX_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius Y", (float*)PRECIP_RAINRADIUSY_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Radius Z", (float*)PRECIP_RAINRADIUSZ_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Wind Effect", (float*)PRECIP_RAINWINDEFF_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Percentage", (float*)PRECIP_RAINPERCENT_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain in the headlights", (float*)PRECIP_RAININTHEHEADLIGHTS_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Rain Rate of change", (float*)PRECIP_RAINRATEOFCHANGE_ADDR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
			}
		}
#endif
#endif
#ifdef GAME_MW
		if (ImGui::CollapsingHeader("Visual Filter Control", ImGuiTreeNodeFlags_None))
		{

			if (*(int*)VISUALTREATMENT_INSTANCE_ADDR)
			{
				int visualFilterVars = 0;

				if (**(int**)VISUALTREATMENT_INSTANCE_ADDR == COPCAM_LOOK)
					visualFilterVars = (*(int*)VISUALTREATMENT_INSTANCE_ADDR) + 0x1AC;
				else
				{
					**(int**)VISUALTREATMENT_INSTANCE_ADDR = 0;
					visualFilterVars = (*(int*)VISUALTREATMENT_INSTANCE_ADDR) + 0x184;
				}
				if (visualFilterVars)
				{
					visualFilterVars = *(int*)(visualFilterVars + 8);
					if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_None))
					{
						ImGui::Checkbox("Cop Cam Look", *(bool**)VISUALTREATMENT_INSTANCE_ADDR);

						(VisualFilterColourPicker.x) = *(float*)(visualFilterVars + 0xC0);
						(VisualFilterColourPicker.y) = *(float*)(visualFilterVars + 0xC4);
						(VisualFilterColourPicker.z) = *(float*)(visualFilterVars + 0xC8);

						if (ImGui::ColorPicker3("", (float*)&(VisualFilterColourPicker.x), ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueWheel))
						{
							*(float*)(visualFilterVars + 0xC0) = (VisualFilterColourPicker.x) * VisualFilterColourMultiR;
							*(float*)(visualFilterVars + 0xC4) = (VisualFilterColourPicker.y) * VisualFilterColourMultiG;
							*(float*)(visualFilterVars + 0xC8) = (VisualFilterColourPicker.z) * VisualFilterColourMultiB;
						}
						ImGui::InputFloat("Multiplier R", &VisualFilterColourMultiR, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
						ImGui::InputFloat("Multiplier G", &VisualFilterColourMultiG, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
						ImGui::InputFloat("Multiplier B", &VisualFilterColourMultiB, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
						ImGui::Separator();
					}
					ImGui::InputFloat("Filter Colour Power", (float*)(visualFilterVars + 0xD0), 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
					ImGui::InputFloat("Saturation", (float*)(visualFilterVars + 0xD4), 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
					ImGui::InputFloat("Black Bloom", (float*)(visualFilterVars + 0xDC), 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
					if (ImGui::CollapsingHeader("Unknown values", ImGuiTreeNodeFlags_None))
					{
						ImGui::InputFloat("Unknown 1", (float*)(visualFilterVars + 0xCC), 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
						ImGui::InputFloat("Unknown 2", (float*)(visualFilterVars + 0xD8), 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
					}
				}
				else
					ImGui::TextUnformatted("ERROR: Cannot fetch visual filter values!");

				ImGui::Text("Base Address: 0x%X", visualFilterVars);
			}
		}
#endif
	}
	ImGui::Separator();
#ifndef OLD_NFS
#ifndef GAME_UC
	if (ImGui::CollapsingHeader("FMV", ImGuiTreeNodeFlags_None))
	{
#ifdef GAME_MW
		char* moviefilename_pointer = (char*)MOVIEFILENAME_ADDR;
		if (*(int*)GAMEFLOWMGR_STATUS_ADDR == GAMEFLOW_STATE_RACING)
			moviefilename_pointer = (char*)INGAMEMOVIEFILENAME_ADDR;
		ImGui::InputText("Movie Filename", moviefilename_pointer, 0x40);
#else
		ImGui::InputText("Movie Filename", MovieFilename, 0x40);
#endif
		if (ImGui::Button("Play Movie", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
#ifdef NFS_MULTITHREAD
			bDoPlayMovie = true;
#else
			PlayMovie();
#endif
		}
		if (ImGui::CollapsingHeader("Movie filename info (how to)", ImGuiTreeNodeFlags_None))
		{
			ImGui::PushTextWrapPos();
			ImGui::TextUnformatted("In the \"Movie Filename\" box only type in the name of the movie, not the full filename\nExample: If you want to play blacklist_01_english_ntsc.vp6, only type in the \"blacklist_01\", not the \"_english_ntsc.vp6\" part of it.\nWhen you go in-game, the input box will change its pointer to the in-game buffer.");
			ImGui::PopTextWrapPos();
		}
#ifdef GAME_MW
		ImGui::Separator();
		if (ImGui::CollapsingHeader("BlackList FMV", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Blacklist 01", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_01");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 02", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_02");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 03", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_03");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 04", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_04");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 05", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_05");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 06", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_06");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 07", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_07");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 08", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_08");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 09", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_09");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 10", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_10");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 11", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_11");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 12", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_12");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 13", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_13");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 14", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_14");
				PlayMovie();
			}
			if (ImGui::Button("Blacklist 15", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "blacklist_15");
				PlayMovie();
			}
		}
		if (ImGui::CollapsingHeader("Story FMV", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("storyfmv_bla134", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_bla134");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_bus12", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_bus12");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_cro06_coh06a", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_cro06_coh06a");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_dda01", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_dda01");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_epi138", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_epi138");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_her136", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_her136");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_pin11", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_pin11");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_pol17_mot21", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_pol17_mot21");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_rap30", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_rap30");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_raz08", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_raz08");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_roc02", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_roc02");
				PlayMovie();
			}
			if (ImGui::Button("storyfmv_saf25", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "storyfmv_saf25");
				PlayMovie();
			}
		}
		if (ImGui::CollapsingHeader("Demo FMV", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("SSX OnTour Trailer", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "SX-OT-n-480");
				PlayMovie();
			}
			if (ImGui::Button("NBA Live 2006 Trailer", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "NBALive-2006-n");
				PlayMovie();
			}
			if (ImGui::Button("MarvNM_480", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "MarvNM-480");
				PlayMovie();
			}
		}
		if (ImGui::CollapsingHeader("Intro FMV", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Intro", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "intro_movie");
				PlayMovie();
			}
			if (ImGui::Button("Attract", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "attract_movie");
				PlayMovie();
			}
			if (ImGui::Button("EA Logo", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "ealogo");
				PlayMovie();
			}
			if (ImGui::Button("PSA", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "psa");
				PlayMovie();
			}
		}
		if (ImGui::CollapsingHeader("Tutorial FMV", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::Button("Drag Tutorial", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "drag_tutorial");
				PlayMovie();
			}
			if (ImGui::Button("Sprint Tutorial", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "sprint_tutorial");
				PlayMovie();
			}
			if (ImGui::Button("Tollbooth Tutorial", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "tollbooth_tutorial");
				PlayMovie();
			}
			if (ImGui::Button("Pursuit Tutorial", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "pursuit_tutorial");
				PlayMovie();
			}
			if (ImGui::Button("Bounty Tutorial", ImVec2(ImGui::CalcItemWidth(), 0)))
			{
				strcpy(moviefilename_pointer, "bounty_tutorial");
				PlayMovie();
			}
		}
#endif
	}
	ImGui::Separator();
#endif
#endif
#ifndef OLD_NFS
	if (ImGui::CollapsingHeader("Rub Test", ImGuiTreeNodeFlags_None))
	{
#ifndef GAME_MW
#ifdef GAME_PS
		ImGui::Checkbox("Draw World", &bDrawWorld);
#else
		ImGui::Checkbox("Draw World", (bool*)DRAWWORLD_ADDR); // TODO - find / make the toggle for MW
#endif
#endif
		ImGui::Checkbox("Draw Cars", (bool*)DRAWCARS_ADDR);
	}
	ImGui::Separator();
#endif
	if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_None))
	{
#if defined(GAME_MW) || defined(GAME_UG2)
		if (ImGui::CollapsingHeader("Build Version", ImGuiTreeNodeFlags_None))
		{
#ifdef GAME_UG2
			ImGui::Text("Perforce Changelist Number: %d\nPerforce Changelist Name: %s\nBuild Machine: %s", *(int*)BUILDVERSIONCLNUMBER_ADDR, *(char**)BUILDVERSIONCLNAME_ADDR, *(char**)BUILDVERSIONMACHINE_ADDR); // TODO: make these flexible addresses...
#else
			ImGui::Text("Platform: %s\nBuild Type:%s%s\nPerforce Changelist Number: %d\nPerforce Changelist Name: %s\nBuild Date: %s\nBuild Machine: %s", (char*)BUILDVERSIONPLAT_ADDR, (char*)BUILDVERSIONNAME_ADDR, (char*)BUILDVERSIONOPTNAME_ADDR, *(int*)BUILDVERSIONCLNUMBER_ADDR, *(char**)BUILDVERSIONCLNAME_ADDR, *(char**)BUILDVERSIONDATE_ADDR, *(char**)BUILDVERSIONMACHINE_ADDR); // TODO: make these flexible addresses...
#endif
			ImGui::Separator();
		}
#endif
#ifdef HAS_DAL
		if (ImGui::CollapsingHeader("DAL Options", ImGuiTreeNodeFlags_None))
		{
			if (ImGui::CollapsingHeader("FrontEnd", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("FE Scale", (float*)FESCALE_POINTER, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::Checkbox("Widescreen mode", (bool*)WIDESCREEN_POINTER);
#ifdef GAME_CARBON
				ImGui::Separator();
				ImGui::Checkbox("Lap Info", (bool*)FELAPINFO_POINTER);
				ImGui::Checkbox("Score", (bool*)FESCORE_POINTER);
				ImGui::Checkbox("Leaderboard", (bool*)FELEADERBOARD_POINTER);
				ImGui::Checkbox("Crew Info", (bool*)FECREWINFO_POINTER);
				ImGui::Checkbox("Transmission Prompt", (bool*)FETRANSMISSIONPROMPT_POINTER);
				ImGui::Checkbox("Rearview Mirror", (bool*)FERVM_POINTER);
				ImGui::Checkbox("Picture In Picture", (bool*)FEPIP_POINTER);
				ImGui::Checkbox("Metric Speedo Units", (bool*)SPEEDOUNIT_POINTER);
#endif
			}
#ifdef GAME_CARBON
			if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_None))
			{
				ImGui::Checkbox("Rumble", (bool*)RUMBLEON_POINTER);
				ImGui::InputInt("PC Pad Index", (int*)PCPADIDX_POINTER, 1, 100);
				ImGui::InputInt("PC Device Type", (int*)PCDEVTYPE_POINTER, 1, 100);
			}
#endif
			if (ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_None))
			{
				ImGui::InputFloat("Highlight Cam", (float*)HIGHLIGHTCAM_POINTER, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
				ImGui::InputFloat("Time Of Day", (float*)TIMEOFDAY_POINTER, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_CharsScientific);
#ifdef GAME_CARBON
				ImGui::Checkbox("Jump Cam", (bool*)JUMPCAM_POINTER);
				ImGui::Checkbox("Car Damage", (bool*)DAMAGEON_POINTER);
				ImGui::Checkbox("Autosave", (bool*)AUTOSAVEON_POINTER);
#endif
			}
			if (ImGui::CollapsingHeader("Audio Levels", ImGuiTreeNodeFlags_None))
			{
				ImGui::SliderFloat("Master", (float*)MASTERVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Speech", (float*)SPEECHVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("FE Music", (float*)FEMUSICVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("IG Music", (float*)IGMUSICVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Sound Effects", (float*)SOUNDEFFECTSVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Engine", (float*)ENGINEVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Car", (float*)CARVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Ambient", (float*)AMBIENTVOL_POINTER, 0.0, 1.0);
				ImGui::SliderFloat("Speed", (float*)SPEEDVOL_POINTER, 0.0, 1.0);
			}

		}
		ImGui::Separator();
#endif
		if (ImGui::Button("Exit Game", ImVec2(ImGui::CalcItemWidth(), 0)))
		{
			*(int*)EXITGAMEFLAG_ADDR = 1;
		}
	}
	ImGui::Separator();
	ImGui::Text("GameFlow State: %s", GameFlowStateNames[*(int*)GAMEFLOWMGR_STATUS_ADDR]);
	ImGui::Separator();
	if (modified)
		save_config();
}
// NFS CODE END


#if RESHADE_ADDON
void reshade::runtime::draw_gui_addons()
{
	ini_file &config = global_config();

#if RESHADE_ADDON == 1
	if (!addon_enabled)
	{
		ImGui::PushTextWrapPos();
		ImGui::TextColored(COLOR_YELLOW, _("High network activity discovered.\nAll add-ons are disabled to prevent exploitation."));
		ImGui::PopTextWrapPos();
		return;
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(_("This build of ReShade has only limited add-on functionality."));
#else
	std::filesystem::path addon_search_path = L".\\";
	config.get("ADDON", "AddonPath", addon_search_path);
	if (imgui::directory_input_box(_("Add-on search path"), addon_search_path, _file_selection_path))
		config.set("ADDON", "AddonPath", addon_search_path);
#endif

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	imgui::search_input_box(_addons_filter, sizeof(_addons_filter));

	ImGui::Spacing();

	if (!addon_all_loaded)
	{
		ImGui::PushTextWrapPos();
#if RESHADE_ADDON == 1
		ImGui::TextColored(COLOR_YELLOW, _("Some add-ons were not loaded because this build of ReShade has only limited add-on functionality."));
#else
		ImGui::PushStyleColor(ImGuiCol_Text, COLOR_RED);
		ImGui::TextUnformatted(_("There were errors loading some add-ons."));
		ImGui::TextUnformatted(_("Check the log for more details."));
		ImGui::PopStyleColor();
#endif
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
	}

	if (ImGui::BeginChild("##addons", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + _imgui_context->Style.ItemSpacing.y)), ImGuiChildFlags_NavFlattened))
	{
		std::vector<std::string> disabled_addons;
		config.get("ADDON", "DisabledAddons", disabled_addons);
		std::vector<std::string> collapsed_or_expanded_addons;
		config.get("ADDON", "OverlayCollapsed", collapsed_or_expanded_addons);

		const float child_window_width = ImGui::GetContentRegionAvail().x;

		for (addon_info &info : addon_loaded_info)
		{
			const std::string name = !info.name.empty() ? info.name : std::filesystem::u8path(info.file).stem().u8string();

			if (!string_contains(name, _addons_filter))
				continue;

			ImGui::BeginChild(name.c_str(), ImVec2(child_window_width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);

			const bool builtin = (info.file == g_reshade_dll_path.filename().u8string());
			const std::string unique_name = builtin ? info.name : info.name + '@' + info.file;

			const auto collapsed_it = std::find(collapsed_or_expanded_addons.begin(), collapsed_or_expanded_addons.end(), unique_name);

			bool open = ImGui::GetStateStorage()->GetBool(ImGui::GetID("##addon_open"), builtin ? collapsed_it == collapsed_or_expanded_addons.end() : collapsed_it != collapsed_or_expanded_addons.end());
			if (ImGui::ArrowButton("##addon_open", open ? ImGuiDir_Down : ImGuiDir_Right))
			{
				ImGui::GetStateStorage()->SetBool(ImGui::GetID("##addon_open"), open = !open);

				if (builtin ? open : !open)
				{
					if (collapsed_it != collapsed_or_expanded_addons.end())
						collapsed_or_expanded_addons.erase(collapsed_it);
				}
				else
				{
					if (collapsed_it == collapsed_or_expanded_addons.end())
						collapsed_or_expanded_addons.push_back(unique_name);
				}

				config.set("ADDON", "OverlayCollapsed", collapsed_or_expanded_addons);
			}

			ImGui::SameLine();

			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(info.handle != nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled));

			const auto disabled_it = std::find_if(disabled_addons.begin(), disabled_addons.end(),
				[&info](const std::string_view addon_name) {
					const size_t at_pos = addon_name.find('@');
					if (at_pos == std::string_view::npos)
						return addon_name == info.name;
					return (at_pos == 0 || addon_name.substr(0, at_pos) == info.name) && addon_name.substr(at_pos + 1) == info.file;
				});

			bool enabled = (disabled_it == disabled_addons.end());
			if (ImGui::Checkbox(name.c_str(), &enabled))
			{
				if (enabled)
					disabled_addons.erase(disabled_it);
				else
					disabled_addons.push_back(unique_name);

				config.set("ADDON", "DisabledAddons", disabled_addons);
			}

			ImGui::PopStyleColor();

			if (enabled == (info.handle == nullptr))
			{
				ImGui::SameLine();
				ImGui::TextUnformatted(enabled ? _("(will be enabled on next application restart)") : _("(will be disabled on next application restart)"));
			}

			if (open)
			{
				ImGui::Spacing();
				ImGui::BeginGroup();

				if (!builtin)
					ImGui::Text(_("File:"));
				if (!info.author.empty())
					ImGui::Text(_("Author:"));
				if (info.version.value)
					ImGui::Text(_("Version:"));
				if (!info.description.empty())
					ImGui::Text(_("Description:"));
				if (!info.website_url.empty())
					ImGui::Text(_("Website:"));
				if (!info.issues_url.empty())
					ImGui::Text(_("Issues:"));

				ImGui::EndGroup();
				ImGui::SameLine(ImGui::GetWindowWidth() * 0.25f);
				ImGui::BeginGroup();

				if (!builtin)
					ImGui::TextUnformatted(info.file.c_str(), info.file.c_str() + info.file.size());
				if (!info.author.empty())
					ImGui::TextUnformatted(info.author.c_str(), info.author.c_str() + info.author.size());
				if (info.version.value)
					ImGui::Text("%u.%u.%u.%u", info.version.number.major, info.version.number.minor, info.version.number.build, info.version.number.revision);
				if (!info.description.empty())
				{
					ImGui::PushTextWrapPos();
					ImGui::TextUnformatted(info.description.c_str(), info.description.c_str() + info.description.size());
					ImGui::PopTextWrapPos();
				}
				if (!info.website_url.empty())
					ImGui::TextLinkOpenURL(info.website_url.c_str());
				if (!info.issues_url.empty())
					ImGui::TextLinkOpenURL(info.issues_url.c_str());

				ImGui::EndGroup();

				if (info.settings_overlay_callback != nullptr)
				{
					ImGui::Spacing();
					ImGui::Separator();
					ImGui::Spacing();

					info.settings_overlay_callback(this);
				}
			}

			ImGui::EndChild();
		}
	}
	ImGui::EndChild();

	ImGui::Spacing();

	ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(_("Open developer documentation")).x) / 2);
	ImGui::TextLinkOpenURL(_("Open developer documentation"), "https://reshade.me/docs");
}
#endif

#if RESHADE_FX
void reshade::runtime::draw_variable_editor()
{
	const ImVec2 popup_pos = ImGui::GetCursorScreenPos() + ImVec2(std::max(0.f, ImGui::GetContentRegionAvail().x * 0.5f - 200.0f), ImGui::GetFrameHeightWithSpacing());

	if (imgui::popup_button(_("Edit global preprocessor definitions"), ImGui::GetContentRegionAvail().x, ImGuiWindowFlags_NoMove))
	{
		ImGui::SetWindowPos(popup_pos);

		bool global_modified = false, preset_modified = false;
		float popup_height = (std::max(_global_preprocessor_definitions.size(), _preset_preprocessor_definitions[{}].size()) + 2) * ImGui::GetFrameHeightWithSpacing();
		popup_height = std::min(popup_height, ImGui::GetWindowViewport()->Size.y - popup_pos.y - 20.0f);
		popup_height = std::max(popup_height, 42.0f); // Ensure window always has a minimum height
		const float button_size = ImGui::GetFrameHeight();
		const float button_spacing = _imgui_context->Style.ItemInnerSpacing.x;

		ImGui::BeginChild("##definitions", ImVec2(30.0f * _font_size, popup_height));

		if (ImGui::BeginTabBar("##definition_types", ImGuiTabBarFlags_NoTooltip))
		{
			const float content_region_width = ImGui::GetContentRegionAvail().x;

			struct
			{
				std::string name;
				std::vector<std::pair<std::string, std::string>> &definitions;
				bool &modified;
			} definition_types[] = {
				{ _("Global"), _global_preprocessor_definitions, global_modified },
				{ _("Current Preset"), _preset_preprocessor_definitions[{}], preset_modified },
			};

			for (const auto &type : definition_types)
			{
				if (ImGui::BeginTabItem(type.name.c_str()))
				{
					for (auto it = type.definitions.begin(); it != type.definitions.end();)
					{
						char name[128];
						name[it->first.copy(name, sizeof(name) - 1)] = '\0';
						char value[256];
						value[it->second.copy(value, sizeof(value) - 1)] = '\0';

						ImGui::PushID(static_cast<int>(std::distance(type.definitions.begin(), it)));

						ImGui::SetNextItemWidth(content_region_width * 0.66666666f - (button_spacing));
						type.modified |= ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
							[](ImGuiInputTextCallbackData *data) -> int { return data->EventChar == '=' || (data->EventChar != '_' && !isalnum(data->EventChar)); }); // Filter out invalid characters

						ImGui::SameLine(0, button_spacing);

						ImGui::SetNextItemWidth(content_region_width * 0.33333333f - (button_spacing + button_size));
						type.modified |= ImGui::InputText("##value", value, sizeof(value));

						ImGui::SameLine(0, button_spacing);

						if (imgui::confirm_button(ICON_FK_MINUS, button_size, _("Do you really want to remove the preprocessor definition '%s'?"), name))
						{
							type.modified = true;
							it = type.definitions.erase(it);
						}
						else
						{
							if (type.modified)
							{
								it->first = name;
								it->second = value;
							}

							++it;
						}

						ImGui::PopID();
					}

					ImGui::Dummy(ImVec2());
					ImGui::SameLine(0, content_region_width - button_size);
					if (ImGui::Button(ICON_FK_PLUS, ImVec2(button_size, 0)))
						type.definitions.emplace_back();

					ImGui::EndTabItem();
				}
			}

			ImGui::EndTabBar();
		}

		ImGui::EndChild();

		if (global_modified)
			save_config();
		if (preset_modified)
			save_current_preset();
		if (global_modified || preset_modified)
			_was_preprocessor_popup_edited = true;

		ImGui::EndPopup();
	}
	else if (_was_preprocessor_popup_edited)
	{
		reload_effects();
		_was_preprocessor_popup_edited = false;
	}

	ImGui::BeginChild("##variables", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
	if (_variable_editor_tabs)
		ImGui::BeginTabBar("##variables", ImGuiTabBarFlags_TabListPopupButton | ImGuiTabBarFlags_FittingPolicyScroll);

	for (size_t effect_index = 0, id = 0; effect_index < _effects.size(); ++effect_index)
	{
		reshade::effect &effect = _effects[effect_index];

		// Hide variables that are not currently used in any of the active effects
		// Also skip showing this effect in the variable list if it doesn't have any uniform variables to show
		if (!effect.rendering || (effect.uniforms.empty() && effect.definitions.empty()))
			continue;
		assert(effect.compiled);

		bool force_reload_effect = false;
		const bool is_focused = _focused_effect == effect_index;
		const std::string effect_name = effect.source_file.filename().u8string();

		// Create separate tab for every effect file
		if (_variable_editor_tabs)
		{
			ImGuiTabItemFlags flags = 0;
			if (is_focused)
				flags |= ImGuiTabItemFlags_SetSelected;

			if (!ImGui::BeginTabItem(effect_name.c_str(), nullptr, flags))
				continue;
			// Begin a new child here so scrolling through variables does not move the tab itself too
			ImGui::BeginChild("##tab");
		}
		else
		{
			if (is_focused || _effects_expanded_state & 1)
				ImGui::SetNextItemOpen(is_focused || (_effects_expanded_state >> 1) != 0);

			if (!ImGui::TreeNodeEx(effect_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				continue; // Skip rendering invisible items
		}

		if (is_focused)
		{
			ImGui::SetScrollHereY(0.0f);
			_focused_effect = std::numeric_limits<size_t>::max();
		}

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(_imgui_context->Style.FramePadding.x, 0));
		if (imgui::confirm_button(
				(ICON_FK_UNDO " " + std::string(_("Reset all to default"))).c_str(),
				_variable_editor_tabs ? ImGui::GetContentRegionAvail().x : ImGui::CalcItemWidth(),
				_("Do you really want to reset all values in '%s' to their defaults?"), effect_name.c_str()))
		{
			// Reset all uniform variables
			for (uniform &variable_it : effect.uniforms)
				if (variable_it.special == special_uniform::none && !variable_it.annotation_as_uint("noreset"))
					reset_uniform_value(variable_it);

			// Reset all preprocessor definitions
			if (const auto preset_it = _preset_preprocessor_definitions.find({});
				preset_it != _preset_preprocessor_definitions.end() && !preset_it->second.empty())
			{
				for (const std::pair<std::string, std::string> &definition : effect.definitions)
				{
					if (const auto it = std::remove_if(preset_it->second.begin(), preset_it->second.end(),
							[&definition](const std::pair<std::string, std::string> &preset_definition) { return preset_definition.first == definition.first; });
						it != preset_it->second.end())
					{
						preset_it->second.erase(it, preset_it->second.end());
						force_reload_effect = true; // Need to reload after changing preprocessor defines so to get accurate defaults again
					}
				}
			}

			if (const auto preset_it = _preset_preprocessor_definitions.find(effect_name);
				preset_it != _preset_preprocessor_definitions.end() && !preset_it->second.empty())
			{
				_preset_preprocessor_definitions.erase(preset_it);
				force_reload_effect = true;
			}

			if (_auto_save_preset)
				save_current_preset();
			else
				_preset_is_modified = true;
		}
		ImGui::PopStyleVar();

		bool category_closed = false;
		bool category_visible = true;
		std::string current_category;

		size_t active_variable = 0;
		size_t active_variable_index = std::numeric_limits<size_t>::max();
		size_t hovered_variable = 0;
		size_t hovered_variable_index = std::numeric_limits<size_t>::max();

		for (size_t variable_index = 0; variable_index < effect.uniforms.size(); ++variable_index)
		{
			reshade::uniform &variable = effect.uniforms[variable_index];

			// Skip hidden and special variables
			if (variable.annotation_as_int("hidden") || variable.special != special_uniform::none)
			{
				if (variable.special == special_uniform::overlay_active)
					active_variable_index = variable_index;
				else if (variable.special == special_uniform::overlay_hovered)
					hovered_variable_index = variable_index;
				continue;
			}

			if (const std::string_view category = variable.annotation_as_string("ui_category");
				category != current_category)
			{
				current_category = category;

				if (!current_category.empty())
				{
					std::string category_label(get_localized_annotation(variable, "ui_category", _current_language));
					if (!_variable_editor_tabs)
					{
						for (float x = 0, space_x = ImGui::CalcTextSize(" ").x, width = (ImGui::CalcItemWidth() - ImGui::CalcTextSize(category_label.data()).x - 45) / 2; x < width; x += space_x)
							category_label.insert(0, " ");
						// Ensure widget ID does not change with varying width
						category_label += "###" + current_category;
						// Append a unique value so that the context menu does not contain duplicated widgets when a category is made current multiple times
						category_label += std::to_string(variable_index);
					}

					if (category_visible = true;
						variable.annotation_as_uint("ui_category_toggle") != 0)
						get_uniform_value(variable, &category_visible);

					if (category_visible)
					{
						ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen;
						if (!variable.annotation_as_int("ui_category_closed"))
							flags |= ImGuiTreeNodeFlags_DefaultOpen;

						category_closed = !ImGui::TreeNodeEx(category_label.c_str(), flags);

						if (ImGui::BeginPopupContextItem(category_label.c_str()))
						{
							char temp[64];
							const int temp_size = ImFormatString(temp, sizeof(temp), _("Reset all in '%s' to default"), current_category.c_str());

							if (imgui::confirm_button(
									(ICON_FK_UNDO " " + std::string(temp, temp_size)).c_str(),
									ImGui::GetContentRegionAvail().x,
									_("Do you really want to reset all values in '%s' to their defaults?"), current_category.c_str()))
							{
								for (uniform &variable_it : effect.uniforms)
									if (variable_it.special == special_uniform::none && !variable_it.annotation_as_uint("noreset") &&
										variable_it.annotation_as_string("ui_category") == category)
										reset_uniform_value(variable_it);

								if (_auto_save_preset)
									save_current_preset();
								else
									_preset_is_modified = true;
							}

							ImGui::EndPopup();
						}
					}
					else
					{
						category_closed = false;
					}
				}
				else
				{
					category_closed = false;
					category_visible = true;
				}
			}

			// Skip rendering invisible items
			if (category_closed || (!category_visible && !variable.annotation_as_uint("ui_category_toggle")))
				continue;

			bool modified = false;
			bool is_default_value = true;

			ImGui::PushID(static_cast<int>(id++));

			reshadefx::constant value;
			switch (variable.type.base)
			{
			case reshadefx::type::t_bool:
				get_uniform_value(variable, value.as_uint, variable.type.components());
				for (size_t i = 0; is_default_value && i < variable.type.components(); i++)
					is_default_value = (value.as_uint[i] != 0) == (variable.initializer_value.as_uint[i] != 0);
				break;
			case reshadefx::type::t_int:
			case reshadefx::type::t_uint:
				get_uniform_value(variable, value.as_int, variable.type.components());
				is_default_value = std::memcmp(value.as_int, variable.initializer_value.as_int, variable.type.components() * sizeof(int)) == 0;
				break;
			case reshadefx::type::t_float:
				const float threshold = variable.annotation_as_float("ui_step", 0, 0.001f) * 0.75f + FLT_EPSILON;
				get_uniform_value(variable, value.as_float, variable.type.components());
				for (size_t i = 0; is_default_value && i < variable.type.components(); i++)
					is_default_value = std::abs(value.as_float[i] - variable.initializer_value.as_float[i]) < threshold;
				break;
			}

#if RESHADE_ADDON
			if (invoke_addon_event<addon_event::reshade_overlay_uniform_variable>(this, api::effect_uniform_variable{ reinterpret_cast<uintptr_t>(&variable) }))
			{
				reshadefx::constant new_value;
				switch (variable.type.base)
				{
				case reshadefx::type::t_bool:
					get_uniform_value(variable, new_value.as_uint, variable.type.components());
					for (size_t i = 0; !modified && i < variable.type.components(); i++)
						modified = (new_value.as_uint[i] != 0) != (value.as_uint[i] != 0);
					break;
				case reshadefx::type::t_int:
				case reshadefx::type::t_uint:
					get_uniform_value(variable, new_value.as_int, variable.type.components());
					modified = std::memcmp(new_value.as_int, value.as_int, variable.type.components() * sizeof(int)) != 0;
					break;
				case reshadefx::type::t_float:
					get_uniform_value(variable, new_value.as_float, variable.type.components());
					for (size_t i = 0; !modified && i < variable.type.components(); i++)
						modified = std::abs(new_value.as_float[i] - value.as_float[i]) > FLT_EPSILON;
					break;
				}
			}
			else
#endif
			{
				// Add spacing before variable widget
				for (int i = 0, spacing = variable.annotation_as_int("ui_spacing"); i < spacing; ++i)
					ImGui::Spacing();

				// Add user-configurable text before variable widget
				if (const std::string_view text = get_localized_annotation(variable, "ui_text", _current_language);
					!text.empty())
				{
					ImGui::PushTextWrapPos();
					ImGui::TextUnformatted(text.data(), text.data() + text.size());
					ImGui::PopTextWrapPos();
				}

				ImGui::BeginDisabled(variable.annotation_as_uint("noedit") != 0);

				std::string_view label = get_localized_annotation(variable, "ui_label", _current_language);
				if (label.empty())
					label = variable.name;
				const std::string_view ui_type = variable.annotation_as_string("ui_type");

				switch (variable.type.base)
				{
					case reshadefx::type::t_bool:
					{
						if (ui_type == "button")
						{
							if (ImGui::Button(label.data(), ImVec2(ImGui::CalcItemWidth(), 0)))
							{
								value.as_uint[0] = 1;
								modified = true;
							}
							else if (value.as_uint[0] != 0)
							{
								// Reset value again next frame after button was pressed
								value.as_uint[0] = 0;
								modified = true;
							}
						}
						else if (ui_type == "combo")
							modified = imgui::combo_with_buttons(label.data(), reinterpret_cast<bool *>(&value.as_uint[0]));
						else
							modified = imgui::checkbox_list(label.data(), get_localized_annotation(variable, "ui_items", _current_language), value.as_uint, variable.type.components());

						if (modified)
							set_uniform_value(variable, value.as_uint, variable.type.components());
						break;
					}
					case reshadefx::type::t_int:
					case reshadefx::type::t_uint:
					{
						const int ui_min_val = variable.annotation_as_int("ui_min", 0, ui_type == "slider" ? 0 : std::numeric_limits<int>::lowest());
						const int ui_max_val = variable.annotation_as_int("ui_max", 0, ui_type == "slider" ? 1 : std::numeric_limits<int>::max());
						const int ui_stp_val = variable.annotation_as_int("ui_step", 0, 1);

						// Append units
						std::string format = "%d";
						format += get_localized_annotation(variable, "ui_units", _current_language);

						if (ui_type == "slider")
							modified = imgui::slider_with_buttons(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, format.c_str());
						else if (ui_type == "drag")
							modified = variable.annotation_as_int("ui_step") == 0 ?
								ImGui::DragScalarN(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, 1.0f, &ui_min_val, &ui_max_val, format.c_str()) :
								imgui::drag_with_buttons(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, format.c_str());
						else if (ui_type == "list")
							modified = imgui::list_with_buttons(label.data(), get_localized_annotation(variable, "ui_items", _current_language), &value.as_int[0]);
						else if (ui_type == "combo")
							modified = imgui::combo_with_buttons(label.data(), get_localized_annotation(variable, "ui_items", _current_language), &value.as_int[0]);
						else if (ui_type == "radio")
							modified = imgui::radio_list(label.data(), get_localized_annotation(variable, "ui_items", _current_language), &value.as_int[0]);
						else if (variable.type.is_matrix())
							for (unsigned int row = 0; row < variable.type.rows; ++row)
								modified |= ImGui::InputScalarN((std::string(label) + " [row " + std::to_string(row) + ']').c_str(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, &value.as_int[variable.type.cols * row], variable.type.cols) || modified;
						else
							modified = ImGui::InputScalarN(label.data(), variable.type.is_signed() ? ImGuiDataType_S32 : ImGuiDataType_U32, value.as_int, variable.type.rows);

						if (modified)
							set_uniform_value(variable, value.as_int, variable.type.components());
						break;
					}
					case reshadefx::type::t_float:
					{
						const float ui_min_val = variable.annotation_as_float("ui_min", 0, ui_type == "slider" ? 0.0f : std::numeric_limits<float>::lowest());
						const float ui_max_val = variable.annotation_as_float("ui_max", 0, ui_type == "slider" ? 1.0f : std::numeric_limits<float>::max());
						const float ui_stp_val = variable.annotation_as_float("ui_step", 0, 0.001f);

						// Calculate display precision based on step value
						std::string precision_format = "%.0f";
						for (float x = 1.0f; x * ui_stp_val < 1.0f && precision_format[2] < '9'; x *= 10.0f)
							++precision_format[2]; // This changes the text to "%.1f", "%.2f", "%.3f", ...

						// Append units
						precision_format += get_localized_annotation(variable, "ui_units", _current_language);

						if (ui_type == "slider")
							modified = imgui::slider_with_buttons(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str());
						else if (ui_type == "drag")
							modified = variable.annotation_as_float("ui_step") == 0.0f ?
								ImGui::DragScalarN(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str()) :
								imgui::drag_with_buttons(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows, &ui_stp_val, &ui_min_val, &ui_max_val, precision_format.c_str());
						else if (ui_type == "color" && variable.type.rows == 1)
							modified = imgui::slider_for_alpha_value(label.data(), value.as_float);
						else if (ui_type == "color" && variable.type.rows == 3)
							modified = ImGui::ColorEdit3(label.data(), value.as_float, ImGuiColorEditFlags_NoOptions);
						else if (ui_type == "color" && variable.type.rows == 4)
							modified = ImGui::ColorEdit4(label.data(), value.as_float, ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_AlphaBar);
						else if (variable.type.is_matrix())
							for (unsigned int row = 0; row < variable.type.rows; ++row)
								modified |= ImGui::InputScalarN((std::string(label) + " [row " + std::to_string(row) + ']').c_str(), ImGuiDataType_Float, &value.as_float[variable.type.cols * row], variable.type.cols) || modified;
						else
							modified = ImGui::InputScalarN(label.data(), ImGuiDataType_Float, value.as_float, variable.type.rows);

						if (modified)
							set_uniform_value(variable, value.as_float, variable.type.components());
						break;
					}
				}

				ImGui::EndDisabled();

				// Display tooltip
				if (const std::string_view tooltip = get_localized_annotation(variable, "ui_tooltip", _current_language);
					!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
				{
					if (ImGui::BeginTooltip())
					{
						ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
						ImGui::EndTooltip();
					}
				}
			}

			if (ImGui::IsItemActive())
				active_variable = variable_index + 1;
			if (ImGui::IsItemHovered())
				hovered_variable = variable_index + 1;

			// Create context menu
			if (ImGui::BeginPopupContextItem("##context"))
			{
				ImGui::SetNextItemWidth(18.0f * _font_size);
				if (variable.supports_toggle_key() &&
					_input != nullptr &&
					imgui::key_input_box("##toggle_key", variable.toggle_key_data, *_input))
					modified = true;

				if (ImGui::Button((ICON_FK_UNDO " " + std::string(_("Reset to default"))).c_str(), ImVec2(18.0f * _font_size, 0)))
				{
					modified = true;
					reset_uniform_value(variable);
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}

			if (!is_default_value && !variable.annotation_as_uint("noreset"))
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_FK_UNDO))
				{
					modified = true;
					reset_uniform_value(variable);
				}
			}

			if (variable.toggle_key_data[0] != 0)
			{
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
				ImGui::TextDisabled("%s", input::key_name(variable.toggle_key_data).c_str());
			}

			ImGui::PopID();

			// A value has changed, so save the current preset
			if (modified && !variable.annotation_as_uint("nosave"))
			{
				if (_auto_save_preset)
					save_current_preset();
				else
					_preset_is_modified = true;
			}
		}

		if (active_variable_index < effect.uniforms.size())
			set_uniform_value(effect.uniforms[active_variable_index], static_cast<uint32_t>(active_variable));
		if (hovered_variable_index < effect.uniforms.size())
			set_uniform_value(effect.uniforms[hovered_variable_index], static_cast<uint32_t>(hovered_variable));

		// Draw preprocessor definition list after all uniforms of an effect file
		std::vector<std::pair<std::string, std::string>> &effect_definitions = _preset_preprocessor_definitions[effect_name];
		std::vector<std::pair<std::string, std::string>>::iterator modified_definition;

		if (!effect.definitions.empty())
		{
			std::string category_label = _("Preprocessor definitions");
			if (!_variable_editor_tabs)
			{
				for (float x = 0, space_x = ImGui::CalcTextSize(" ").x, width = (ImGui::CalcItemWidth() - ImGui::CalcTextSize(category_label.c_str()).x - 45) / 2; x < width; x += space_x)
					category_label.insert(0, " ");
				category_label += "###ppdefinitions";
			}

			if (ImGui::TreeNodeEx(category_label.c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (const std::pair<std::string, std::string> &definition : effect.definitions)
				{
					std::vector<std::pair<std::string, std::string>> *definition_scope = nullptr;
					std::vector<std::pair<std::string, std::string>>::iterator definition_it;

					char value[256];
					if (get_preprocessor_definition(effect_name, definition.first, 0b111, definition_scope, definition_it))
						value[definition_it->second.copy(value, sizeof(value) - 1)] = '\0';
					else
						value[0] = '\0';

					if (ImGui::InputTextWithHint(definition.first.c_str(), definition.second.c_str(), value, sizeof(value), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
					{
						if (value[0] == '\0') // An empty value removes the definition
						{
							if (definition_scope == &effect_definitions)
							{
								force_reload_effect = true;
								definition_scope->erase(definition_it);
							}
						}
						else
						{
							force_reload_effect = true;

							if (definition_scope == &effect_definitions)
							{
								definition_it->second = value;
								modified_definition = definition_it;
							}
							else
							{
								effect_definitions.emplace_back(definition.first, value);
								modified_definition = effect_definitions.end() - 1;
							}
						}
					}

					if (force_reload_effect) // Cannot compare iterators if definitions were just modified above
						continue;

					if (ImGui::BeginPopupContextItem())
					{
						if (ImGui::Button((ICON_FK_UNDO " " + std::string(_("Reset to default"))).c_str(), ImVec2(18.0f * _font_size, 0)))
						{
							if (definition_scope != nullptr)
							{
								force_reload_effect = true;
								definition_scope->erase(definition_it);
							}

							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}

					if (definition_scope == &effect_definitions)
					{
						ImGui::PushID(definition_it->first.c_str());

						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_FK_UNDO))
						{
							force_reload_effect = true;
							definition_scope->erase(definition_it);
						}

						ImGui::PopID();
					}
				}
			}
		}

		if (_variable_editor_tabs)
		{
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		else
		{
			ImGui::TreePop();
		}

		if (force_reload_effect)
		{
			save_current_preset();

			_preset_is_modified = false;

			const bool reload_successful_before = _last_reload_successful;

			// Reload current effect file
			if (!reload_effect(effect_index) && modified_definition != std::vector<std::pair<std::string, std::string>>::iterator())
			{
				// The preprocessor definition that was just modified caused the effect to not compile, so reset to default and try again
				effect_definitions.erase(modified_definition);

				if (reload_effect(effect_index))
				{
					_last_reload_successful = reload_successful_before;
					ImGui::OpenPopup("##pperror"); // Notify the user about this

					// Update preset again now, so that the removed preprocessor definition does not reappear on a reload
					// The preset is actually loaded again next frame to update the technique status (see 'update_effects'), so cannot use 'save_current_preset' here
					ini_file::load_cache(_current_preset_path).set(effect_name, "PreprocessorDefinitions", effect_definitions);
				}
			}

			// Reloading an effect file invalidates all textures, but the statistics window may already have drawn references to those, so need to reset it
			if (ImGuiWindow *const statistics_window = ImGui::FindWindowByName("###statistics"))
				statistics_window->DrawList->CmdBuffer.clear();
		}
	}

	if (ImGui::BeginPopup("##pperror"))
	{
		ImGui::TextColored(COLOR_RED, _("The effect failed to compile after this change, so reverted back to the default value."));
		ImGui::EndPopup();
	}

	if (_variable_editor_tabs)
		ImGui::EndTabBar();
	ImGui::EndChild();
}
void reshade::runtime::draw_technique_editor()
{
	if (_reload_count != 0 && _effects.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, COLOR_YELLOW);
		ImGui::TextWrapped(_("No effect files (.fx) found in the configured effect search paths%c"), _effect_search_paths.empty() ? '.' : ':');
		for (const std::filesystem::path &search_path : _effect_search_paths)
			ImGui::Text("  %s", (g_reshade_base_path / search_path).lexically_normal().u8string().c_str());
		ImGui::Spacing();
		ImGui::TextWrapped(_("Go to the settings and configure the 'Effect search paths' option to point to the directory containing effect files, then hit 'Reload'!"));
		ImGui::PopStyleColor();
		return;
	}

	if (!_last_reload_successful)
	{
		// Add fake items at the top for effects that failed to compile
		for (size_t effect_index = 0; effect_index < _effects.size(); ++effect_index)
		{
			const effect &effect = _effects[effect_index];

			if (effect.compiled || effect.skipped)
				continue;

			ImGui::PushID(static_cast<int>(_technique_sorting.size() + effect_index));

			ImGui::PushStyleColor(ImGuiCol_Text, COLOR_RED);
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);

			{
				char label[128] = "";
				ImFormatString(label, sizeof(label), _("[%s] failed to compile"), effect.source_file.filename().u8string().c_str());

				bool value = false;
				ImGui::Checkbox(label, &value);
			}

			ImGui::PopItemFlag();

			// Display tooltip
			if (!effect.errors.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
			{
				if (ImGui::BeginTooltip())
				{
					ImGui::TextUnformatted(effect.errors.c_str(), effect.errors.c_str() + effect.errors.size());
					ImGui::EndTooltip();
				}
			}

			ImGui::PopStyleColor();

			// Create context menu
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::OpenPopup("##context", ImGuiPopupFlags_MouseButtonRight);

			if (ImGui::BeginPopup("##context"))
			{
				if (ImGui::Button((ICON_FK_FOLDER " " + std::string(_("Open folder in explorer"))).c_str(), ImVec2(18.0f * _font_size, 0)))
					utils::open_explorer(effect.source_file);

				ImGui::Separator();

				if (imgui::popup_button((ICON_FK_PENCIL " " + std::string(_("Edit source code"))).c_str(), 18.0f * _font_size))
				{
					std::unordered_map<std::string_view, std::string> file_errors_lookup;
					parse_errors(effect.errors,
						[&file_errors_lookup](const std::string_view file, int line, const std::string_view message) {
							file_errors_lookup[file] += std::string(file) + '(' + std::to_string(line) + "): " + std::string(message) + '\n';
						});

					const auto source_file_errors_it = file_errors_lookup.find(effect.source_file.u8string());
					if (source_file_errors_it != file_errors_lookup.end())
						ImGui::PushStyleColor(ImGuiCol_Text, source_file_errors_it->second.find("error") != std::string::npos ? COLOR_RED : COLOR_YELLOW);

					std::filesystem::path source_file;
					if (ImGui::MenuItem(effect.source_file.filename().u8string().c_str()))
						source_file = effect.source_file;

					if (source_file_errors_it != file_errors_lookup.end())
					{
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
						{
							if (ImGui::BeginTooltip())
							{
								ImGui::TextUnformatted(source_file_errors_it->second.c_str(), source_file_errors_it->second.c_str() + source_file_errors_it->second.size());
								ImGui::EndTooltip();
							}
						}

						ImGui::PopStyleColor();
					}

					if (!effect.included_files.empty())
					{
						ImGui::Separator();

						for (const std::filesystem::path &included_file : effect.included_files)
						{
							// Color file entries that contain warnings or errors
							const auto included_file_errors_it = file_errors_lookup.find(included_file.u8string());
							if (included_file_errors_it != file_errors_lookup.end())
								ImGui::PushStyleColor(ImGuiCol_Text, included_file_errors_it->second.find("error") != std::string::npos ? COLOR_RED : COLOR_YELLOW);

							std::filesystem::path display_path = included_file.lexically_relative(effect.source_file.parent_path());
							if (display_path.empty())
								display_path = included_file.filename();
							if (ImGui::MenuItem(display_path.u8string().c_str()))
								source_file = included_file;

							if (included_file_errors_it != file_errors_lookup.end())
							{
								if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
								{
									if (ImGui::BeginTooltip())
									{
										ImGui::TextUnformatted(included_file_errors_it->second.c_str(), included_file_errors_it->second.c_str() + included_file_errors_it->second.size());
										ImGui::EndTooltip();
									}
								}

								ImGui::PopStyleColor();
							}
						}
					}

					ImGui::EndPopup();

					if (!source_file.empty())
					{
						open_code_editor(effect_index, source_file);
						ImGui::CloseCurrentPopup();
					}
				}

				for (size_t permutation_index = 0; permutation_index < effect.permutations.size(); ++permutation_index)
				{
					std::string label = _("Show compiled results");
					if (effect.permutations.size() > 1)
						label += " (" + std::to_string(permutation_index) + ")";

					if (!effect.permutations[permutation_index].generated_code.empty() &&
						imgui::popup_button(label.c_str(), 18.0f * _font_size))
					{
						const bool open_generated_code = ImGui::MenuItem(_("Generated code"));

						ImGui::EndPopup();

						if (open_generated_code)
						{
							open_code_editor(effect_index, permutation_index, std::string());
							ImGui::CloseCurrentPopup();
						}
					}
				}

				ImGui::EndPopup();
			}

			ImGui::PopID();
		}
	}

	size_t force_reload_effect = std::numeric_limits<size_t>::max();
	size_t hovered_technique_index = std::numeric_limits<size_t>::max();

	for (size_t index = 0; index < _technique_sorting.size(); ++index)
	{
		const size_t technique_index = _technique_sorting[index];
		{
			technique &tech = _techniques[technique_index];
			const effect &effect = _effects[tech.effect_index];

			// Skip hidden techniques
			if (tech.hidden || !effect.compiled)
				continue;

			bool modified = false;

			ImGui::PushID(static_cast<int>(index));

			// Draw border around the item if it is selected
			const bool draw_border = _selected_technique == index;
			if (draw_border)
				ImGui::Separator();

			// Prevent user from disabling the technique when it is set to always be enabled via annotation
			const bool force_enabled = tech.annotation_as_int("enabled");

#if RESHADE_ADDON
			if (bool was_enabled = tech.enabled;
				invoke_addon_event<addon_event::reshade_overlay_technique>(this, api::effect_technique { reinterpret_cast<uintptr_t>(&tech) }))
			{
				modified = tech.enabled != was_enabled;
			}
			else
#endif
			{
				ImGui::BeginDisabled(tech.annotation_as_uint("noedit") != 0);

				// Gray out disabled techniques
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(tech.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));

				std::string label(get_localized_annotation(tech, "ui_label", _current_language));
				if (label.empty())
					label = tech.name;
				label += " [" + effect.source_file.filename().u8string() + ']';

				if (bool status = tech.enabled;
					ImGui::Checkbox(label.c_str(), &status) && !force_enabled)
				{
					modified = true;

					if (status)
						enable_technique(tech);
					else
						disable_technique(tech);
				}

				ImGui::PopStyleColor();

				ImGui::EndDisabled();

				// Display tooltip
				if (const std::string_view tooltip = get_localized_annotation(tech, "ui_tooltip", _current_language);
					!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
				{
					if (ImGui::BeginTooltip())
					{
						ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
						ImGui::EndTooltip();
					}
				}
			}

			if (ImGui::IsItemActive())
				_selected_technique = index;
			if (ImGui::IsItemClicked())
				_focused_effect = tech.effect_index;
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly | ImGuiHoveredFlags_AllowWhenDisabled))
				hovered_technique_index = index;

			// Create context menu
			if (ImGui::BeginPopupContextItem("##context"))
			{
				ImGui::TextUnformatted(tech.name.c_str(), tech.name.c_str() + tech.name.size());
				ImGui::Separator();

				ImGui::SetNextItemWidth(18.0f * _font_size);
				if (_input != nullptr && !force_enabled &&
					imgui::key_input_box("##toggle_key", tech.toggle_key_data, *_input))
				{
					if (_auto_save_preset)
						save_current_preset();
					else
						_preset_is_modified = true;
				}

				const bool is_not_top = index > 0;
				const bool is_not_bottom = index < _technique_sorting.size() - 1;

				if (is_not_top && ImGui::Button(_("Move to top"), ImVec2(18.0f * _font_size, 0)))
				{
					std::vector<size_t> technique_indices = _technique_sorting;
					technique_indices.insert(technique_indices.begin(), technique_indices[index]);
					technique_indices.erase(technique_indices.begin() + 1 + index);
					reorder_techniques(std::move(technique_indices));

					if (_auto_save_preset)
						save_current_preset();
					else
						_preset_is_modified = true;

					ImGui::CloseCurrentPopup();
				}
				if (is_not_bottom && ImGui::Button(_("Move to bottom"), ImVec2(18.0f * _font_size, 0)))
				{
					std::vector<size_t> technique_indices = _technique_sorting;
					technique_indices.push_back(technique_indices[index]);
					technique_indices.erase(technique_indices.begin() + index);
					reorder_techniques(std::move(technique_indices));

					if (_auto_save_preset)
						save_current_preset();
					else
						_preset_is_modified = true;

					ImGui::CloseCurrentPopup();
				}

				if (is_not_top || is_not_bottom || (_input != nullptr && !force_enabled))
					ImGui::Separator();

				if (ImGui::Button((ICON_FK_FOLDER " " + std::string(_("Open folder in explorer"))).c_str(), ImVec2(18.0f * _font_size, 0)))
					utils::open_explorer(effect.source_file);

				ImGui::Separator();

				if (imgui::popup_button((ICON_FK_PENCIL " " + std::string(_("Edit source code"))).c_str(), 18.0f * _font_size))
				{
					std::filesystem::path source_file;
					if (ImGui::MenuItem(effect.source_file.filename().u8string().c_str()))
						source_file = effect.source_file;

					if (!effect.preprocessed)
					{
						// Force preprocessor to run to update included files
						force_reload_effect = tech.effect_index;
					}
					else if (!effect.included_files.empty())
					{
						ImGui::Separator();

						for (const std::filesystem::path &included_file : effect.included_files)
						{
							std::filesystem::path display_path = included_file.lexically_relative(effect.source_file.parent_path());
							if (display_path.empty())
								display_path = included_file.filename();
							if (ImGui::MenuItem(display_path.u8string().c_str()))
								source_file = included_file;
						}
					}

					ImGui::EndPopup();

					if (!source_file.empty())
					{
						open_code_editor(tech.effect_index, source_file);
						ImGui::CloseCurrentPopup();
					}
				}

				for (size_t permutation_index = 0; permutation_index < effect.permutations.size(); ++permutation_index)
				{
					std::string label = _("Show compiled results");
					if (effect.permutations.size() > 1)
						label += " (" + std::to_string(permutation_index) + ")";

					if (!effect.permutations[permutation_index].generated_code.empty() &&
						imgui::popup_button(label.c_str(), 18.0f * _font_size))
					{
						const bool open_generated_code = ImGui::MenuItem(_("Generated code"));

						ImGui::Separator();

						std::string entry_point_name;
						for (const std::pair<std::string, reshadefx::shader_type> &entry_point : effect.permutations[permutation_index].module.entry_points)
							if (const auto assembly_it = effect.permutations[permutation_index].assembly_text.find(entry_point.first);
								assembly_it != effect.permutations[permutation_index].assembly_text.end() && ImGui::MenuItem(entry_point.first.c_str()))
								entry_point_name = entry_point.first;

						ImGui::EndPopup();

						if (open_generated_code || !entry_point_name.empty())
						{
							open_code_editor(tech.effect_index, permutation_index, entry_point_name);
							ImGui::CloseCurrentPopup();
						}
					}
				}

				ImGui::EndPopup();
			}

			if (tech.toggle_key_data[0] != 0)
			{
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
				ImGui::TextDisabled("%s", input::key_name(tech.toggle_key_data).c_str());
			}

			if (draw_border)
				ImGui::Separator();

			ImGui::PopID();

			if (modified)
			{
				if (_auto_save_preset)
					save_current_preset();
				else
					_preset_is_modified = true;
			}
		}
	}

	// Move the selected technique to the position of the mouse in the list
	if (_selected_technique < _technique_sorting.size() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		if (hovered_technique_index < _technique_sorting.size() && hovered_technique_index != _selected_technique)
		{
			std::vector<size_t> technique_indices = _technique_sorting;

			const auto move_technique = [this, &technique_indices](size_t from_index, size_t to_index) {
				if (to_index < from_index) // Up
					for (size_t i = from_index; to_index < i; --i)
						std::swap(technique_indices[i - 1], technique_indices[i]);
				else // Down
					for (size_t i = from_index; i < to_index; ++i)
						std::swap(technique_indices[i], technique_indices[i + 1]);
			};

			move_technique(_selected_technique, hovered_technique_index);

			// Pressing shift moves all techniques from the same effect file to the new location as well
			if (_imgui_context->IO.KeyShift)
			{
				for (size_t i = hovered_technique_index + 1, offset = 1; i < technique_indices.size(); ++i)
				{
					if (_techniques[technique_indices[i]].effect_index == _focused_effect)
					{
						if ((i - hovered_technique_index) > offset)
							move_technique(i, hovered_technique_index + offset);
						offset++;
					}
				}
				for (size_t i = hovered_technique_index - 1, offset = 0; i >= 0 && i != std::numeric_limits<size_t>::max(); --i)
				{
					if (_techniques[technique_indices[i]].effect_index == _focused_effect)
					{
						offset++;
						if ((hovered_technique_index - i) > offset)
							move_technique(i, hovered_technique_index - offset);
					}
				}
			}

			reorder_techniques(std::move(technique_indices));
			_selected_technique = hovered_technique_index;

			if (_auto_save_preset)
				save_current_preset();
			else
				_preset_is_modified = true;
			return;
		}
	}
	else
	{
		_selected_technique = std::numeric_limits<size_t>::max();
	}

	if (force_reload_effect != std::numeric_limits<size_t>::max())
	{
		reload_effect(force_reload_effect);

		// Reloading an effect file invalidates all textures, but the statistics window may already have drawn references to those, so need to reset it
		if (ImGuiWindow *const statistics_window = ImGui::FindWindowByName("###statistics"))
			statistics_window->DrawList->CmdBuffer.clear();
	}
}

void reshade::runtime::open_code_editor(size_t effect_index, size_t permutation_index, const std::string &entry_point)
{
	assert(effect_index < _effects.size());

	const std::filesystem::path &path = _effects[effect_index].source_file;

	if (const auto it = std::find_if(_editors.begin(), _editors.end(),
			[effect_index, permutation_index, &path, &entry_point](const editor_instance &instance) {
				return instance.effect_index == effect_index && instance.permutation_index == permutation_index && instance.file_path == path && instance.generated && instance.entry_point_name == entry_point;
			});
		it != _editors.end())
	{
		it->selected = true;
		open_code_editor(*it);
	}
	else
	{
		editor_instance instance { effect_index, permutation_index, path, entry_point, true, true };
		open_code_editor(instance);
		_editors.push_back(std::move(instance));
	}
}
void reshade::runtime::open_code_editor(size_t effect_index, const std::filesystem::path &path)
{
	assert(effect_index < _effects.size());

	if (const auto it = std::find_if(_editors.begin(), _editors.end(),
			[effect_index, &path](const editor_instance &instance) {
				return instance.effect_index == effect_index && instance.file_path == path && !instance.generated;
			});
		it != _editors.end())
	{
		it->selected = true;
		open_code_editor(*it);
	}
	else
	{
		editor_instance instance { effect_index, std::numeric_limits<size_t>::max(), path, std::string(), true, false };
		open_code_editor(instance);
		_editors.push_back(std::move(instance));
	}
}
void reshade::runtime::open_code_editor(editor_instance &instance) const
{
	const effect &effect = _effects[instance.effect_index];

	if (instance.generated)
	{
		if (instance.entry_point_name.empty())
			instance.editor.set_text(effect.permutations[instance.permutation_index].generated_code);
		else
			instance.editor.set_text(effect.permutations[instance.permutation_index].assembly_text.at(instance.entry_point_name));
		instance.editor.set_readonly(true);
		return; // Errors only apply to the effect source, not generated code
	}

	// Only update text if there is no undo history (in which case it can be assumed that the text is already up-to-date)
	if (!instance.editor.is_modified() && !instance.editor.can_undo())
	{
		if (FILE *const file = _wfsopen(instance.file_path.c_str(), L"rb", SH_DENYWR))
		{
			fseek(file, 0, SEEK_END);
			const size_t file_size = ftell(file);
			fseek(file, 0, SEEK_SET);

			std::string text(file_size, '\0');
			fread(text.data(), 1, file_size, file);

			fclose(file);

			instance.editor.set_text(text);
			instance.editor.set_readonly(false);
		}
	}

	instance.editor.clear_errors();

	parse_errors(effect.errors,
		[&instance](const std::string_view file, int line, const std::string_view message) {
			// Ignore errors that aren't in the current source file
			if (file != instance.file_path.u8string())
				return;

			instance.editor.add_error(line, message, message.find("error") == std::string::npos);
		});
}
void reshade::runtime::draw_code_editor(editor_instance &instance)
{
	if (!instance.generated && (
			ImGui::Button((ICON_FK_FLOPPY " " + std::string(_("Save"))).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) || (
			_input != nullptr && _input->is_key_pressed('S', true, false, false))))
	{
		// Write current editor text to file
		if (FILE *const file = _wfsopen(instance.file_path.c_str(), L"wb", SH_DENYWR))
		{
			const std::string text = instance.editor.get_text();
			fwrite(text.data(), 1, text.size(), file);
			fclose(file);
		}

		if (!is_loading() && instance.effect_index < _effects.size())
		{
			// Clear modified flag, so that errors are updated next frame (see 'update_and_render_effects')
			instance.editor.clear_modified();

			reload_effect(instance.effect_index);

			// Reloading an effect file invalidates all textures, but the statistics window may already have drawn references to those, so need to reset it
			if (ImGuiWindow *const statistics_window = ImGui::FindWindowByName("###statistics"))
				statistics_window->DrawList->CmdBuffer.clear();
		}
	}

	instance.editor.render("##editor", _editor_palette, false, _imgui_context->IO.Fonts->Fonts[_imgui_context->IO.Fonts->Fonts.Size - 1]);

	// Disable keyboard shortcuts when the window is focused so they don't get triggered while editing text
	const bool is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	_ignore_shortcuts |= is_focused;

	// Disable keyboard navigation starting with next frame when editor is focused so that the Alt key can be used without it switching focus to the menu bar
	if (is_focused)
		_imgui_context->IO.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	else // Enable navigation again if focus is lost
		_imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}
#endif

bool reshade::runtime::init_imgui_resources()
{
	// Adjust default font size based on the vertical resolution
	if (_font_size == 0)
		_editor_font_size = _font_size = _height >= 2160 ? 26 : _height >= 1440 ? 20 : 13;

	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	if (_imgui_sampler_state == 0)
	{
		api::sampler_desc sampler_desc = {};
		sampler_desc.filter = api::filter_mode::min_mag_mip_linear;
		sampler_desc.address_u = api::texture_address_mode::clamp;
		sampler_desc.address_v = api::texture_address_mode::clamp;
		sampler_desc.address_w = api::texture_address_mode::clamp;

		if (!_device->create_sampler(sampler_desc, &_imgui_sampler_state))
		{
			log::message(log::level::error, "Failed to create ImGui sampler object!");
			return false;
		}
	}

	if (_imgui_pipeline_layout == 0)
	{
		uint32_t num_layout_params = 0;
		api::pipeline_layout_param layout_params[3];

		if (has_combined_sampler_and_view)
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler_with_resource_view }; // s0
		}
		else
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler }; // s0
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::shader_resource_view }; // t0
		}

		layout_params[num_layout_params++] = api::constant_range { 0, 0, 0, 18, api::shader_stage::vertex | api::shader_stage::pixel }; // b0

		if (!_device->create_pipeline_layout(num_layout_params, layout_params, &_imgui_pipeline_layout))
		{
			log::message(log::level::error, "Failed to create ImGui pipeline layout!");
			return false;
		}
	}

	if (_imgui_pipeline != 0)
		return true;

	const resources::data_resource vs_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_VS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_VS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_VS_4_0 : IDR_IMGUI_VS_3_0);
	api::shader_desc vs_desc;
	vs_desc.code = vs_res.data;
	vs_desc.code_size = vs_res.data_size;

	const resources::data_resource ps_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_PS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_PS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_PS_4_0 : IDR_IMGUI_PS_3_0);
	api::shader_desc ps_desc;
	ps_desc.code = ps_res.data;
	ps_desc.code_size = ps_res.data_size;

	std::vector<api::pipeline_subobject> subobjects;
	subobjects.push_back({ api::pipeline_subobject_type::vertex_shader, 1, &vs_desc });
	subobjects.push_back({ api::pipeline_subobject_type::pixel_shader, 1, &ps_desc });

	const api::input_element input_layout[3] = {
		{ 0, "POSITION", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, pos), sizeof(ImDrawVert), 0 },
		{ 1, "TEXCOORD", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, uv ), sizeof(ImDrawVert), 0 },
		{ 2, "COLOR",    0, api::format::r8g8b8a8_unorm, 0, offsetof(ImDrawVert, col), sizeof(ImDrawVert), 0 }
	};
	subobjects.push_back({ api::pipeline_subobject_type::input_layout, 3, (void *)input_layout });

	api::primitive_topology topology = api::primitive_topology::triangle_list;
	subobjects.push_back({ api::pipeline_subobject_type::primitive_topology, 1, &topology });

	api::blend_desc blend_state;
	blend_state.blend_enable[0] = true;
	blend_state.source_color_blend_factor[0] = api::blend_factor::source_alpha;
	blend_state.dest_color_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.color_blend_op[0] = api::blend_op::add;
	blend_state.source_alpha_blend_factor[0] = api::blend_factor::one;
	blend_state.dest_alpha_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.alpha_blend_op[0] = api::blend_op::add;
	blend_state.render_target_write_mask[0] = 0xF;
	subobjects.push_back({ api::pipeline_subobject_type::blend_state, 1, &blend_state });

	api::rasterizer_desc rasterizer_state;
	rasterizer_state.cull_mode = api::cull_mode::none;
	rasterizer_state.scissor_enable = true;
	subobjects.push_back({ api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer_state });

	api::depth_stencil_desc depth_stencil_state;
	depth_stencil_state.depth_enable = false;
	depth_stencil_state.stencil_enable = false;
	subobjects.push_back({ api::pipeline_subobject_type::depth_stencil_state, 1, &depth_stencil_state });

	// Always choose non-sRGB format variant, since 'render_imgui_draw_data' is called with the non-sRGB render target (see 'draw_gui')
	api::format render_target_format = api::format_to_default_typed(_back_buffer_format, 0);
	subobjects.push_back({ api::pipeline_subobject_type::render_target_formats, 1, &render_target_format });

	if (!_device->create_pipeline(_imgui_pipeline_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &_imgui_pipeline))
	{
		log::message(log::level::error, "Failed to create ImGui pipeline!");
		return false;
	}

	return true;
}
void reshade::runtime::render_imgui_draw_data(api::command_list *cmd_list, ImDrawData *draw_data, api::resource_view rtv)
{
	// Need to multi-buffer vertex data so not to modify data below when the previous frame is still in flight
	const size_t buffer_index = _frame_count % std::size(_imgui_vertices);

	// Create and grow vertex/index buffers if needed
	if (_imgui_num_indices[buffer_index] < draw_data->TotalIdxCount)
	{
		if (_imgui_indices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle(); // Be safe and ensure nothing still uses this buffer

			_device->destroy_resource(_imgui_indices[buffer_index]);
		}

		const int new_size = draw_data->TotalIdxCount + 10000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawIdx), api::memory_heap::cpu_to_gpu, api::resource_usage::index_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_indices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui index buffer!");
			return;
		}

		_device->set_resource_name(_imgui_indices[buffer_index], "ImGui index buffer");

		_imgui_num_indices[buffer_index] = new_size;
	}
	if (_imgui_num_vertices[buffer_index] < draw_data->TotalVtxCount)
	{
		if (_imgui_vertices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle();

			_device->destroy_resource(_imgui_vertices[buffer_index]);
		}

		const int new_size = draw_data->TotalVtxCount + 5000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawVert), api::memory_heap::cpu_to_gpu, api::resource_usage::vertex_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_vertices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui vertex buffer!");
			return;
		}

		_device->set_resource_name(_imgui_vertices[buffer_index], "ImGui vertex buffer");

		_imgui_num_vertices[buffer_index] = new_size;
	}

#ifndef NDEBUG
	cmd_list->begin_debug_event("ReShade overlay");
#endif

	if (ImDrawIdx *idx_dst;
		_device->map_buffer_region(_imgui_indices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&idx_dst)))
	{
		for (int n = 0; n < draw_data->CmdListsCount; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
			idx_dst += draw_list->IdxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_indices[buffer_index]);
	}
	if (ImDrawVert *vtx_dst;
		_device->map_buffer_region(_imgui_vertices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&vtx_dst)))
	{
		for (int n = 0; n < draw_data->CmdListsCount; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
			vtx_dst += draw_list->VtxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_vertices[buffer_index]);
	}

	api::render_pass_render_target_desc render_target = {};
	render_target.view = rtv;

	cmd_list->begin_render_pass(1, &render_target, nullptr);

	// Setup render state
	cmd_list->bind_pipeline(api::pipeline_stage::all_graphics, _imgui_pipeline);

	cmd_list->bind_index_buffer(_imgui_indices[buffer_index], 0, sizeof(ImDrawIdx));
	cmd_list->bind_vertex_buffer(0, _imgui_vertices[buffer_index], 0, sizeof(ImDrawVert));

	const api::viewport viewport = { 0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.0f, 1.0f };
	cmd_list->bind_viewports(0, 1, &viewport);

	// Setup orthographic projection matrix
	const bool flip_y = (_renderer_id & 0x10000) != 0 && !_is_vr;
	const bool adjust_half_pixel = _renderer_id < 0xa000; // Bake half-pixel offset into matrix in D3D9
	const bool depth_clip_zero_to_one = (_renderer_id & 0x10000) == 0;

	const struct {
		float ortho_projection[16];
		api::color_space color_space;
		float hdr_overlay_brightness;
	} push_constants = {
		{
			2.0f / draw_data->DisplaySize.x, 0.0f, 0.0f, 0.0f,
			0.0f, (flip_y ? 2.0f : -2.0f) / draw_data->DisplaySize.y, 0.0f, 0.0f,
			0.0f,                            0.0f, depth_clip_zero_to_one ? 0.5f : -1.0f, 0.0f,
							   -(2 * draw_data->DisplayPos.x + draw_data->DisplaySize.x + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.x,
			(flip_y ? -1 : 1) * (2 * draw_data->DisplayPos.y + draw_data->DisplaySize.y + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.y, depth_clip_zero_to_one ? 0.5f : 0.0f, 1.0f,
		},
		_hdr_overlay_overwrite_color_space != api::color_space::unknown ?
			_hdr_overlay_overwrite_color_space :
			// Workaround for early HDR games, RGBA16F without a color space defined is pretty much guaranteed to be HDR for games
			_back_buffer_format == api::format::r16g16b16a16_float ?
				api::color_space::extended_srgb_linear : _back_buffer_color_space,
		_hdr_overlay_brightness
	};

	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	cmd_list->push_constants(api::shader_stage::vertex | api::shader_stage::pixel, _imgui_pipeline_layout, has_combined_sampler_and_view ? 1 : 2, 0, (_renderer_id != 0x9000 ? sizeof(push_constants) : sizeof(push_constants.ortho_projection)) / 4, &push_constants);
	if (!has_combined_sampler_and_view)
		cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler, &_imgui_sampler_state });

	int vtx_offset = 0, idx_offset = 0;
	for (int n = 0; n < draw_data->CmdListsCount; ++n)
	{
		const ImDrawList *const draw_list = draw_data->CmdLists[n];

		for (const ImDrawCmd &cmd : draw_list->CmdBuffer)
		{
			if (cmd.UserCallback != nullptr)
			{
				cmd.UserCallback(draw_list, &cmd);
				continue;
			}

			assert(cmd.TextureId != 0);

			const api::rect scissor_rect = {
				static_cast<int32_t>(cmd.ClipRect.x - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.w + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.y - draw_data->DisplayPos.y),
				static_cast<int32_t>(cmd.ClipRect.z - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.y + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.w - draw_data->DisplayPos.y)
			};

			cmd_list->bind_scissor_rects(0, 1, &scissor_rect);

			const api::resource_view srv = { (uint64_t)cmd.TextureId };
			if (has_combined_sampler_and_view)
			{
				api::sampler_with_resource_view sampler_and_view = { _imgui_sampler_state, srv };
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler_with_resource_view, &sampler_and_view });
			}
			else
			{
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 1, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::shader_resource_view, &srv });
			}

			cmd_list->draw_indexed(cmd.ElemCount, 1, cmd.IdxOffset + idx_offset, cmd.VtxOffset + vtx_offset, 0);
		}

		idx_offset += draw_list->IdxBuffer.Size;
		vtx_offset += draw_list->VtxBuffer.Size;
	}

	cmd_list->end_render_pass();

#ifndef NDEBUG
	cmd_list->end_debug_event();
#endif
}
void reshade::runtime::destroy_imgui_resources()
{
	_imgui_context->IO.Fonts->Clear();

	_device->destroy_resource(_font_atlas_tex);
	_font_atlas_tex = {};
	_device->destroy_resource_view(_font_atlas_srv);
	_font_atlas_srv = {};

	for (size_t i = 0; i < std::size(_imgui_vertices); ++i)
	{
		_device->destroy_resource(_imgui_indices[i]);
		_imgui_indices[i] = {};
		_imgui_num_indices[i] = 0;
		_device->destroy_resource(_imgui_vertices[i]);
		_imgui_vertices[i] = {};
		_imgui_num_vertices[i] = 0;
	}

	_device->destroy_sampler(_imgui_sampler_state);
	_imgui_sampler_state = {};
	_device->destroy_pipeline(_imgui_pipeline);
	_imgui_pipeline = {};
	_device->destroy_pipeline_layout(_imgui_pipeline_layout);
	_imgui_pipeline_layout = {};
}

bool reshade::runtime::open_overlay(bool open, api::input_source source)
{
#if RESHADE_ADDON
	if (!_is_in_api_call)
	{
		_is_in_api_call = true;
		const bool skip = invoke_addon_event<addon_event::reshade_open_overlay>(this, open, source);
		_is_in_api_call = false;
		if (skip)
			return false;
	}
#endif

	_show_overlay = open;

	if (open)
		_imgui_context->NavInputSource = static_cast<ImGuiInputSource>(source);

	return true;
}

#endif
