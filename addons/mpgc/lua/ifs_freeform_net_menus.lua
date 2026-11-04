-- Preserve optional nil return slots while temporary stock-loader bindings are restored.
local function net_menu_results(...)
	return arg
end

ScriptCB_DoFile("ifs_freeform_net_browser")

-- Retain the native victory setter's resolved objective without adding fields to campaign saves.
function ifs_freeform_net_fnWrapVictorySummary(main)
	if main.mpgc_victory_wrapped then
		return
	end

	main.mpgc_victory_wrapped = true
	local setters = {
		{ name = "SetVictoryPlanetLimit", key = "ifs.meta.custom.victory_total" },
		{ name = "SetVictoryBaseCapture", key = "ifs.meta.custom.victory_base" },
		{ name = "SetVictoryTurnLimit", key = "ifs.meta.custom.victory_turns_val", default = 10 },
		{ name = "SetVictoryResourceLimit", key = "ifs.meta.custom.victory_credits_val", default = 1000 },
	}
	for _, setter in ipairs(setters) do
		local previous, key, default = main[setter.name], setter.key, setter.default
		local planets = setter.name == "SetVictoryPlanetLimit"
		main[setter.name] = function(this, ...)
			local result = net_menu_results(previous(this, unpack(arg, 1, arg.n)))

			-- Turn/resource setters invoke the planet fallback first; publish the final outer objective last.
			if default then
				this.mpgc_victoryCondition = ScriptCB_ununicode(
					ScriptCB_usprintf(key, ScriptCB_tounicode(arg[1] or default)))
			elseif planets and arg[1] then
				this.mpgc_victoryCondition = "Capture " .. arg[1] .. " planets"
			else
				this.mpgc_victoryCondition = ScriptCB_ununicode(ScriptCB_getlocalizestr(key))
			end

			return unpack(result, 1, result.n)
		end
	end
end

-- Return the selected GC player limit, applying the campaign's faction cap.
function ifs_freeform_net_fnPlayerLimit(preferences, cooperative)
	local selected = ifs_instant_options.mpgc_players or preferences.iNumPlayers
	return math.max(2, math.min(selected, cooperative and 8 or 16))
end

-- Describe both factions, the effective multiplayer difficulty and the loaded campaign's objective.
function ifs_freeform_net_fnHostSummary(main, cooperative, faction)
	local factions = {}
	for team = 1, 2 do
		-- LoadState initializes faction codes; scene Setup creates teamName after hosting.
		factions[team] = ScriptCB_ununicode(
			ScriptCB_getlocalizestr("common.sides." .. main.teamCode[team] .. ".name"))
		if cooperative then
			factions[team] = factions[team] .. (team == faction and " (Human)" or " (AI)")
		end
	end

	local preferences = ScriptCB_GetNetGameDefaults()
	local difficulty_keys = { "ifs.difficulty.easy", "ifs.difficulty.medium", "ifs.difficulty.hard" }
	return {
		campaign = main.scenario,
		era = (main.teamCode[1] == "rep" or main.teamCode[1] == "cis") and "c" or "g",
		turn = main.turnNumber,
		factions = table.concat(factions, " / "),
		victoryCondition = main.mpgc_victoryCondition or "Campaign-defined objective",
		difficulty = ScriptCB_ununicode(
			ScriptCB_getlocalizestr(difficulty_keys[preferences.iDifficulty] or "ifs.difficulty.medium")),
		teamDamage = preferences.iTeamDmg > 0,
		maximum = ifs_freeform_net_fnPlayerLimit(preferences, cooperative)
	}
end

-- Discard a queued Create preview before changing source or leaving its screen.
function ifs_freeform_net_fnStopCreatePreview(this)
	this.movieName, this.movieFile, this.lastMovieFile = nil, nil, nil
	ifelem_shellscreen_fnStopMovie()
	ScriptCB_CloseMovie()
end

-- Hide gameplay settings only for GC; Session, Global and Host keep their stock callbacks.
function ifs_freeform_net_fnSettingsTabs(this, group)
	local selected = ifs_missionselect_pcMulti and ifs_missionselect_pcMulti.mpgc_selected
	for _, tag in ipairs({ "_opt_hero", "_opt_conquest", "_opt_ctf", "_opt_assault", "_opt_hunt" }) do
		ifelem_tabmanager_SetVisable(this, gPCMultiPlayerSettingsTabsLayout, tag, not selected, group)
	end
end

