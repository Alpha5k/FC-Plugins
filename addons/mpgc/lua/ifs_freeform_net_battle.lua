-- Resume a retained native session before stock shell saved-state routing reaches main.
function ifs_freeform_net_fnResumeCampaign()
	if ifs_freeform_net_campaign.active then
		return
	end

	local state = ScriptCB_UpdateMPMetagame(true)
	if not state or not state.returned then
		return
	end

	ifs_freeform_net_campaign.active, ifs_freeform_net_campaign.state = true, state
	ifs_freeform_net_campaign.returned_session = true

	-- Existing membership survives; readiness belongs to this fresh shell VM.
	ScriptCB_SetMPMetagameReady()
end

-- Process action results, prepare accepted battle setup and follow commander changes.
function ifs_freeform_net_fnPollBattle(this, state)
	if state.actionDone then
		local request = ifs_freeform_net_campaign.requested
		ifs_freeform_net_campaign.requested = nil
		if request then
			if state.actionAccepted and request.name == "choose_bonus" then
				request.screen.mpgc_acceptedDecision = request.decisionId
				request.screen.mpgc_acceptedBonus = request.bonus
			end

			ifs_freeform_net_fnActionFeedback(request, state.actionAccepted)
		end

		if state.actionMessage and state.actionMessage ~= "" then
			Popup_Ok.fnDone = nil
			Popup_Ok:fnActivate(1)
			gPopup_fnSetTitleUStr(Popup_Ok, ScriptCB_tounicode(state.actionMessage))
		end
	end

	if state.host and state.action then
		local action = state.action
		local accepted, message = ifs_freeform_net_fnApplyAction(action)
		ScriptCB_CompleteMetagameAction(action.token, accepted == true, message or "")
		if accepted then
			ifs_freeform_net_campaign.refreshPending = true
			ifs_freeform_net_fnPublishDecision()
		end
	end

	if state.battleSetup and ifs_freeform_net_campaign.appliedSetup ~= state.setupId then
		local main = ifs_freeform_main
		if not state.host then
			-- Use the mission and bonuses already restored from the accepted snapshot.
			ScriptCB_SetMissionNames(main.launchMission, nil)
			local side = main.teamCode[state.localFaction]
			for controller in pairs(main.controllerTeam) do
				ScriptCB_MetagameSetSide(main.controllerPlayer[controller], side)
			end

			for team = 1, 2 do
				local bonus = main.activeBonus[team]
				if bonus then
					ActivateBonus(team, "team_bonus_" .. bonus)
				end
			end
		end

		local ready, message = ScriptCB_SetMetagameBattleReady(state.setupId, main.teamCode[1], main.teamCode[2])
		if not ready then
			ifs_freeform_net_fnSessionError(message)
			return
		end

		ifs_freeform_net_campaign.appliedSetup = state.setupId
	end

	if state.host and state.launch then
		ScriptCB_LaunchLobby()
	end

	local decision = ifs_freeform_net_campaign.decision
	if state.host and decision and state.commanders then
		local team = decision.playerTeam
		if decision.screen == "ifs_freeform_ai" and state.commanders[team] >= 0 then
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_fleet")
			ifs_freeform_net_fnPublishDecision()
		elseif decision.screen == "ifs_freeform_fleet" and state.commanders[team] < 0
			and not (decision.move and decision.move.remaining) and not decision.presentation then
			if decision.pending then
				ifs_freeform_net_fnApplyAction({
					name = "cancel_card",
					faction = team
				})
			end

			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_ai")
			ifs_freeform_net_fnPublishDecision()
		elseif decision.screen == "ifs_freeform_battle_mode" and state.commanders[decision.decisionFaction] < 0 then
			ifs_freeform_net_fnAdvanceDecision(decision.screen)
			ifs_freeform_net_fnPublishDecision()
		elseif decision.screen == "ifs_freeform_battle" or decision.screen == "ifs_freeform_result" then
			local faction = ifs_freeform_net_fnDecisionFaction(decision)
			if faction ~= decision.decisionFaction then
				decision.decisionFaction = faction
				ifs_freeform_net_fnPublishDecision()
			end
		end
	end

	ifs_freeform_net_fnSyncPresentation(state, this)
	ifs_freeform_net_fnShowDecision(this)
end
