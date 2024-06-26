﻿#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <iostream>
#include <vector>
#include <heapapi.h>
#include <intsafe.h>

#include <d3d11.h>
#pragma comment (lib, "d3d11")

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

#include "minhook/minhook.h"
#pragma comment (lib, "minhook/minhook.lib")

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

using fPresent = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
fPresent pPresent = NULL;

using fResizeBuffers = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
fResizeBuffers pResizeBuffers = NULL;

WNDPROC pWndProc = NULL;

HWND hwndGame = NULL;

LPVOID pPresentAddress = NULL;
LPVOID pResizeBuffersAddress = NULL;

ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* render_view = nullptr;

static bool renderview_lost = true;

HMODULE game_module;

enum IDXGISwapChainvTable //for dx10 / dx11
{
	QUERY_INTERFACE,
	ADD_REF,
	RELEASE,
	SET_PRIVATE_DATA,
	SET_PRIVATE_DATA_INTERFACE,
	GET_PRIVATE_DATA,
	GET_PARENT,
	GET_DEVICE,
	PRESENT,
	GET_BUFFER,
	SET_FULLSCREEN_STATE,
	GET_FULLSCREEN_STATE,
	GET_DESC,
	RESIZE_BUFFERS,
	RESIZE_TARGET,
	GET_CONTAINING_OUTPUT,
	GET_FRAME_STATISTICS,
	GET_LAST_PRESENT_COUNT
};

namespace vars
{
	bool unload_library;
	bool menu_open;
	namespace visuals
	{
		bool enable;
		bool box;
		bool name;
		bool health;
		bool health_vampire;
		bool armor;
		bool draw_is_defeated;
	}
}

namespace memory_utils
{
	#ifdef _WIN64
		#define DWORD_OF_BITNESS DWORD64
		#define PTRMAXVAL ((PVOID)0x000F000000000000)
	#elif _WIN32
		#define DWORD_OF_BITNESS DWORD
		#define PTRMAXVAL ((PVOID)0xFFF00000)
	#endif

	bool is_valid_ptr(PVOID ptr)
	{
		return (ptr >= (PVOID)0x10000) && (ptr < PTRMAXVAL) && ptr != nullptr && !IsBadReadPtr(ptr, sizeof(ptr));
	}

	HMODULE base;

	HMODULE get_base()
	{
		if (!base)
			base = GetModuleHandle(0);
		return base;
	}

	DWORD_OF_BITNESS get_base_address()
	{
		return (DWORD_OF_BITNESS)get_base();
	}

