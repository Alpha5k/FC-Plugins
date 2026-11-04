ifs_freeform_net_campaign = {
	active = false
}

-- Copy the two saved selection tables before stock navigation changes their entries.
function ifs_freeform_net_fnRememberCampaign()
	local saved = { ScriptCB_LoadMetagameState() }
	local selected, fleets = {}, {}

	for team, planet in pairs(saved[4]) do
		selected[team] = planet
	end

	for team, planet in pairs(saved[5]) do
		fleets[team] = planet
	end

	ifs_freeform_net_campaign.saved = {
		profiles = saved[3],
		lastSelected = selected,
		lastFleet = fleets,
		planetNext = saved[8],
		playerTeam = saved[13],
		curScreen = saved[15],
		fleetTurn = saved[16],
		fleetStart = saved[17],
		fleetNext = saved[18],
	}
end

-- Decode through the original loader; the original main Enter creates the scene later.
function ifs_freeform_net_fnReadCampaign()
	local loaded, message = pcall(ifs_freeform_main.LoadState, ifs_freeform_main)
	if not loaded then
		return nil, tostring(message)
	end

	ifs_freeform_net_fnRememberCampaign()
	return ifs_freeform_net_campaign.saved.profiles
end

-- Use the loaded profile's faction; the current decision may belong to its opponent.
function ifs_freeform_net_fnCampaignMode(main)
	local faction = main.controllerTeam[main.startController]
	local cooperative = main.scenario ~= "custom" and main.scenario ~= "cw" and main.scenario ~= "gcw"

	for _, team in pairs(main.controllerTeam) do
		if team ~= faction then
			cooperative = false
		end
	end

	return cooperative, faction
end

-- Bind this machine's accepted faction through the original local-controller helper.
function ifs_freeform_net_fnBindFaction(faction)
	local main, controllers = ifs_freeform_main, {}
	for controller in pairs(main.controllerTeam) do
		controllers[controller] = faction
	end

	ifs_freeform_controllers(main, controllers)

	-- Controller identity fixes battle participation; the saved active turn belongs to the campaign.
	main:SetActiveTeam(main.playerTeam)

	-- The ordinary chooser Exit refreshes colors after binding controllers; accepted peers skip that chooser.
	main:InitTeamColor()
end

-- Hold the original lobby progress screen until its accepted campaign snapshot arrives.
function ifs_freeform_net_fnBeginCampaign(host)
	ifs_freeform_net_campaign.active, ifs_freeform_net_campaign.initialized, ifs_freeform_net_campaign.failed =
		true, nil, nil
	ifs_freeform_net_campaign.decision, ifs_freeform_net_campaign.requested, ifs_freeform_net_campaign.appliedSetup =
		nil, nil, nil
	ifs_freeform_net_campaign.presentation = nil
	ifs_freeform_net_campaign.heldFleet = nil
	ifs_freeform_net_campaign.diskPending, ifs_freeform_net_campaign.diskNormalize, ifs_freeform_net_campaign.saving =
		nil, nil, nil
	ifs_freeform_net_campaign.returned_session, ifs_freeform_net_campaign.shownDecision = nil, nil
	ifs_freeform_net_campaign.state = {
		phase = "joining",
		host = host,
		localFaction = 0,
		participants = {}
	}

	ScriptCB_SetMPMetagameReady()
end

-- Only the accepted Fleet Commander can recruit during the strategic decision.
function ifs_freeform_net_fnCanRecruit()
	return ifs_freeform_net_fnCanCommand()
		and ifs_freeform_net_campaign.state.decisionScreen == "ifs_freeform_fleet"
end

-- Refresh native unit models and descriptions when the current faction changes.
function ifs_freeform_net_fnRefreshUnits(previousSide)
	local this = ifs_freeform_purchase_unit

	for index, item in ipairs(this.purchaseItems) do
		if previousSide ~= this.main.playerSide and ifs_purchase_unit_table[item.itemtype] then
			this:FinishEntity(index, item)
		end

		this:UpdateColor(item)
	end

	if previousSide ~= this.main.playerSide then
		this:Select(this.purchaseItems[this.selected])
	else
		this:UpdateAction(this.purchaseItems[this.selected])
	end
end

-- Refresh the local faction's native purchase models and descriptions after accepted changes.
function ifs_freeform_net_fnRefreshPurchases(this, previousSide)
	if this == ifs_freeform_purchase_unit then
		ifs_freeform_net_fnRefreshUnits(previousSide)
	elseif this == ifs_freeform_purchase_tech then
		local team = this.main.playerTeam
		for index, item in ipairs(this.purchaseItems) do
			local tech = ifs_purchase_tech_table[index]
			IFModel_fnSetMsh(item.card, tech and tech.mesh)
			if tech then
				local rotate = ifs_purchase_tech_rotate[ifs_purchase_tech_cards[team][index]]
				if item.spin_interpolator:finish() ~= rotate then
					item.spin_interpolator = make_purchase_interpolator(
						item.spin_interpolator:value(), rotate, ifs_purchase_tech_spin_time
					)
				end
			end
		end

		for index, item in ipairs(this.useItems) do
			local using = ifs_purchase_tech_using[team][index]
			IFModel_fnSetMsh(
				item.card, using > 0 and ifs_purchase_tech_table[using].mesh or ifs_purchase_tech_empty
			)
		end

		if this.focus == ifs_purchase_tech_focus_using then
			this:UpdateActionUsing()
		else
			this:UpdateActionCarousel()
		end
	end
