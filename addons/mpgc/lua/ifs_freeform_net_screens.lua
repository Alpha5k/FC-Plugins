-- Retain optional nil return slots while a temporary native callback is restored.
local function net_screen_results(...) return arg end

-- Keep the connected lobby running while native save dialogs own the screen.
function ifs_freeform_net_fnWrapSaveUpdate(this)
	if this.mpgc_save_update then
		return
	end

	this.mpgc_save_update = true
	local previous_update = this.Update
	this.Update = function(this, ...)
		local dt = arg[1] or 0
		local result = net_screen_results(previous_update(this, unpack(arg, 1, arg.n)))

		if ifs_freeform_net_campaign.active then
			if ifs_freeform_net_campaign.initialized and not ifs_freeform_net_campaign.diskPending then
				ifs_freeform_net_campaign.saving = true
				local serviced = net_screen_results(pcall(function()
					ifs_freeform_net_fnPoll(this)
					ifs_freeform_net_fnUpdateDecision(dt)
				end))

				ifs_freeform_net_campaign.saving = nil

				if not serviced[1] then
					error(serviced[2])
				end
			else
				ScriptCB_UpdateLobby(nil)
			end
		end

		return unpack(result, 1, result.n)
	end
end

-- Restore live campaign buffers after the native save queue releases its borrowed source.
function ifs_freeform_net_fnFinishDiskSave()
	if not ifs_freeform_net_campaign.diskPending then
		return
	end

	ifs_freeform_net_campaign.diskPending = nil
	if ifs_freeform_net_campaign.active and ifs_freeform_net_campaign.decision then
		ifs_freeform_main:SaveState()
	end
end

-- Keep the disk buffer stable until native completion while the native session deadlines continue running.
function ifs_freeform_net_fnWrapDiskSave()
	local previous_start, previous_done = ScriptCB_StartSaveMetagame, ifs_saveop_SaveMetagameDone
	ScriptCB_StartSaveMetagame = function(...)
		if not ifs_freeform_net_campaign.active or not ifs_freeform_net_campaign.state.host then
			return previous_start(unpack(arg, 1, arg.n))
		end

		ifs_freeform_net_campaign.diskNormalize = true
		local serialized = net_screen_results(pcall(ifs_freeform_main.SaveState, ifs_freeform_main))
		ifs_freeform_net_campaign.diskNormalize = nil

		if not serialized[1] then
			error(serialized[2])
		end

		ifs_freeform_net_campaign.diskPending = true
		local result = net_screen_results(pcall(previous_start, unpack(arg, 1, arg.n)))

		if not result[1] then
			ifs_freeform_net_fnFinishDiskSave()
			error(result[2])
		end

		table.remove(result, 1)
		return unpack(result, 1, result.n - 1)
	end

	ifs_saveop_SaveMetagameDone = function(...)
		ifs_freeform_net_fnFinishDiskSave()
		return previous_done(unpack(arg, 1, arg.n))
	end
end

-- Leave the whole GC stack after a native session error; the stock default pops only one screen.
function ifs_freeform_net_fnWrapSessionError(this)
	local previous_error = this.fnPostError
	this.fnPostError = function(this, accepted, level, message, ...)
		if ifs_freeform_net_campaign.active and level >= 6 and level <= 12 then
			return ifs_freeform_net_fnLeave()
		end
		return previous_error(this, accepted, level, message, unpack(arg, 1, arg.n))
	end
end

-- Keep the original callback and return values while servicing the connected campaign.
function ifs_freeform_net_fnWrapSessionUpdate(this)
	ifs_freeform_net_fnWrapSessionError(this)

	local previous_update = this.Update
	this.Update = function(this, ...)
		if ifs_freeform_net_campaign.executing then
			return previous_update(this, unpack(arg, 1, arg.n))
		end

		local dt = arg[1]
		local result
		-- Render the current view, then poll the session and advance the host decision once for this frame.
		if ifs_freeform_net_campaign.active and ifs_freeform_net_fnUpdatePresentation(this, dt) then
			-- Committed presentation supplies the stock drawing pass itself.
			result = net_screen_results()
		else
			result = net_screen_results(previous_update(this, unpack(arg, 1, arg.n)))
		end

		ifs_freeform_net_fnPoll(this)
		if ifs_freeform_net_campaign.active then
			ifs_freeform_net_fnUpdateDecision(dt)
		end
		if this ~= ifs_mp_lobby_quick then
			ifs_freeform_net_fnUpdateChat(this, dt)
		end

		return unpack(result, 1, result.n)
	end