	template<class T>
	void write(std::vector<DWORD_OF_BITNESS>address, T value)
	{
		size_t length_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < length_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return;

			if (i < length_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				T* writable_address = (T*)(relative_address + address[length_array]);
				*writable_address = value;
			}
		}
	}

	template<class T>
	T read(std::vector<DWORD_OF_BITNESS>address)
	{
		size_t length_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < length_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return T();

			if (i < length_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				T readable_address = *(T*)(relative_address + address[length_array]);
				return readable_address;
			}
		}
	}

	void write_string(std::vector<DWORD_OF_BITNESS>address, char* value)
	{
		size_t length_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < length_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return;

			if (i < length_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				char* writable_address = (char*)(relative_address + address[length_array]);
				*writable_address = *value;
			}
		}
	}

	char* read_string(std::vector<DWORD_OF_BITNESS>address)
	{
		size_t length_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < length_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return NULL;

			if (i < length_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				char* readable_address = (char*)(relative_address + address[length_array]);
				return readable_address;
			}
		}
	}

	DWORD_OF_BITNESS get_module_size(DWORD_OF_BITNESS address)
	{
		return PIMAGE_NT_HEADERS(address + (DWORD_OF_BITNESS)PIMAGE_DOS_HEADER(address)->e_lfanew)->OptionalHeader.SizeOfImage;
	}

	DWORD_OF_BITNESS find_pattern(HMODULE module, const char* pattern, const char* mask)
	{
		DWORD_OF_BITNESS base = (DWORD_OF_BITNESS)module;
		DWORD_OF_BITNESS size = get_module_size(base);

		DWORD_OF_BITNESS patternLength = (DWORD_OF_BITNESS)strlen(mask);

		for (DWORD_OF_BITNESS i = 0; i < size - patternLength; i++)
		{
			bool found = true;
			for (DWORD_OF_BITNESS j = 0; j < patternLength; j++)
			{
				found &= mask[j] == '?' || pattern[j] == *(char*)(base + i + j);
			}

			if (found)
			{
				return base + i;
			}
		}

		return NULL;
	}

	DWORD_OF_BITNESS address_is_valid_on_module(const DWORD_OF_BITNESS address, DWORD_OF_BITNESS lower, DWORD_OF_BITNESS higher)
	{
		return ((address < lower) || (address > higher));
	}

	DWORD_OF_BITNESS find_pattern_in_heap(const char* pattern, const char* mask)
	{
		DWORD_OF_BITNESS base = (DWORD_OF_BITNESS)GetProcessHeap();

		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);
		DWORD_OF_BITNESS size = (DWORD_OF_BITNESS)sysInfo.lpMaximumApplicationAddress;

		DWORD_OF_BITNESS patternLength = (DWORD_OF_BITNESS)strlen(mask);

		while (base < size)
		{
			MEMORY_BASIC_INFORMATION mbi;

			if (VirtualQuery((LPCVOID)base, &mbi, sizeof(mbi)) != NULL)
			{
				const DWORD_OF_BITNESS& start = (DWORD_OF_BITNESS)mbi.BaseAddress;
				const auto& end = mbi.RegionSize;

				if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE)
				{
					for (DWORD_OF_BITNESS i = 0; i < end - patternLength; i++)
					{
						bool found = true;
						for (DWORD_OF_BITNESS j = 0; j < patternLength; j++)
						{
							if (mask[j] == '?')
								continue;

							/*if (IsBadCodePtr((FARPROC)(base + i + j)) != NULL)
								continue;*/

							if (pattern[j] != *(char*)(base + i + j))
							{
								found = false;
								break;
							}
						}

						if (found)
						{
							return start + i;
						}
					}
				}
				base += end;
			}
			else
			{
				break;
			}
		}

		return NULL;
	}

	void patch_instruction(DWORD_OF_BITNESS instruction_address, const char* instruction_bytes, int sizeof_instruction_byte)
	{
		DWORD dwOldProtection;

		VirtualProtect((LPVOID)instruction_address, sizeof_instruction_byte, PAGE_EXECUTE_READWRITE, &dwOldProtection);

		memcpy((LPVOID)instruction_address, instruction_bytes, sizeof_instruction_byte);

		VirtualProtect((LPVOID)instruction_address, sizeof_instruction_byte, dwOldProtection, NULL);

		FlushInstructionCache(GetCurrentProcess(), (LPVOID)instruction_address, sizeof_instruction_byte);
	}
}

namespace console
{
	FILE* out;
	void attach(const char* title)
	{
		AllocConsole();
		freopen_s(&out, "conout$", "w", stdout);
		SetConsoleTitle(title);
	}
}