-- Bind the embedded list only while a stock selector callback runs; ordinary loading keeps its own state.
function ifs_freeform_net_fnWithLoadList(this, callback, ...)
	local contents, layout = ifs_freeform_load_listbox_contents, ifs_freeform_load_listbox_layout
	ifs_freeform_load_listbox_contents, ifs_freeform_load_listbox_layout = this.mpgc_contents, this.mpgc_layout
	local result = net_menu_results(pcall(callback, this.mpgc_load, unpack(arg, 1, arg.n)))

	this.mpgc_contents = ifs_freeform_load_listbox_contents
	ifs_freeform_load_listbox_contents, ifs_freeform_load_listbox_layout = contents, layout

	if not result[1] then
		error(result[2])
	end

	return unpack(result, 2, result.n)
end

-- Display load results in the existing bottom information box.
function ifs_freeform_net_fnCreateStatus(this, message)
	IFObj_fnSetVis(this.InfoboxBot.Text1, true)
	IFText_fnSetUString(this.InfoboxBot.Text1, ScriptCB_tounicode(string.sub(message or "", 1, 255)))
end

-- Refresh through the stock native file-list pre-operation, once per visit/source selection.
function ifs_freeform_net_fnShowCreateContent(this)
	IFObj_fnSetVis(this.InfoboxBot.Text1, true)
	local visible = not this.mpgc_selected
	for _, control in ipairs({
		this.MapListbox, this.ModeListbox, this.EraListbox, this.PlayListbox,
		this.mode_checkbox, this.Era_C_box, this.Era_G_box
	}) do
		IFObj_fnSetVis(control, visible)
	end

	IFObj_fnSetVis(this.map_buttons, true)
	for name, control in pairs(this.map_buttons) do
		if type(control) == "table" and name ~= "launch_btn" then
			IFObj_fnSetVis(control, visible)
		end
	end

	IFObj_fnSetVis(this.map_buttons.launch_btn, true)
	ifs_freeform_net_fnSettingsTabs(this, 2)
	ifs_missionselect_pcMulti_fnUsingNewShell(this)
	IFObj_fnSetVis(this.mpgc_create, this.mpgc_selected)

	if this.mpgc_selected then
		ifs_freeform_net_fnStopCreatePreview(this)
		IFText_fnSetString(this.InfoboxBot.Text2, "")
		IFText_fnSetString(this.InfoboxBot.Text3, "")
		if not this.mpgc_refreshed then
			this.mpgc_refreshed = true
			ifs_freeform_load.mpgc_owner, this.mpgc_operation = this, "list"
			ifs_freeform_load_StartLoadFileList()
		end
	else
		this.mpgc_refreshed = nil
	end
end

-- Decode only after saveop Exit has cleared its callbacks, then host the selected saved campaign.
function ifs_freeform_net_fnPrepareHost(this)
	this.mpgc_loaded_pending = nil
	ifs_freeform_net_fnWrapVictorySummary(ifs_freeform_main)
	ifs_freeform_main.mpgc_victoryCondition = nil
	local profiles, message = ifs_freeform_net_fnReadCampaign()
	if not profiles then
		ifs_freeform_net_fnCreateStatus(this, "Campaign could not be loaded: " .. tostring(message))
		return
	end

	local main = ifs_freeform_main
	local cooperative, faction = ifs_freeform_net_fnCampaignMode(main)
	if faction ~= 1 and faction ~= 2 then
		ifs_freeform_net_fnCreateStatus(this, "The saved campaign has no local faction.")
		return
	end

	this.mpgc_cooperative, this.mpgc_hostFaction = cooperative, faction
	this.mpgc_summary = ifs_freeform_net_fnHostSummary(main, cooperative, faction)
	this.mpgc_prepared = true
	ifs_freeform_net_fnHost(this)
end

-- Stage the authoritative pair before native lobby creation can advertise it.
function ifs_freeform_net_fnHost(this)
	if not this.mpgc_prepared then
		ifs_freeform_net_fnCreateStatus(this, "Select a saved Galactic Conquest campaign.")
		return
	end

	local preferences = ScriptCB_GetNetGameDefaults()
	preferences.PasswordStr = IFEditbox_fnGetString(this.EditPwdContainer.EditPass)
	preferences.iNumPlayers = ifs_freeform_net_fnPlayerLimit(preferences, this.mpgc_cooperative)

	-- Campaign readiness owns battle launch; a hidden stock minimum must not hold the lobby open.
	preferences.iStartCnt = 0
	this.mpgc_summary.maximum = preferences.iNumPlayers
	ScriptCB_SetNetGameDefaults(preferences)
	ScriptCB_SetGameName(IFEditbox_fnGetString(this.EditContainer.EditGameName))
	ScriptCB_SetDedicated(false)

	local hosted, message = ScriptCB_BeginMPMetagame(
		this.mpgc_cooperative, this.mpgc_hostFaction, this.mpgc_summary)
	if not hosted then
		ifs_freeform_net_fnCreateStatus(this, message)
		return
	end

	ifs_mp_gameopts.bAutoLaunch = nil
	ifs_mp_lobby_quick.mpgc_host = true
	ifs_movietrans_PushScreen(ifs_mp_lobby_quick)
