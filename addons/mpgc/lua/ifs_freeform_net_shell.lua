-- Require native bindings before extending shell screens and their callbacks.
if ifs_freeform_net_loaded or type(ScriptCB_UpdateMPMetagame) ~= "function" then
	return
end

ifs_freeform_net_loaded = true

ScriptCB_DoFile("ifs_freeform_net_campaign")
ScriptCB_DoFile("ifs_freeform_net_chat")
ScriptCB_DoFile("ifs_freeform_net_screens")
ScriptCB_DoFile("ifs_freeform_net_controls")
ScriptCB_DoFile("ifs_freeform_net_presentation")
ScriptCB_DoFile("ifs_freeform_net_battle")
ScriptCB_DoFile("ifs_freeform_net_inventory")
ScriptCB_DoFile("ifs_freeform_net_actions")
ScriptCB_DoFile("ifs_freeform_net_decisions")

-- Save operations are already registered when the custom GC entry loads.
if ifs_saveop then
	ifs_freeform_net_fnWrapSaveUpdate(ifs_saveop)
	ifs_freeform_net_fnWrapDiskSave()
end

local net_source_index

-- Apply the GC source label after stock callbacks, preserving their results.
function ifs_freeform_net_fnFinishSource(this, ...)
	if this.mpgc_selected then
		RoundIFButtonLabel_fnSetString(this.source_button, "Galactic Conquest")
		IFObj_fnSetVis(this.source_button, true)
	end

	if this.mpgc_refresh_browser then
		this.mpgc_refresh_browser()
	end

	return unpack(arg, 1, arg.n)
end

-- Preserve the GC label after asynchronous stock browser refreshes.
function ifs_freeform_net_fnWrapJoin()
	local previous_show = ifs_mp_sessionlist_fnShowHideInterface
	ifs_mp_sessionlist_fnShowHideInterface = function(this, hide, ...)
		return ifs_freeform_net_fnFinishSource(this,
			previous_show(this, hide, unpack(arg, 1, arg.n)))
	end

	local previous_connect = ifs_mp_sessionlist_fnConnectTypeUpdate
	ifs_mp_sessionlist_fnConnectTypeUpdate = function(this, ...)
		return ifs_freeform_net_fnFinishSource(this, previous_connect(this, unpack(arg, 1, arg.n)))
	end

	local previous_dropbox = ifs_mp_sessionlist_fnShowSourceDropbox
	ifs_mp_sessionlist_fnShowSourceDropbox = function(this, enable, ...)
		if enable then
			sourcelist_layout.SelectedIdx = this.mpgc_selected and net_source_index or this.source_value
			sourcelist_layout.CursorIdx = sourcelist_layout.SelectedIdx
			ListManager_fnFillContents(this.source_listbox, sourcelist_content, sourcelist_layout)
		end

		return ifs_freeform_net_fnFinishSource(this,
			previous_dropbox(this, enable, unpack(arg, 1, arg.n)))
	end
end

-- Add a third UI choice while keeping stock source values Internet=1 and LAN=2.
function ifs_freeform_net_fnAddSource(this, click_source, show_content)
	if not net_source_index then
		table.insert(sourcelist_content, { string = "Galactic Conquest" })
		net_source_index = table.getn(sourcelist_content)
	end

	local source_width = 230
	sourcelist_layout.showcount = net_source_index
	sourcelist_layout.width = source_width
	sourcelist_layout.yHeight = ScriptCB_GetFontHeight("gamefont_small")
	sourcelist_layout.yTop = nil -- Recalculate after the stock two-row list was built.
	local source_height = sourcelist_layout.showcount
		* (sourcelist_layout.yHeight + sourcelist_layout.ySpacing) + 16
	this.source_button = NewPCDropDownButton {
		ScreenRelativeX = 0, ScreenRelativeY = 0,
		x = source_width * 0.5, y = 40,
		btnw = source_width, btnh = 20,
		font = "gamefont_small",
		tag = "_source",
		string = "ifs.mp.join.internet",
	}
	this.source_listbox = NewButtonWindow {
		ScreenRelativeX = 0, ScreenRelativeY = 0,
		x = source_width * 0.5, y = 50 + source_height * 0.5,
		ZPos = 0,
		bg_texture = "border_dropdown",
		width = source_width, height = source_height,
	}
	ListManager_fnInitList(this.source_listbox, sourcelist_layout)
	ListManager_fnFillContents(this.source_listbox, sourcelist_content, sourcelist_layout)

	-- Run presentation after stock callbacks without losing optional return values.
	local function finish_presentation(this, ...)
		if show_content then
			show_content(this)
		end

		return ifs_freeform_net_fnFinishSource(this, unpack(arg, 1, arg.n))
	end

	local previous_enter = this.Enter
	this.Enter = function(this, ...)
		ifs_freeform_net_fnSetBrowser(this)
		return finish_presentation(this, previous_enter(this, unpack(arg, 1, arg.n)))
	end

	local previous_accept = this.Input_Accept
	this.Input_Accept = function(this, ...)
		if this.CurButton and (string.sub(this.CurButton, 1, 5) == "_tab_"
			or string.sub(this.CurButton, 1, 5) == "_opt_") then
			return previous_accept(this, unpack(arg, 1, arg.n))
		end

		if this.bSourceDropBoxesOpen or this.CurButton == "_source" then
			if this.bSourceDropBoxesOpen and gMouseListBox == this.source_listbox then
				local selected = sourcelist_layout.CursorIdx
				if selected then
					this.mpgc_selected = selected == net_source_index
					ifs_freeform_net_fnSetBrowser(this)
					if this.mpgc_selected then
						-- Stock helpers still apply their login clamp and browser lifecycle.
						sourcelist_layout.CursorIdx = 1
					end
				end
			end

			return finish_presentation(this, click_source(this, unpack(arg, 1, arg.n)))
		end

		if this.mpgc_selected and this.mpgc_create then
			return ifs_freeform_net_fnCreateAccept(this, unpack(arg, 1, arg.n))
		end

		return previous_accept(this, unpack(arg, 1, arg.n))
	end