end

-- Reuse main entry and saving; share faction colors between ordinary and multiplayer GC.
function ifs_freeform_net_fnWrapCampaign(this)
	local previous_colors = this.InitTeamColor
	local faction_colors = {
		rep = { r = 80, g = 160, b = 255 },
		cis = { r = 230, g = 180, b = 50 },
		all = { r = 80, g = 230, b = 110 },
		imp = { r = 245, g = 75, b = 75 },
	}
	this.InitTeamColor = function(this, ...)
		local result = net_screen_results(previous_colors(this, unpack(arg, 1, arg.n)))

		-- Fill the stock palette even when a custom getter wraps it for ordinary campaigns.
		-- Keep that getter so custom campaigns can still supply their own colors.
		for team = 1, 2 do
			local color = faction_colors[this.teamCode[team]]
			if color then
				for viewer = 1, 2 do
					this.teamColor[viewer][team] = color
				end
			end
		end

		return unpack(result, 1, result.n)
	end

	ifs_freeform_net_fnWrapFleetDrawing(this)
	ifs_freeform_net_fnWrapSessionUpdate(this)

	local previous_player = this.UpdatePlayerText
	this.UpdatePlayerText = function(this, player, ...)
		local result = net_screen_results(previous_player(this, player, unpack(arg, 1, arg.n)))
		if ifs_freeform_net_campaign.active then
			ifs_freeform_net_fnUpdateHud(player)
		elseif player and player.mpgc_turn then
			IFObj_fnSetVis(player.mpgc_turn, false)
			if player.mpgc_timer then
				IFObj_fnSetVis(player.mpgc_timer, false)
			end
		end

		return unpack(result, 1, result.n)
	end

	local entry_screens = {
		ifs_freeform_fleet = true,
		ifs_freeform_sides = true,
		ifs_freeform_ai = true,
		ifs_freeform_result = true,
		ifs_freeform_summary = true,
		ifs_freeform_battle = true,
		ifs_freeform_battle_mode = true,
		ifs_freeform_battle_card = true,
		ifs_freeform_end = true,
	}
	local previous_enter = this.Enter
	this.Enter = function(this, bFwd, ...)
		ifs_freeform_net_fnResumeCampaign()
		if not ifs_freeform_net_campaign.active or not bFwd then
			return previous_enter(this, bFwd, unpack(arg, 1, arg.n))
		end

		local previous_push, previous_victory = ScriptCB_PushScreen, ScriptCB_GetLastBattleVictory
		local previous_init, setup_sides = this.OneTimeInit, this.setupSides
		local resume
		this.OneTimeInit = function(this, ...)
			local initialized = net_screen_results(previous_init(this, unpack(arg, 1, arg.n)))
			setup_sides = this.setupSides
			-- Players already assigned to a faction can continue from the saved campaign step.
			if ifs_freeform_net_campaign.state.localFaction > 0 then
				this.setupSides = nil
			end
			return unpack(initialized)
		end

		-- Create the original scene before executing its saved decision on the host.
		ScriptCB_PushScreen = function(name, ...)
			if entry_screens[name] then
				resume = name
			else
				return previous_push(name, unpack(arg, 1, arg.n))
			end
		end

		-- Only the host's first entry applies the battle result; clients and later entries show restored state.
		if not ifs_freeform_net_campaign.state.host or ifs_freeform_net_campaign.initialized then
			ScriptCB_GetLastBattleVictory = function() return 0 end
		end
		local result = net_screen_results(pcall(previous_enter, this, bFwd, unpack(arg, 1, arg.n)))

		ScriptCB_PushScreen, ScriptCB_GetLastBattleVictory = previous_push, previous_victory
		this.OneTimeInit, this.setupSides = previous_init, setup_sides

		if not result[1] then
			error(result[2])
		end

		ifs_freeform_net_fnRememberCampaign()
		ifs_freeform_net_campaign.initialized = true
		local state = ifs_freeform_net_campaign.state
		ifs_freeform_net_fnBindFaction(state.localFaction > 0 and state.localFaction or state.hostFaction)
		if state.host then
			ScriptCB_SetMetagameSummary({ map = "Galaxy Map" })
		end
		if state.phase ~= "choosing" then
			ifs_freeform_net_fnStartDecision(resume or "ifs_freeform_fleet")
		end
		ifs_freeform_net_fnSetViewFaction()
		previous_push(state.phase == "choosing" and "ifs_freeform_sides" or "ifs_freeform_fleet")
		return unpack(result, 2, result.n)
	end

	local previous_turn, previous_result = this.NextTurn, this.ApplyBattleResult
	this.NextTurn = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
			return
		end
		return previous_turn(this, unpack(arg, 1, arg.n))
	end

	this.ApplyBattleResult = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.state.host then
			return
		end
		return previous_result(this, unpack(arg, 1, arg.n))
	end

	local previous_setup = this.SaveMissionSetup
	this.SaveMissionSetup = function(this, ...)
		if not ifs_freeform_net_campaign.active then
			return previous_setup(this, unpack(arg, 1, arg.n))
		end

		local prefs, get_difficulty = ScriptCB_GetNetGameDefaults(), ScriptCB_GetDifficulty
		-- Stock GC reads profile difficulty last; retain the host's agreed multiplayer difficulty.
		ScriptCB_GetDifficulty = function() return prefs.iDifficulty end
		local result = net_screen_results(pcall(previous_setup, this, unpack(arg, 1, arg.n)))

		ScriptCB_GetDifficulty = get_difficulty

		if not result[1] then
			error(result[2])
		end

		return unpack(result, 2, result.n)
	end

	local previous_save = this.SaveState
	this.SaveState = function(this, ...)
		if not ifs_freeform_net_campaign.active then
			return previous_save(this, unpack(arg, 1, arg.n))
		end
		if not ifs_freeform_net_campaign.state.host then
			return
		end
		return ifs_freeform_net_fnSaveDecision(previous_save, this, unpack(arg, 1, arg.n))
	end

	local previous_prompt = this.PromptSave
	this.PromptSave = function(this, ...)
		-- Stock battle return queues a save for later Fleet entry; multiplayer saves require an explicit request.
		if ifs_freeform_net_campaign.active and not arg[1] then
			this.requestSave = nil
			return false
		end

		-- Open native save prompts from the visible screen after campaign rules finish.
		if ifs_freeform_net_campaign.active and ifs_freeform_net_campaign.executing then
			return false
		end

		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.state.host then
			-- Clients can save dirty profiles through stock logic, but never request a campaign save.
			local request = this.requestSave
			this.requestSave, arg[1] = nil, nil
			local result = net_screen_results(pcall(previous_prompt, this, unpack(arg, 1, arg.n)))

			this.requestSave = request

			if not result[1] then
				error(result[2])
			end

			return unpack(result, 2, result.n)
		end

		if ifs_freeform_net_campaign.active and ifs_freeform_net_campaign.decision
			and ifs_freeform_net_campaign.decision.pending
			and (arg[1] or this.requestSave) then
			-- Disk GC saves omit an unplaced purchase; use its original refund before saving.
			ifs_freeform_net_fnApplyAction({
				name = "cancel_card",
				faction = ifs_freeform_net_campaign.decision.decisionFaction,
			})
			ifs_freeform_net_fnPublishDecision()
		end
		return previous_prompt(this, unpack(arg, 1, arg.n))
	end