end

-- Keep stock selection, native loading and the original Launch control in the Create screen.
function ifs_freeform_net_fnCreateAccept(this)
	if gShellScreen_fnDefaultInputAccept(this, 1) then
		return
	end

	if gMouseListBox == this.mpgc_create and not this.CurButton then
		this.mpgc_layout.SelectedIdx = this.mpgc_layout.CursorIdx
		ListManager_fnFillContents(this.mpgc_create, this.mpgc_contents, this.mpgc_layout)
		local selected = this.mpgc_contents[this.mpgc_layout.SelectedIdx]
		if selected then
			IFObj_fnSetVis(this.InfoboxBot.Text1, true)
			IFText_fnSetUString(this.InfoboxBot.Text1, selected.namestr)
		end
	elseif this.CurButton == "Launch" and not this.mpgc_operation then
		if not this.mpgc_contents[this.mpgc_layout.SelectedIdx] then
			return
		end

		this.mpgc_prepared, this.mpgc_summary = nil, nil
		ifs_freeform_load.mpgc_owner, this.mpgc_operation = this, "load"
		this.mpgc_load.CurButton = "accept"
		gCurEditbox = nil
		ifs_freeform_net_fnWithLoadList(this, ifs_freeform_load.Input_Accept)
	elseif this.CurButton == "_back" then
		this:Input_Back()
	end
end

-- Reuse the complete native save-row constructors across the former Map/Mode/Playlist bounds.
function ifs_freeform_net_fnBuildCreate(this)
	local left = this.MapListbox.x - this.MapListbox.width * 0.5
	local right = this.PlayListbox.x + this.PlayListbox.width * 0.5
	local w, h = right - left, this.MapListbox.height
	this.mpgc_create = NewButtonWindow {
		ScreenRelativeX = this.MapListbox.ScreenRelativeX, ScreenRelativeY = this.MapListbox.ScreenRelativeY,
		x = (left + right) * 0.5, y = this.MapListbox.y,
		width = w, height = h,
		ZPos = 200,
		titleText = "ifs.meta.load.load",
		bCreateHidden = 1,
	}

	local layout = {}
	for key, value in pairs(ifs_freeform_load_listbox_layout) do
		layout[key] = value
	end
	layout.width, layout.yTop = w - 40, nil
	layout.showcount = math.floor((h - 30) / (layout.yHeight + layout.ySpacing))
	layout.FirstShownIdx, layout.SelectedIdx = 1, 1
	ListManager_fnInitList(this.mpgc_create, layout)

	local previous_enter = this.Enter
	this.Enter = function(this, ...)
		-- Runtime-only selector data must not register a duplicate tree of native controls.
		if not this.mpgc_load then
			this.mpgc_layout, this.mpgc_contents = layout, {}
			this.mpgc_load = {
				Mode = "Load",
				listbox = this.mpgc_create,
				nogames = this.InfoboxBot.Text1,
				Helptext_Accept = this.map_buttons.launch_btn,
				acceptSound = this.acceptSound
			}
		end

		return previous_enter(this, unpack(arg, 1, arg.n))
	end

	local previous_update = this.Update
	this.Update = function(this, dt, ...)
		local function finish(...)
			if this.mpgc_selected and this.mpgc_list_pending then
				this.mpgc_list_pending = nil
				ifs_freeform_net_fnWithLoadList(this, ifs_freeform_load_fnGetSavedGameList)

				-- The stock nogames control shares this info text; restore it after stock list visibility updates.
				IFObj_fnSetVis(this.InfoboxBot.Text1, true)
				local selected = this.mpgc_contents[this.mpgc_layout.SelectedIdx]
				if selected then
					IFText_fnSetUString(this.InfoboxBot.Text1, selected.namestr)
				else
					IFText_fnSetString(this.InfoboxBot.Text1, "ifs.meta.load.nogames")
				end
			elseif this.mpgc_selected and this.mpgc_loaded_pending then
				ifs_freeform_net_fnPrepareHost(this)
			end

			return unpack(arg, 1, arg.n)
		end

		return finish(previous_update(this, dt, unpack(arg, 1, arg.n)))
	end

	local previous_exit = this.Exit
	this.Exit = function(this, bFwd, ...)
		if this.mpgc_selected then
			ifs_freeform_net_fnStopCreatePreview(this)
			if not bFwd then
				this.mpgc_refreshed = nil
			end
		end

		return previous_exit(this, bFwd, unpack(arg, 1, arg.n))
	end

	local previous_preview = ifs_missionselect_pcMulti_fnSetMapPreview
	ifs_missionselect_pcMulti_fnSetMapPreview = function(this, ...)
		if not this.mpgc_selected then
			return previous_preview(this, unpack(arg, 1, arg.n))
		end
	end