end

ScriptCB_DoFile("ifs_freeform_net_menus")

local previous_add_screen = AddIFScreen
local net_create_registration

-- Preserve an existing registration wrapper and its optional arguments and results.
function AddIFScreen(this, name, ...)
	if name == "ifs_freeform_load" or name == "ifs_saveop" then
		ifs_freeform_net_fnWrapSaveUpdate(this)
	end

	if name == "ifs_mp_sessionlist" then
		ifs_freeform_net_fnBuildBrowser(this)
		ifs_freeform_net_fnAddSource(this, ifs_mp_sessionlist_fnClickSourceButtons)
		ifs_freeform_net_fnWrapJoin()
	elseif name == "ifs_missionselect_pcMulti" then
		-- Stock loads the save-row constructors after Create; register its complete controls at that boundary.
		net_create_registration = { screen = this, arguments = arg }
		return
	elseif name == "ifs_freeform_load" then
		ifs_freeform_net_fnWrapLoad(this)
		if net_create_registration then
			local pending = net_create_registration
			net_create_registration = nil
			ifs_freeform_net_fnBuildCreate(pending.screen)
			ifs_freeform_net_fnAddSource(pending.screen,
				ifs_missionselect_pcMulti_fnClickSourceButtons, ifs_freeform_net_fnShowCreateContent)
			previous_add_screen(pending.screen, "ifs_missionselect_pcMulti",
				unpack(pending.arguments, 1, pending.arguments.n))
		end
	elseif name == "ifs_mp_lobby_quick" then
		ifs_freeform_net_fnWrapQuickLobby(this)
	elseif name == "ifs_freeform_menu" then
		ifs_freeform_net_fnWrapCampaignMenu(this)
	elseif name == "ifs_instant_options" then
		ifs_freeform_net_fnWrapOptions(this)
	elseif name == "ifs_freeform_main" then
		ifs_freeform_net_fnWrapCampaign(this)
	elseif name == "ifs_freeform_sides" then
		ifs_freeform_net_fnWrapSides(this)
	elseif name == "ifs_freeform_fleet" then
		ifs_freeform_net_fnWrapFleet(this)
	elseif name == "ifs_freeform_purchase_unit" or name == "ifs_freeform_purchase_tech" then
		ifs_freeform_net_fnWrapPurchase(this, name == "ifs_freeform_purchase_unit")
	elseif name == "ifs_freeform_focus" then
		ifs_freeform_net_fnWrapSessionUpdate(this)
	elseif name == "ifs_freeform_ai" then
		ifs_freeform_net_fnWrapAI(this)
	elseif name == "ifs_freeform_end" then
		ifs_freeform_net_fnWrapCampaignEnd(this)
	elseif name == "ifs_freeform_summary" or name == "ifs_freeform_battle"
		or name == "ifs_freeform_battle_mode" or name == "ifs_freeform_battle_card"
		or name == "ifs_freeform_result" then
		ifs_freeform_net_fnWrapDecisionScreen(this, name)
	elseif name == "ifs_mp_lobby" then
		ifs_freeform_net_fnWrapPlayerList(this)
	end

	if name == "ifs_freeform_sides" or name == "ifs_freeform_fleet" or name == "ifs_freeform_focus"
		or name == "ifs_freeform_purchase_unit" or name == "ifs_freeform_purchase_tech"
		or name == "ifs_freeform_menu" or name == "ifs_mp_lobby"
		or name == "ifs_freeform_main" or name == "ifs_freeform_ai"
		or name == "ifs_freeform_summary" or name == "ifs_freeform_battle"
		or name == "ifs_freeform_battle_mode" or name == "ifs_freeform_battle_card"
		or name == "ifs_freeform_result" or name == "ifs_freeform_end" then
		ifs_freeform_net_fnBuildChat(this)
		if this.player then
			local width = ScriptCB_GetSafeScreenInfo()
			this.player.mpgc_turn = NewIFText {
				x = width * 0.5 - 148, y = 0,
				textw = 140, texth = 30,
				font = "gamefont_small",
				halign = "right", valign = "vcenter",
				nocreatebackground = 1,
				bCreateHidden = 1,
			}
			this.player.mpgc_timer = NewIFText {
				x = width * 0.5 - 148, y = 24,
				textw = 140, texth = 20,
				font = "gamefont_small",
				halign = "right", valign = "vcenter",
				nocreatebackground = 1,
				bCreateHidden = 1,
			}
		end
	end

	return previous_add_screen(this, name, unpack(arg, 1, arg.n))
end