end

-- Keep the original local chooser; native acceptance decides when its original countdown can finish.
function ifs_freeform_net_fnWrapSides(this)
	ifs_freeform_net_fnWrapSessionError(this)

	local width = ScriptCB_GetSafeScreenInfo() * 0.5
	this.mpgc_players = NewIFContainer {
		ScreenRelativeX = 0,
		ScreenRelativeY = 0,
		y = 230,
		bCreateHidden = 1
	}
	for faction = 1, 2 do
		local column = NewIFContainer { x = (faction - 1) * width + 8 }
		column.count = NewIFText {
			x = 0,
			textw = width - 16,
			texth = 16,
			font = "gamefont_small",
			halign = "hcenter",
			nocreatebackground = 1
		}
		for index = 1, 8 do
			column[index] = NewIFText {
				x = 0,
				y = index * 16,
				textw = width - 16,
				texth = 16,
				font = "gamefont_small",
				halign = "hcenter",
				nocreatebackground = 1
			}
		end
		this.mpgc_players[faction] = column
	end

	-- Remote participants are display data; the original chooser still owns local controllers.
	local function update_players()
		IFObj_fnSetVis(this.mpgc_players, ifs_freeform_net_campaign.active)
		if not ifs_freeform_net_campaign.active then
			return
		end

		local names = { {}, {} }
		for _, participant in ipairs(ifs_freeform_net_campaign.state.participants or {}) do
			if participant.faction == 1 or participant.faction == 2 then
				table.insert(names[participant.faction], participant.name .. (participant.host and " (Host)" or ""))
			end
		end

		for faction = 1, 2 do
			local column = this.mpgc_players[faction]
			IFText_fnSetUString(column.count,
				ScriptCB_tounicode(string.format("Players: %d/8", table.getn(names[faction]))))
			for index = 1, 8 do
				IFText_fnSetUString(column[index], ScriptCB_tounicode(names[faction][index] or ""))
			end
		end
	end

	local previous_enter = this.Enter
	this.Enter = function(this, ...)
		local function finish(...)
			if ifs_freeform_net_campaign.active then
				this.displayTimer, this.mpgc_requested = nil, nil
				local name = ScriptCB_tounicode(
					ifs_freeform_net_fnParticipantName(ifs_freeform_net_campaign.state.localSlot))
				for controller in pairs(this.controllerTeam) do
					IFText_fnSetUString(this.players[controller], name)
				end
			end
			update_players()
			return unpack(arg, 1, arg.n)
		end
		return finish(previous_enter(this, unpack(arg, 1, arg.n)))
	end

	local previous_start = this.Input_Start
	this.Input_Start = function(this, joystick, ...)
		if ifs_freeform_net_campaign.active then
			return ifs_freeform_fleet.Input_Start(this, joystick, unpack(arg, 1, arg.n))
		end
		if previous_start then
			return previous_start(this, joystick, unpack(arg, 1, arg.n))
		end
	end

	local previous_accept = this.Input_Accept
	this.Input_Accept = function(this, joystick, ...)
		if not ifs_freeform_net_campaign.active then
			return previous_accept(this, joystick, unpack(arg, 1, arg.n))
		end
		if this.mpgc_requested then
			return
		end

		local controller = ifs_freeform_main.startController
		local requested, message = ScriptCB_SetMPMetagameTeam(this.controllerTeam[controller])
		if requested then
			previous_accept(this, controller, unpack(arg, 1, arg.n))
			this.displayTimer, this.mpgc_requested = nil, true
			IFText_fnSetUString(this.title.text, ScriptCB_tounicode("Waiting for the host..."))
		else
			IFText_fnSetUString(this.title.text, ScriptCB_tounicode(message))
		end
	end

	local previous_update = this.Update
	this.Update = function(this, dt, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.failed then
			ifs_freeform_net_fnPoll(this)
			local state = ifs_freeform_net_campaign.state
			if state.phase == "strategic" and state.localFaction > 0 and not this.displayTimer then
				this:SetPlayerTeam(ifs_freeform_main.startController, state.localFaction)
				previous_accept(this, ifs_freeform_main.startController)
			end
		end

		local function finish(...)
			update_players()
			ifs_freeform_net_fnUpdateChat(this, dt)
			return unpack(arg, 1, arg.n)
		end
		return finish(previous_update(this, dt, unpack(arg, 1, arg.n)))
	end

	local previous_exit = this.Exit
	this.Exit = function(this, ...)
		local function finish(...)
			if ifs_freeform_net_campaign.active and ifs_freeform_net_campaign.state.localFaction > 0 then
				ifs_freeform_net_fnBindFaction(ifs_freeform_net_campaign.state.localFaction)
				if ifs_freeform_net_campaign.state.host and not ifs_freeform_net_campaign.decision then
					ifs_freeform_net_fnStartDecision(
						ifs_freeform_main.joystick and "ifs_freeform_fleet" or "ifs_freeform_ai")
				end
				ifs_freeform_net_fnSetViewFaction()
			end
			return unpack(arg, 1, arg.n)
		end
		return finish(previous_exit(this, unpack(arg, 1, arg.n)))
	end
end

-- Preserve the ordinary pause menu and host save/discard flow, then cancel the session on quit.
function ifs_freeform_net_fnWrapCampaignMenu(this)
	local previous_quit = ifs_freeform_menu_fnQuitPopupDone
	ifs_freeform_menu_fnQuitPopupDone = function(result, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.state.host and result then
			return ifs_freeform_net_fnLeave()
		end
		return previous_quit(result, unpack(arg, 1, arg.n))
	end

	local previous_enter = this.Enter
	this.Enter = function(this, bFwd, ...)
		if ifs_freeform_net_campaign.active and not bFwd and this.QuitRequested then
			if ifs_freeform_net_campaign.returned_session then
				return ifs_freeform_net_fnLeave()
			end
			ScriptCB_CancelLobby()
			ScriptCB_CancelSessionList()
			ScriptCB_EndMPMetagame()
			ifs_freeform_net_campaign.active = false
		end

		-- Restore only values owned by the previous GC entry before stock chooses its normal rows.
		if this.mpgc_hidden then
			for name, value in pairs(this.mpgc_hidden) do
				this.buttons[name].hidden = value.hidden
			end
			this.mpgc_hidden = nil
		end

		local function finish(...)
			if ifs_freeform_net_campaign.active then
				this.mpgc_hidden = {}
				for _, name in ipairs({ "load", "cheat", "save" }) do
					this.mpgc_hidden[name] = { hidden = this.buttons[name].hidden }
				end
				this.buttons.load.hidden, this.buttons.cheat.hidden = true, true
				this.buttons.save.hidden = not ifs_freeform_net_campaign.state.host
				this.CurButton = ShowHideVerticalButtons(this.buttons, ifsfreeformmenu_vbutton_layout)
			end
			return unpack(arg, 1, arg.n)
		end
		return finish(previous_enter(this, bFwd, unpack(arg, 1, arg.n)))
	end

	local previous_accept = this.Input_Accept
	this.Input_Accept = function(this, ...)
		if ifs_freeform_net_campaign.active and (this.CurButton == "load" or this.CurButton == "cheat"
			or (this.CurButton == "save" and not ifs_freeform_net_campaign.state.host)) then
			return
		end
		return previous_accept(this, unpack(arg, 1, arg.n))
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end

-- Show the native victory movie once on each machine and leave through the connected-session cleanup.
function ifs_freeform_net_fnWrapCampaignEnd(this)
	local previous_enter, previous_done = this.Enter, this.Done
	this.Enter = function(this, ...)
		if ifs_freeform_net_campaign.active and ifs_freeform_main.CheckVictory then
			ifs_freeform_main.teamVictory = ifs_freeform_main:CheckVictory()
		end
		return previous_enter(this, unpack(arg, 1, arg.n))
	end

	this.Done = function(this, ...)
		if ifs_freeform_net_campaign.active then
			if this.moviePlaying then
				ScriptCB_StopMovie()
				ScriptCB_SndBusFade("shellfx", 0.0, 1.0)
			end
			gMovieAlwaysPlay = nil
			return ifs_freeform_net_fnLeave()
		end
		return previous_done(this, unpack(arg, 1, arg.n))
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end

-- Adapt the existing native player-list controls; retain their ordinary geometry for later sessions.
function ifs_freeform_net_fnPlayerListColumns(this, columns)
	local width = lobby_listbox_layout.width

	-- Fit GC columns or restore the row's saved geometry.
	local function update_row(row, geometry)
		for name, saved in pairs(geometry) do
			local field = row[name]
			local x, w = saved.x, saved.width
			if ifs_freeform_net_campaign.active then
				if name == "namefield" then
					x, w = 10, width * 0.55 - 10
				elseif name == "teamfield" then
					x, w = width * 0.55, width * 0.25
				elseif name == "pingfield" then
					x, w = width * 0.80, width * 0.20 - 10
				end
			end

			IFObj_fnSetPos(field, x, saved.y)
			IFText_fnSetTextBox(field, w, saved.height)
			if name == "killsfield" then
				IFObj_fnSetVis(field, not ifs_freeform_net_campaign.active)
			end
		end
	end

	update_row(this.columnheaders, columns.headers)
	for index = 1, lobby_listbox_layout.showcount do
		update_row(this.listbox[index], columns[index])
	end
end

-- Open the original native roster with its existing-lobby refresh, keeping the GC session on return.
function ifs_freeform_net_fnWrapPlayerList(this)
	local columns = {}

	-- Copy row positions and text-box dimensions before native registration consumes the geometry fields.
	local function remember(row)
		local geometry = {}
		for _, name in ipairs({ "namefield", "teamfield", "pingfield", "killsfield" }) do
			local field = row[name]
			if field then
				geometry[name] = {
					x = field.x,
					y = field.y,
					width = field.textw,
					height = field.texth
				}
			end
		end
		return geometry
	end

	local previous_populate = lobby_listbox_layout.PopulateFn
	lobby_listbox_layout.PopulateFn = function(row, data, ...)
		local function finish(...)
			if ifs_freeform_net_campaign.active then
				if data and not gBlankListbox and row.teamfield then
					local team = ""
					-- Shell team flags lack mission Team names; accepted slots identify the campaign faction.
					for _, player in ipairs(ifs_freeform_net_campaign.state.participants or {}) do
						if player.slot == tonumber(data.indexstr)
							and (player.faction == 1 or player.faction == 2) then
							team = ifs_freeform_main.teamName[player.faction]
							IFObj_fnSetColor(row.teamfield, ifs_freeform_main:GetTeamColor(player.faction))
						end
					end
					IFText_fnSetString(row.teamfield, team)
				end

				for _, name in ipairs({ "killsfield", "StateIcon" }) do
					if row[name] then
						IFObj_fnSetVis(row[name], nil)
					end
				end
			end
			return unpack(arg, 1, arg.n)
		end
		return finish(previous_populate(row, data, unpack(arg, 1, arg.n)))
	end

	columns.headers = remember(this.columnheaders)
	for index = 1, lobby_listbox_layout.showcount do
		columns[index] = remember(this.listbox[index])
	end

	local previous_enter, previous_exit = this.Enter, this.Exit
	this.Enter = function(this, bFwd, ...)
		local function finish(...)
			ifs_freeform_net_fnPlayerListColumns(this, columns)
			return unpack(arg, 1, arg.n)
		end
		if not ifs_freeform_net_campaign.active then
			return finish(previous_enter(this, bFwd, unpack(arg, 1, arg.n)))
		end
		this.bAutoLaunch = nil
		return finish(previous_enter(this, false, unpack(arg, 1, arg.n)))
	end

	this.Exit = function(this, bFwd, ...)
		return previous_exit(this, ifs_freeform_net_campaign.active or bFwd, unpack(arg, 1, arg.n))
	end

	local previous_back = this.Input_Back
	this.Input_Back = function(this, ...)
		if ifs_freeform_net_campaign.active then
			return ScriptCB_PopScreen()
		end
		return previous_back(this, unpack(arg, 1, arg.n))
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end