namespace drawing
{
	void AddCircle(const ImVec2& position, float radius, const ImColor& color, int segments)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddCircle(position, radius, ImGui::ColorConvertFloat4ToU32(color), segments);
	}

	void AddRect(const ImVec2& position, const ImVec2& size, const ImColor& color, float rounding = 0.f)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddRect(position, size, ImGui::ColorConvertFloat4ToU32(color), rounding);
	}

	void AddRectFilled(const ImVec2& position, const ImVec2& size, const ImColor& color, float rounding)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddRectFilled(position, size, ImGui::ColorConvertFloat4ToU32(color), rounding);
	}

	void DrawBox(float x, float y, float w, float h, const ImColor& color)
	{
		AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color);
	}

	void DrawFillArea(float x, float y, float w, float h, const ImColor& color, float rounding = 0.f)
	{
		AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), color, rounding);
	}

	void DrawEspBox(float x, float y, float w, float h, const ImColor& color)
	{
		if (vars::visuals::box == false)
			return;

		DrawBox(x, y, w, h, color);
	}

	enum
	{
		FL_NONE = 1 << 0,
		FL_SHADOW = 1 << 1,
		FL_OUTLINE = 1 << 2,
		FL_CENTER_X = 1 << 3,
		FL_CENTER_Y = 1 << 4
	};

	void AddText(float x, float y, const ImColor& color, int flags, const char* format, ...)
	{
		int style = 0;

		if (!format)
			return;

		auto& io = ImGui::GetIO();
		auto DrawList = ImGui::GetWindowDrawList();
		auto Font = io.Fonts->Fonts[0];

		char szBuff[256] = { '\0' };
		va_list vlist = nullptr;
		va_start(vlist, format);
		vsprintf_s(szBuff, format, vlist);
		va_end(vlist);

		DrawList->PushTextureID(io.Fonts->TexID);

		float size = Font->FontSize;
		ImVec2 text_size = Font->CalcTextSizeA(size, FLT_MAX, 0.f, szBuff);

		ImColor Color = ImColor(0.f, 0.f, 0.f, color.Value.w);

		if (flags & FL_CENTER_X)
			x -= text_size.x / 2.f;

		if (flags & FL_CENTER_Y)
			y -= text_size.x / 2.f;

		if (style == 1)
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);

		if (style == 2)
		{
			DrawList->AddText(Font, size, ImVec2(x, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x - 1.f, y), ImGui::ColorConvertFloat4ToU32(Color), szBuff);

			DrawList->AddText(Font, size, ImVec2(x - 1.f, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x - 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
		}

		DrawList->AddText(Font, size, ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(color), szBuff);
		DrawList->PopTextureID();
	}

	void DrawName(const char* pcszPlayerName, float x, float y, float w, ImColor col)
	{
		if (vars::visuals::name == false)
			return;

		if (pcszPlayerName == NULL)
			return;

		ImFont* Font = ImGui::GetIO().Fonts->Fonts[0];
		ImVec2 text_size = Font->CalcTextSizeA(Font->FontSize, FLT_MAX, 0, "");

		AddText(x + w / 2.f, y - text_size.y - 6.f, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w), FL_CENTER_X, u8"%s", pcszPlayerName);
	}

	enum LINE_STATUS_BAR
	{
		LEFT,
		RIGHT,
		UPPER,
		BOTTOM
	};

	void DrawStatusLine(float x, float y, float w, float h, float status_value, float max_of_status_value, ImColor col, LINE_STATUS_BAR status_side = LINE_STATUS_BAR::LEFT)
	{
		if (status_value == 0.f)
			return;

		status_value = ImClamp(status_value, 0.f, max_of_status_value);

		const auto size_h = h / max_of_status_value * status_value;
		const auto size_w = w / max_of_status_value * status_value;

		const auto thickness = 2.f;

		switch (status_side)
		{
		case LINE_STATUS_BAR::LEFT:
			DrawFillArea(x - thickness - 1.9f, y + h, thickness, -size_h, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w));
			break;
		case LINE_STATUS_BAR::RIGHT:
			DrawFillArea(x + w - thickness + (1.9f * 2.f), y + h, thickness, -size_h, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w));
			break;
		case LINE_STATUS_BAR::UPPER:
			DrawFillArea(x, y - thickness - 1.9f, size_w, thickness, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w));
			break;
		case LINE_STATUS_BAR::BOTTOM:
			DrawFillArea(x, y + h + thickness, size_w, thickness, ImColor(col.Value.x, col.Value.y, col.Value.z, col.Value.w));
			break;
		default:
			break;

		}
	}

	void DrawIfDefeated(float x, float y, float w, float h, float health)
	{
		if (vars::visuals::name == false)
			return;

		if (health <= 2.f)
		{
			drawing::AddText(x + w / 2.f, y + h, ImColor(1.f, 0.f, 0.f), drawing::FL_CENTER_X, "THIS ASS DEZTROYED");
		}
	}
}

namespace math_utils
{
	class Matrix4x4
	{
	public:
		union
		{
			struct
			{
				float        _11, _12, _13, _14;
				float        _21, _22, _23, _24;
				float        _31, _32, _33, _34;
				float        _41, _42, _43, _44;
			};
			float m[4][4];
		};
	};

	class Vector
	{
	public:
		Vector() {};
		Vector(float x, float y, float z) : x(x), y(y), z(z) {};

		Vector operator+(Vector other)
		{
			return Vector(x + other.x, y + other.y, z + other.z);
		}

		Vector operator-(Vector other)
		{
			return Vector(x - other.x, y - other.y, z - other.z);
		}

		float x, y, z;
	};
}