end

-- Apply the host's campaign update while preserving this player's camera and selection.
function ifs_freeform_net_fnRestoreCampaign(this, campaign, mission)
	local restored, message = ScriptCB_RestoreMetagameState(campaign, mission)
	if not restored then
		return nil, message
	end

	-- The original loader needs its scenario initializer before it can restore controller teams.
	local _, scenario = ScriptCB_LoadMetagameState()
	if type(_G["ifs_freeform_start_" .. tostring(scenario)]) ~= "function" then
		return nil, "Scenario '" .. tostring(scenario) .. "' is not available on this installation."
	end

	if not ifs_freeform_net_campaign.initialized then
		-- Ordinary disk-load Accept invalidates a previous local battle before entering GC.
		if not ifs_freeform_net_campaign.state.returned then
			ScriptCB_SetLastBattleVictoryValid(nil)
		end

		ifs_freeform_net_fnRememberCampaign()
		return true
	end

	local main = ifs_freeform_main
	local previousSide = main.playerSide
	local fleets = main.planetFleet
	local selected, nextPlanet = main.planetSelected, main.planetNext
	local lastSelected, lastFleet, cameraOffset = main.lastSelected, main.lastFleet, main.cameraOffset
	local previous_victory = ScriptCB_GetLastBattleVictory

	-- Snapshot loading is presentation; the setup acknowledgement applies exact bonuses once.
	ScriptCB_GetLastBattleVictory = function()
		return 0
	end

	-- Loaded fleets and undo state are shared; this viewer's selection and camera stay local.
	local loaded, error = ifs_freeform_net_fnReadCampaign()
	ScriptCB_GetLastBattleVictory = previous_victory

	if not loaded then
		return nil, error
	end

	ifs_freeform_net_fnRestoreFleets(fleets)
	main.lastSelected, main.lastFleet, main.cameraOffset = lastSelected, lastFleet, cameraOffset
	main.planetSelected, main.planetNext = selected, nextPlanet

	local state = ifs_freeform_net_campaign.state
	ifs_freeform_net_fnBindFaction(state.localFaction > 0 and state.localFaction or state.hostFaction)
	ifs_freeform_net_fnSetViewFaction()
	ifs_freeform_net_campaign.refreshSide, ifs_freeform_net_campaign.refreshPending = previousSide, true

	return true
end

-- Keep normal native cancellation on the shell thread before clearing GC intent.
function ifs_freeform_net_fnLeave()
	ifs_freeform_net_fnFinishDiskSave()
	ifs_freeform_net_fnCloseChat()
	ScriptCB_CancelLobby()
	ScriptCB_CancelSessionList()
	ScriptCB_EndMPMetagame()
	ifs_freeform_net_campaign.active = false
	ScriptCB_ClearCampaignState()
	ScriptCB_ClearMetagameState()
	ScriptCB_ClearMissionSetup()
	ScriptCB_SetGameRules("instantaction")

	-- Restart the native shell after clearing the session and campaign state.
	Popup_Busy:fnActivate(nil)
	ScriptCB_SetInNetGame(nil)
	ifs_mp_sessionlist.bForceRefresh = 1
	SetState("shell")
end

-- Present a terminal native/load error through the original modal shell popup, then leave.
function ifs_freeform_net_fnSessionError(message)
	if ifs_freeform_net_campaign.failed then
		return
	end

	ifs_freeform_net_campaign.failed = true
	ifs_freeform_sides.displayTimer = nil
	ifs_freeform_net_fnCloseChat()
	Popup_Busy:fnActivate(nil)
	Popup_Ok.fnDone = ifs_freeform_net_fnLeave
	Popup_Ok:fnActivate(1)
	gPopup_fnSetTitleUStr(Popup_Ok, ScriptCB_tounicode(message))
end

-- Service the held native lobby and consume copied state on the original active screen's update.
function ifs_freeform_net_fnPoll(this)
	if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.failed then
		return
	end

	if this ~= ifs_mp_lobby_quick and this ~= ifs_mp_lobby then
		ScriptCB_UpdateLobby(nil)
	end

	local state = ScriptCB_UpdateMPMetagame()
	if not state then
		return
	end

	-- Campaign entry and addon readiness queue control-thread work; an initial closed/joining poll is not an accepted session.
	if this == ifs_mp_lobby_quick and not ifs_freeform_net_campaign.initialized
		and (state.phase == "closed" or state.phase == "joining") then
		return
	end

	ifs_freeform_net_campaign.state = state
	if state.phase == "closed" or state.phase == "error" then
		ifs_freeform_net_fnSessionError(state.message or "The campaign session ended.")
		return
	end

	if state.campaign then
		local applied, message = ifs_freeform_net_fnRestoreCampaign(this, state.campaign, state.mission)
		if not applied then
			ifs_freeform_net_fnSessionError("Campaign could not be loaded: " .. tostring(message))
			return
		end

		if not ifs_freeform_net_campaign.initialized and this == ifs_mp_lobby_quick then
			Popup_Busy:fnActivate(nil)
			ScriptCB_PushScreen("ifs_freeform_main")
		end
	end

	ifs_freeform_net_fnPollBattle(this, state)

	if this == ifs_freeform_sides and state.phase == "choosing" and state.message then
		IFText_fnSetUString(this.title.text, ScriptCB_tounicode(state.message))
		this.displayTimer, this.mpgc_requested = nil, nil
		this:SetPlayerReady(ifs_freeform_main.startController, 0)
	end
end