end

-- Set the native slider's whole-minute value and display Off or a number.
function ifs_freeform_net_fnRefreshTurnTimer(element, minutes)
	element.selValue = Clamp(RoundTo(minutes, 1), 0, 10)
	if element.selValue == 0 then
		element.sliderString = ScriptCB_getlocalizestr("common.off")
	else
		element.sliderString = nil
	end
end

-- Reuse stock option controls and reflow only the rows retained by GC.
function ifs_freeform_net_fnWrapOptions(this)
	local layout = ifs_io_GetLayoutFor(ifs_io_listtags.global, this)
	table.insert(layout.elements, {
		tag = "mpgc_turntimer",
		title = "Turn Timer (Minutes):",
		control = "slider",
		minValue = 0, maxValue = 10,
		selValue = 0,
		sliderMultiplier = 1,
		hidden = true,
		fnChanged = function(form, element)
			ifs_freeform_net_fnRefreshTurnTimer(element, element.selValue)
			ScriptCB_SetMetagameTurnTimer(element.selValue)
		end
	})

	-- Screen registration has not allocated native controls yet; rebuild this one form with its added row.
	Form_CreateVertical(this.screens.global, layout)
	local retained = {
		global = { aidifficulty = true, mpgc_turntimer = true },
		host = { players = true, warmup = true, teamdmg = true, shownames = true }
	}
	local ordinary_hidden = { global = {}, host = {} }
	for name, tags in pairs(retained) do
		for tag, element in pairs(this.screens[name].form.elements) do
			if type(element) == "table" then
				ordinary_hidden[name][tag] = { hidden = element.hidden }
			end
		end
	end

	-- Report whether the native options screen is editing the GC source.
	local function net_options_selected()
		return ifs_main.option_mp and ifs_missionselect_pcMulti.mpgc_selected
	end

	-- Stock defaults and the engine getter cap players before form refresh; restore GC's remembered selection.
	local previous_defaults = this.set_defaults
	this.set_defaults = function(this, ...)
		local result = net_menu_results(previous_defaults(this, unpack(arg, 1, arg.n)))
		if net_options_selected() then
			this.iMinPlayers, this.GamePrefs.iMaxPlayers = 2, 16
			this.GamePrefs.iNumPlayers = ifs_freeform_net_fnPlayerLimit(this.GamePrefs)
			this.mpgc_players = this.GamePrefs.iNumPlayers
		end

		return unpack(result, 1, result.n)
	end

	local players = this.screens.host.form.elements.players
	local ordinary_minimum = players.minValue

	-- Native Exit saves preferences before the next Enter; ordinary options must not replace the GC selection.
	local previous_push = this.push_prefs
	this.push_prefs = function(this, ...)
		if net_options_selected() then
			this.mpgc_players = this.GamePrefs.iNumPlayers
		end

		return previous_push(this, unpack(arg, 1, arg.n))
	end

	local previous_help = ifs_instant_options_fnSetHelptext
	ifs_instant_options_fnSetHelptext = function(this, tag, ...)
		if tag == "mpgc_turntimer" then
			return IFText_fnSetUString(this.InfoboxBot.Text,
				ScriptCB_tounicode("Automatically submit the current choices after 1-10 minutes per strategic turn. Off removes the turn time limit."))
		end

		return previous_help(this, tag, unpack(arg, 1, arg.n))
	end

	local previous_enter = this.Enter
	this.Enter = function(this, ...)
		local function finish(...)
			local selected = net_options_selected()
			players.minValue = selected and 2 or ordinary_minimum
			players.maxValue = this.GamePrefs.iMaxPlayers
			ifs_freeform_net_fnRefreshTurnTimer(this.screens.global.form.elements.mpgc_turntimer,
				ScriptCB_GetMetagameTurnTimer())

			for name, tags in pairs(retained) do
				local form = this.screens[name].form
				for tag, saved in pairs(ordinary_hidden[name]) do
					if selected then
						form.elements[tag].hidden = not tags[tag]
					else
						form.elements[tag].hidden = saved.hidden
					end
				end
				Form_SetValues(form)
				Form_UpdateAllElements(form)
				Form_ShowHideElements(form)
			end

			if ifs_main.option_mp then
				ifs_freeform_net_fnSettingsTabs(this, 3)
			end

			return unpack(arg, 1, arg.n)
		end

		return finish(previous_enter(this, unpack(arg, 1, arg.n)))
	end