namespace game_utils
{
	math_utils::Matrix4x4 get_matrix()
	{
		return memory_utils::read<math_utils::Matrix4x4>({ memory_utils::get_base_address(), 0x254E620 }); //0x7FF7CD2CE620
	}
	
	DWORD64 get_entity_list()
	{
		return memory_utils::read<DWORD64>({ (DWORD64)game_module, 0xC80CB8, 0x80 });
	}

	class CDeceitProperties
	{
	public:

		static CDeceitProperties* get_player_by_id(DWORD64 entity_list, int id)
		{
			return memory_utils::read<CDeceitProperties*>({ entity_list, (DWORD64)((id + 1) * 0x8) });
		}

		CDeceitProperties* get_entity_class()
		{
			return this;
		}

		char* get_name()
		{
			return memory_utils::read_string({ (DWORD64)this, 0x3F8, 0x0 });
		}

		float get_health()
		{
			return memory_utils::read<float>({ (DWORD64)this, 0xF0 });
		}

		float get_vampire_health()
		{
			return memory_utils::read<float>({ (DWORD64)this, 0xF4 });
		}

		float get_armor()
		{
			return memory_utils::read<float>({ (DWORD64)this, 0xF8 });
		}

		bool get_is_vampire_now() //TerrorTransformAction
		{
			return (bool)memory_utils::read<DWORD64>({ (DWORD64)this, 0x6A8 }); //check is valid pointer of TerrorTransformAction (old vampire check)
		}

		DWORD64 player_entity() //CPlayer
		{
			return memory_utils::read<DWORD64>({ (DWORD64)this, 0x148 });
		}

		DWORD64 player_entity_movement_controller(DWORD64 player_entity) //CPlayerMovementController
		{
			return memory_utils::read<DWORD64>({ player_entity, 0x60, 0x78 });
		}

		math_utils::Vector get_origin_aabb_max(DWORD64 player_entity_movement_controller)
		{
			return memory_utils::read<math_utils::Vector>({ player_entity_movement_controller, 0x218 });
		}

		math_utils::Vector get_origin_aabb_min(DWORD64 player_entity_movement_controller)
		{
			return memory_utils::read<math_utils::Vector>({ player_entity_movement_controller, 0x1E8 });
		}
	};

	bool world_to_screen(math_utils::Matrix4x4 view_projection, const math_utils::Vector vIn, float* flOut)
	{
		float w = view_projection.m[0][3] * vIn.x + view_projection.m[1][3] * vIn.y + view_projection.m[2][3] * vIn.z + view_projection.m[3][3];
		flOut[2] = w;

		if (w < 0.01)
			return false;

		flOut[0] = view_projection.m[0][0] * vIn.x + view_projection.m[1][0] * vIn.y + view_projection.m[2][0] * vIn.z + view_projection.m[3][0];
		flOut[1] = view_projection.m[0][1] * vIn.x + view_projection.m[1][1] * vIn.y + view_projection.m[2][1] * vIn.z + view_projection.m[3][1];

		float invw = 1.0f / w;

		flOut[0] *= invw;
		flOut[1] *= invw;

		int width, height;

		auto io = ImGui::GetIO();
		width = io.DisplaySize.x;
		height = io.DisplaySize.y;

		float x = (float)width / 2;
		float y = (float)height / 2;

		x += 0.5 * flOut[0] * (float)width + 0.5;
		y -= 0.5 * flOut[1] * (float)height + 0.5;

		flOut[0] = x;
		flOut[1] = y;

		return true;
	}
}

namespace functions
{
	namespace visuals
	{
		void esp()
		{
			if (vars::visuals::enable == false)
				return;

			const auto entity_list = game_utils::get_entity_list();

			if (entity_list == NULL)
				return;

			const auto view_projection = game_utils::get_matrix();

			for (int i = 0; i < 6; i++)
			{
				const auto &entity = game_utils::CDeceitProperties::get_player_by_id(entity_list, i);

				if (entity == NULL)
					continue;

				auto health = entity->get_health();

				if (health <= 0.1f)
					continue;

				auto health_vampire = entity->get_vampire_health();

				auto armor = entity->get_armor();

				auto name = entity->get_name();

				if (name == NULL)
					continue;

				auto is_active_terror_mode = entity->get_is_vampire_now();

				auto player_entity = entity->player_entity();

				if (player_entity == NULL)
					continue;

				auto player_entity_movement_controller = entity->player_entity_movement_controller(player_entity);

				if (player_entity_movement_controller == NULL)
					continue;

				auto v_bottom = entity->get_origin_aabb_min(player_entity_movement_controller);
				auto v_top = entity->get_origin_aabb_max(player_entity_movement_controller);

				float out_bottom[3], out_top[3];
				if (game_utils::world_to_screen(view_projection, v_bottom, out_bottom)
					&& game_utils::world_to_screen(view_projection, v_top, out_top))
				{
					if (out_top[2] <= 0.45f) //resolve drawin' my box
						continue;

					float h = out_bottom[1] - out_top[1];
					float w = h / 2;
					float x = out_bottom[0] - w / 2;
					float y = out_top[1];

					auto color = health_vampire > 0.f ? ImColor(1.f, 0.4f, 0.4f) : ImColor(1.f, 1.f, 1.f);

					drawing::DrawEspBox(x, y, w, h, color);

					drawing::DrawName(name, x, y, w, ImColor(1.f, 1.f, 1.f));

					if (vars::visuals::health)
						drawing::DrawStatusLine(x, y, w, h, health, 100.f, ImColor(0.f, 1.f, 0.f));

					if (vars::visuals::health_vampire && is_active_terror_mode)
						drawing::DrawStatusLine(x, y, w, h, health_vampire, 300.f, ImColor(1.f, 0.f, 0.f), drawing::LINE_STATUS_BAR::RIGHT);

					if (vars::visuals::armor)
						drawing::DrawStatusLine(x, y, w, h, armor, 100.f, ImColor(0.4f, 04.f, 1.f), drawing::LINE_STATUS_BAR::BOTTOM);

					if (vars::visuals::draw_is_defeated)
						drawing::DrawIfDefeated(x, y, w, h, health);
				}
			}
		}
	}
	namespace misc
	{
		
	}
	void run()
	{
		visuals::esp();
	}
}

void initialize_imgui()
{
	ImGui::CreateContext();
	ImGui::StyleColorsClassic();

	auto& style = ImGui::GetStyle();

	style.FrameRounding = 3.f;
	style.ChildRounding = 3.f;
	style.ChildBorderSize = 1.f;
	style.ScrollbarSize = 0.6f;
	style.ScrollbarRounding = 3.f;
	style.GrabRounding = 3.f;
	style.WindowRounding = 0.f;

	style.Colors[ImGuiCol_FrameBg] = ImColor(200, 200, 200);
	style.Colors[ImGuiCol_FrameBgHovered] = ImColor(220, 220, 220);
	style.Colors[ImGuiCol_FrameBgActive] = ImColor(230, 230, 230);
	style.Colors[ImGuiCol_Separator] = ImColor(180, 180, 180);
	style.Colors[ImGuiCol_CheckMark] = ImColor(255, 172, 19);
	style.Colors[ImGuiCol_SliderGrab] = ImColor(255, 172, 19);
	style.Colors[ImGuiCol_SliderGrabActive] = ImColor(255, 172, 19);
	style.Colors[ImGuiCol_ScrollbarBg] = ImColor(120, 120, 120);
	style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(255, 172, 19);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarGrab);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarGrab);
	style.Colors[ImGuiCol_Header] = ImColor(160, 160, 160);
	style.Colors[ImGuiCol_HeaderHovered] = ImColor(200, 200, 200);
	style.Colors[ImGuiCol_Button] = ImColor(180, 180, 180);
	style.Colors[ImGuiCol_ButtonHovered] = ImColor(200, 200, 200);
	style.Colors[ImGuiCol_ButtonActive] = ImColor(230, 230, 230);
	style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.78f, 0.78f, 0.78f, 1.f);
	style.Colors[ImGuiCol_WindowBg] = ImColor(220, 220, 220, 0.7 * 255);
	style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.75f, 0.75f, 0.75f, 0.53f);
	style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.40f, 0.40f, 0.55f, 0.80f);
	style.Colors[ImGuiCol_Border] = ImVec4(0.72f, 0.72f, 0.72f, 0.70f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.77f, 0.77f, 0.77f, 0.83f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.75f, 0.75f, 0.75f, 0.87f);
	style.Colors[ImGuiCol_Text] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0.72f, 0.72f, 0.72f, 0.76f);
	style.Colors[ImGuiCol_PopupBg] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
	style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.81f, 0.81f, 0.81f, 1.00f);
	style.Colors[ImGuiCol_Tab] = ImVec4(0.61f, 0.61f, 0.61f, 0.79f);
	style.Colors[ImGuiCol_TabHovered] = ImVec4(0.71f, 0.71f, 0.71f, 0.80f);
	style.Colors[ImGuiCol_TabActive] = ImVec4(0.77f, 0.77f, 0.77f, 0.84f);
	style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.73f, 0.73f, 0.73f, 0.82f);
	style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.58f, 0.58f, 0.58f, 0.84f);

	auto& io = ImGui::GetIO();
	io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", 15.0f, NULL, io.Fonts->GetGlyphRangesCyrillic());
	ImGui_ImplWin32_Init(hwndGame);
	ImGui_ImplDX11_Init(device, context);
	ImGui_ImplDX11_CreateDeviceObjects();

	ImGuiWindowFlags flags_color_edit = ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoInputs;
	ImGui::SetColorEditOptions(flags_color_edit);
}