end

-- Preserve stock saveop callbacks and cleanup, changing only the embedded selector's continuation.
function ifs_freeform_net_fnWrapLoad(this)
	local previous_list = ifs_freeform_load_LoadFileListSuccess
	ifs_freeform_load_LoadFileListSuccess = function(...)
		local owner = this.mpgc_owner
		if not owner then
			return previous_list(unpack(arg, 1, arg.n))
		end

		local from_list = this.bFromLoadFileList
		owner.mpgc_operation, owner.mpgc_list_pending, this.mpgc_owner = nil, true, nil
		local result = net_menu_results(pcall(previous_list, unpack(arg, 1, arg.n)))

		this.bFromLoadFileList = from_list

		if not result[1] then
			error(result[2])
		end

		return unpack(result, 2, result.n)
	end

	local previous_success = ifs_freeform_load_LoadMetagameSuccess
	ifs_freeform_load_LoadMetagameSuccess = function(...)
		local owner = this.mpgc_owner
		if not owner then
			return previous_success(unpack(arg, 1, arg.n))
		end

		local previous_state, from_load = SetState, this.bFromLoadDelete
		SetState = function(state, ...)
			if state == "shell" then
				owner.mpgc_loaded_pending = true
				return ScriptCB_PopScreen()
			end

			return previous_state(state, unpack(arg, 1, arg.n))
		end
		local result = net_menu_results(pcall(previous_success, unpack(arg, 1, arg.n)))

		SetState, this.bFromLoadDelete, this.mpgc_owner, owner.mpgc_operation = previous_state, from_load, nil, nil

		if not result[1] then
			error(result[2])
		end

		return unpack(result, 2, result.n)
	end

	for _, name in ipairs({ "ifs_freeform_load_LoadFileListCancel", "ifs_freeform_load_LoadMetagameCancel" }) do
		local previous_cancel = _G[name]
		_G[name] = function(...)
			local owner = this.mpgc_owner
			if not owner then
				return previous_cancel(unpack(arg, 1, arg.n))
			end

			this.mpgc_owner, owner.mpgc_operation = nil, nil
			ifs_freeform_net_fnCreateStatus(owner, "Campaign could not be loaded.")
			return ScriptCB_PopScreen()
		end
	end
end

-- Keep the original native lobby progress and update, then enter GC when its snapshot arrives.
function ifs_freeform_net_fnWrapQuickLobby(this)
	local previous_enter = this.Enter
	this.Enter = function(this, bFwd, ...)
		local host = this.mpgc_host
		this.mpgc_host = nil
		if not (host or ifs_mp_sessionlist.mpgc_selected) or not bFwd then
			return previous_enter(this, bFwd, unpack(arg, 1, arg.n))
		end

		local function finish(...)
			this.bAutoLaunch, ifs_mp_gameopts.bAutoLaunch = nil, nil
			IFText_fnSetString(this.launching, host and "Creating campaign..." or "Joining campaign...")
			if host then
				IFText_fnSetUString(Popup_Busy.title, ScriptCB_tounicode("Creating campaign..."))
			end

			ifs_freeform_net_fnBeginCampaign(host == true)
			return unpack(arg, 1, arg.n)
		end

		return finish(previous_enter(this, bFwd, unpack(arg, 1, arg.n)))
	end

	local previous_exit = this.Exit
	this.Exit = function(this, bFwd, ...)
		local function finish(...)
			if ifs_freeform_net_campaign.active and not bFwd then
				-- Stock backward Exit cancels the lobby before GC intent is cleared.
				ifs_freeform_net_fnCloseChat()
				ScriptCB_EndMPMetagame()
				ifs_freeform_net_campaign.active = false
			end

			return unpack(arg, 1, arg.n)
		end

		return finish(previous_exit(this, bFwd, unpack(arg, 1, arg.n)))
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end