void begin_scene()
{
	if (vars::unload_library)
		return;

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();

	if (vars::menu_open)
	{
		ImGui::GetIO().MouseDrawCursor = true;
		ImGui::Begin("alternativehack.xyz: Deceit meme | credits: zerrocxste, guss.", &vars::menu_open);
		ImGui::BeginChild("functions", ImVec2(), true);
		ImGui::Text("Visuals");
		ImGui::Checkbox("Enable", &vars::visuals::enable);
		ImGui::Checkbox("Box", &vars::visuals::box);
		ImGui::Checkbox("Name", &vars::visuals::name);
		ImGui::Checkbox("Health", &vars::visuals::health);
		ImGui::Checkbox("Health vampire", &vars::visuals::health_vampire);
		ImGui::Checkbox("Armor", &vars::visuals::armor);
		ImGui::Checkbox("Draw is defeated", &vars::visuals::draw_is_defeated);
		ImGui::EndChild();
		//ImGui::End();
		ImGui::EndWithShadow();
	}
	else
	{
		ImGui::GetIO().MouseDrawCursor = false;
	}

	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4());
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::Begin("##BackBuffer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
	ImGui::SetWindowPos(ImVec2(), ImGuiCond_Always);
	ImGui::SetWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y), ImGuiCond_Always);

	functions::run();

	ImGui::GetCurrentWindow()->DrawList->PushClipRectFullScreen();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();

	ImGui::EndFrame();

	ImGui::Render();

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT WINAPI wndproc_hooked(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	static auto once = []()
	{
		std::cout << __FUNCTION__ << " > first called!" << std::endl;
		return true;
	}();

	if (Msg == WM_KEYDOWN && wParam == VK_INSERT)
		vars::menu_open = !vars::menu_open;

	if (vars::menu_open)
	{
		ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
		return TRUE;
	}

	return CallWindowProc(pWndProc, hWnd, Msg, wParam, lParam);
}

HRESULT WINAPI present_hooked(IDXGISwapChain* pChain, UINT SyncInterval, UINT Flags)
{
	if (renderview_lost)
	{
		if (SUCCEEDED(pChain->GetDevice(__uuidof(ID3D11Device), (void**)&device)))
		{
			device->GetImmediateContext(&context);

			ID3D11Texture2D* pBackBuffer;
			pChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
			device->CreateRenderTargetView(pBackBuffer, NULL, &render_view);
			pBackBuffer->Release();

			std::cout << __FUNCTION__ << " > renderview successfully received!" << std::endl;
			renderview_lost = false;
		}
	}

	static auto once = [pChain, SyncInterval, Flags]()
	{
		initialize_imgui();
		std::cout << __FUNCTION__ << " > first called!" << std::endl;
		return true;
	}();

	context->OMSetRenderTargets(1, &render_view, NULL);

	begin_scene();

	return pPresent(pChain, SyncInterval, Flags);
}

HRESULT WINAPI resizebuffers_hooked(IDXGISwapChain* pChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT Flags)
{
	static auto once = []()
	{
		std::cout << __FUNCTION__ << " > first called!" << std::endl;
		return true;
	}();

	render_view->Release();
	render_view = nullptr;
	renderview_lost = true;

	ImGui_ImplDX11_CreateDeviceObjects();

	ImGui_ImplDX11_InvalidateDeviceObjects();

	return pResizeBuffers(pChain, BufferCount, Width, Height, NewFormat, Flags);
}

void hook_dx11(HMODULE module)
{
	D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
	DXGI_SWAP_CHAIN_DESC scd{};
	ZeroMemory(&scd, sizeof(scd));
	scd.BufferCount = 1;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	scd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	scd.OutputWindow = hwndGame;
	scd.SampleDesc.Count = 1;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	scd.Windowed = true;
	scd.BufferDesc.RefreshRate.Numerator = 60;
	scd.BufferDesc.RefreshRate.Denominator = 1;

	IDXGISwapChain* swapchain = nullptr;

	if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, NULL, &feature_level, 1, D3D11_SDK_VERSION, &scd, &swapchain, &device, NULL, &context)))
	{
		std::cout << __FUNCTION__ << " > failed to create device\n";
		return;
	}

	void** vtable_swapchain = *(void***)swapchain;

	swapchain->Release();
	swapchain = nullptr;

	pPresentAddress = vtable_swapchain[IDXGISwapChainvTable::PRESENT];

	if (MH_CreateHook(pPresentAddress, &present_hooked, (LPVOID*)&pPresent) != MH_OK ||
		MH_EnableHook(pPresentAddress) != MH_OK)
	{
		std::cout << __FUNCTION__ << " > failed create hook present\n";
		FreeLibraryAndExitThread(module, 1);
	}

	pResizeBuffersAddress = vtable_swapchain[IDXGISwapChainvTable::RESIZE_BUFFERS];

	if (MH_CreateHook(pResizeBuffersAddress, &resizebuffers_hooked, (LPVOID*)&pResizeBuffers) != MH_OK || 
		MH_EnableHook(pResizeBuffersAddress) != MH_OK)
	{
		std::cout << __FUNCTION__ << " > failed create hook resizebuffers\n";
		FreeLibraryAndExitThread(module, 1);
	}

	
}

void unhook(LPVOID address)
{
	MH_DisableHook(address);
	MH_RemoveHook(address);
	Sleep(100);
}

void hook_wndproc(HMODULE module)
{
	pWndProc = (WNDPROC)SetWindowLongPtr(hwndGame, GWLP_WNDPROC, (LONG_PTR)wndproc_hooked);

	if (pWndProc == NULL)
	{
		std::cout << __FUNCTION__ << " > failed hook wndproc\n";
		FreeLibraryAndExitThread(module, 1);
	}
}

void unhook_wndproc()
{
	SetWindowLongPtr(hwndGame, GWLP_WNDPROC, (LONG)pWndProc);
	Sleep(100);
}

void hack_thread(HMODULE module)
{
	console::attach("alternative.xyz: Deceit meme");

	std::cout << __FUNCTION__ << " > attach success\n";

	hwndGame = FindWindow(NULL, "Deceit");

	if (hwndGame == NULL)
	{
		std::cout << __FUNCTION__ << " > game window not found\n";
		FreeLibraryAndExitThread(module, 1);
	}

	game_module = GetModuleHandle("Game.DLL");

	if (!game_module)
	{
		std::cout << __FUNCTION__ << " > game.dll not found\n";
		FreeLibraryAndExitThread(module, 1);
	}

	MH_Initialize();

	hook_dx11(module);

	hook_wndproc(module);

	//Address of signature = Game.DLL + 0x00383EE5
	//"\x89\x58\x00\xB0", "xx?x"
	//	"89 58 ? B0"

	while (true)
	{
		if (GetAsyncKeyState(VK_DELETE))
		{
			vars::unload_library = true;
			break;
		}

		Sleep(100);
	}

	unhook(pPresentAddress);

	unhook(pResizeBuffersAddress);

	render_view->Release();
	render_view = nullptr;

	unhook_wndproc();

	MH_Uninitialize();

	std::cout << __FUNCTION__ << " > free library...\n";

	FreeLibraryAndExitThread(module, 0);
}

BOOL APIENTRY DllMain( HMODULE hModule,
					   DWORD  ul_reason_for_call,
					   LPVOID lpReserved
					 )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)hack_thread, hModule, 0, 0);
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

